#include <Heimdall/Lexer.hpp>
#include <Heimdall/LineTable.hpp>
#include <Heimdall/Preprocessor.hpp>
#include <Heimdall/SemanticAnalyzer.hpp>

#include <array>
#include <string_view>
#include <utility>

namespace heimdall
{

    namespace
    {

        bool IsWord(std::string_view value)
        {
            if (value.empty())
            {
                return false;
            }

            const char first = value.front();
            return (first >= 'a' && first <= 'z') ||(first >= 'A' && first <= 'Z') || first == '_';
        }

        std::vector<std::string_view> SignificantTokens(std::string_view source)
        {
            std::vector<std::string_view> result;
            for (const auto& token : Lexer(source).Lex())
            {
                if (token.kind != TokenKind::Whitespace && token.kind != TokenKind::LineComment &&
                    token.kind != TokenKind::BlockComment)
                {
                    result.push_back(source.substr(token.offset, token.length));
                }
            }

            return result;
        }

        struct SignificantToken
        {
            std::string_view text;
            TokenKind kind;
            std::size_t offset;
        };

        // Non-owning preprocessor over the command's defines: avoids the deep copy
        // of the macro map per call. A filtered copy is built only when the command
        // carries -U undefines.
        Preprocessor MakePreprocessor(const CompileCommand* command)
        {
            static const Preprocessor::MacroMap kEmpty;
            if (command == nullptr)
            {
                return Preprocessor(kEmpty);
            }

            if (command->undefines.empty())
            {
                return Preprocessor(command->defines);
            }

            Preprocessor::MacroMap filtered = command->defines;
            for (const auto& name : command->undefines)
            {
                filtered.erase(name);
            }

            return Preprocessor(std::make_shared<const Preprocessor::MacroMap>(std::move(filtered)));
        }

        std::vector<std::string_view> SignificantViews(std::string_view source,
            const std::vector<Token>& tokens)
        {
            std::vector<std::string_view> result;
            result.reserve(tokens.size());
            for (const auto& token : tokens)
            {
                if (token.kind != TokenKind::Whitespace && token.kind != TokenKind::LineComment &&
                    token.kind != TokenKind::BlockComment)
                {
                    result.push_back(source.substr(token.offset, token.length));
                }
            }

            return result;
        }

        bool IsModifier(std::string_view token)
        {
            return token == "const" || token == "volatile" || token == "static" || token == "constexpr" ||
                token == "register" || token == "thread_local";
        }

        bool IsControlKeyword(std::string_view token)
        {
            return token == "if" || token == "for" || token == "while" || token == "switch" || token == "catch";
        }

        bool IsClassBody(const std::vector<SignificantToken>& tokens, std::size_t open_brace)
        {
            for (std::size_t i = open_brace; i > 0; --i)
            {
                const auto token = tokens[i - 1].text;
                if (token == ";" || token == "{" || token == "}")
                {
                    break;
                }

                if (token == "class" || token == "struct" || token == "union")
                {
                    return true;
                }
            }

            return false;
        }

        bool IsFunctionBody(const std::vector<SignificantToken>& tokens, std::size_t open_brace)
        {
            if (open_brace == 0 || tokens[open_brace - 1].text != ")")
            {
                return false;
            }

            std::size_t depth = 0;
            for (std::size_t i = open_brace; i > 0; --i)
            {
                const auto token = tokens[i - 1].text;
                if (token == ")")
                {
                    ++depth;
                }
                else if (token == "(" && --depth == 0)
                {
                    return i >= 2 && !IsControlKeyword(tokens[i - 2].text);
                }
            }

            return false;
        }

    } // namespace

    std::unordered_set<std::string> SemanticAnalyzer::CollectTypeNames(std::string_view source,
        const CompileCommand* command) const
    {
        return CollectTypeNamesFromViews(SignificantViews(source, Lexer(source).Lex()), command);
    }

    std::unordered_set<std::string> SemanticAnalyzer::CollectTypeNamesFromViews(
        const std::vector<std::string_view>& tokens, const CompileCommand* command) const
    {
        std::unordered_set<std::string> types = {
            "void", "bool", "char", "wchar_t", "char8_t", "char16_t", "char32_t", "short", "int", "long",
            "signed", "unsigned", "float", "double", "auto", "size_t", "std::size_t"
        };
        for (std::size_t i = 0; i + 1 < tokens.size(); ++i)
        {
            if (tokens[i] == "enum" && i + 2 < tokens.size() &&
                (tokens[i + 1] == "class" || tokens[i + 1] == "struct") && IsWord(tokens[i + 2]))
            {
                types.emplace(tokens[i + 2]);
            }
            else if ((tokens[i] == "class" || tokens[i] == "struct" || tokens[i] == "union" || tokens[i] == "enum") &&
                IsWord(tokens[i + 1]))
            {
                types.emplace(tokens[i + 1]);
            }
            else if (tokens[i] == "using" && IsWord(tokens[i + 1]))
            {
                types.emplace(tokens[i + 1]);
            }
        }

        if (command != nullptr)
        {
            for (const auto& [name, value] : command->defines)
            {
                if (types.contains(value))
                {
                    types.insert(name);
                }
            }
        }

        return types;
    }

    AsteriskMeaning SemanticAnalyzer::ClassifyAsteriskStatement(
        std::string_view statement, const std::unordered_set<std::string>& known_types,
        const std::unordered_set<std::string>& known_values) const
    {
        const auto tokens = SignificantTokens(statement);
        if (tokens.size() != 4 ||!IsWord(tokens[0]) || tokens[1] != "*" ||!IsWord(tokens[2]) || tokens[3] != ";")
        {
            return AsteriskMeaning::NotApplicable;
        }

        if (known_types.contains(std::string(tokens[0])))
        {
            return AsteriskMeaning::Declaration;
        }

        if (known_values.contains(std::string(tokens[0])))
        {
            return AsteriskMeaning::Multiplication;
        }

        return AsteriskMeaning::Ambiguous;
    }

    std::vector<SemanticDiagnostic> SemanticAnalyzer::AnalyzeUnusedLocals(std::string_view source,
        const CompileCommand* command) const
    {
        const auto preprocessing = MakePreprocessor(command).Process(source);
        return AnalyzeUnusedLocalsImpl(source, Lexer(source).Lex(), preprocessing, command);
    }

    std::vector<SemanticDiagnostic> SemanticAnalyzer::AnalyzeUnusedLocals(const ParseTree& tree,
        const CompileCommand* command) const
    {
        // Only the active-range mask is recomputed (a cheap line scan); the
        // tokens themselves are reused from the tree (saves 2 full lexes).
        const auto preprocessing = MakePreprocessor(command).Process(tree.Source());
        return AnalyzeUnusedLocalsImpl(tree.Source(), tree.Tokens(), preprocessing, command);
    }

    std::vector<SemanticDiagnostic> SemanticAnalyzer::AnalyzeUnusedLocalsImpl(
        std::string_view source,
        const std::vector<Token>& lexed,
        const PreprocessorResult& preprocessing,
        const CompileCommand* command) const
    {
        std::vector<SignificantToken> tokens;
        tokens.reserve(lexed.size());
        std::vector<std::string_view> views;
        views.reserve(lexed.size());
        std::size_t active_cursor = 0;
        for (const auto& token : lexed)
        {
            while (active_cursor < preprocessing.active_ranges.size() &&
                preprocessing.active_ranges[active_cursor].offset + preprocessing.active_ranges[active_cursor].length <=
                token.offset)
            {
                ++active_cursor;
            }

            const bool active = active_cursor < preprocessing.active_ranges.size() &&
                preprocessing.active_ranges[active_cursor].offset <= token.offset &&
                token.offset < preprocessing.active_ranges[active_cursor].offset +
                preprocessing.active_ranges[active_cursor].length;
            if (!active)
            {
                continue;
            }

            if (token.kind != TokenKind::Whitespace && token.kind != TokenKind::LineComment &&
                token.kind != TokenKind::BlockComment)
            {
                tokens.push_back({source.substr(token.offset, token.length), token.kind, token.offset});
                views.push_back(source.substr(token.offset, token.length));
            }
        }

        const auto known_types = CollectTypeNamesFromViews(views, command);
        struct ScopeFrame
        {
            bool class_body;
            bool function_root;
            std::size_t previous_function;
        };

        std::vector<ScopeFrame> scopes;
        std::vector<std::size_t> function_ids(tokens.size(), 0);
        std::size_t function_id = 0;
        std::size_t next_function_id = 1;
        std::size_t class_depth = 0;
        for (std::size_t i = 0; i < tokens.size(); ++i)
        {
            const auto text = tokens[i].text;
            function_ids[i] = function_id;
            if (text == "{")
            {
                const bool class_body = IsClassBody(tokens, i);
                const bool function_root =!class_body && function_id == 0 && IsFunctionBody(tokens, i);
                scopes.push_back({class_body, function_root, function_id});
                if (class_body)
                {
                    ++class_depth;
                }

                if (function_root)
                {
                    function_id = next_function_id++;
                }
            }
            else if (text == "}" && !scopes.empty())
            {
                const auto frame = scopes.back();
                scopes.pop_back();
                if (frame.class_body && class_depth > 0)
                {
                    --class_depth;
                }

                if (frame.function_root)
                {
                    function_id = frame.previous_function;
                }
            }

            (void) class_depth;
        }

        std::vector<SemanticDiagnostic> diagnostics;
        LineTable line_table;
        line_table.Build(source);
        for (std::size_t i = 0; i < tokens.size(); ++i)
        {
            if (function_ids[i] == 0 || tokens[i].kind != TokenKind::Identifier)
            {
                continue;
            }

            std::size_t declaration_start = i;
            while (declaration_start > 0 && IsModifier(tokens[declaration_start - 1].text))
            {
                --declaration_start;
            }

            if (declaration_start > 0)
            {
                const auto previous = tokens[declaration_start - 1].text;
                if (previous != ";" && previous != "{" && previous != "}")
                {
                    continue;
                }
            }
            else if (declaration_start != 0)
            {
                continue;
            }

            if (!known_types.contains(std::string(tokens[i].text)))
            {
                continue;
            }

            std::size_t name_index = i + 1;
            while (name_index < tokens.size() &&
                (tokens[name_index].text == "*" || tokens[name_index].text == "&" || tokens[name_index].text == "&&" ||
                tokens[name_index].text == "const"))
            {
                ++name_index;
            }

            if (name_index >= tokens.size() || tokens[name_index].kind != TokenKind::Identifier ||
                function_ids[name_index] != function_ids[i])
            {
                continue;
            }

            const auto following = name_index + 1 < tokens.size() ? tokens[name_index + 1].text : std::string_view {};
            if (following != "=" && following != ";" && following != "[")
            {
                continue;
            }

            const std::string_view name = tokens[name_index].text;
            bool used = false;
            for (std::size_t j = name_index + 1; j < tokens.size() && function_ids[j] == function_ids[i]; ++j)
            {
                if (tokens[j].kind == TokenKind::Identifier && tokens[j].text == name)
                {
                    used = true;
                    break;
                }
            }

            if (!used)
            {
                const auto position = line_table.Lookup(tokens[name_index].offset);
                diagnostics.push_back({"semantic/no-unused-local",
                        "local variable '" + std::string(name) + "' is never used",
                        tokens[name_index].offset, name.size(), position.line, position.column});
            }
        }

        return diagnostics;
    }

} // namespace heimdall
