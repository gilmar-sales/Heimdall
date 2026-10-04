#include <Heimdall/IncludeAnalyzer.hpp>

#include <Heimdall/Lexer.hpp>
#include <Heimdall/LineTable.hpp>
#include <Heimdall/Preprocessor.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <fstream>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace heimdall
{

    struct NameHash
    {
        using is_transparent = void;
        std::size_t operator()(std::string_view name) const noexcept
        {
            return std::hash<std::string_view>{}
            (name);
        }
    };

    struct RecordDecl
    {
        std::string name;
        std::string keyword; // class, struct or union
        std::string ns;      // a::b, empty for the global namespace
        bool forwardable = false;
    };

    struct HeaderSymbols
    {
        std::unordered_set<std::string, NameHash, std::equal_to<>> names;
        // The file declares something that is used without being named:
        // a namespace-scope operator or an explicit template specialization.
        bool implicit_use = false;
        bool readable = false;
        // Only extracted for project headers (candidates for forward declaration).
        std::vector<RecordDecl> records;
    };

    namespace
    {

        const std::unordered_set<std::string_view> & Keywords()
        {
            static const std::unordered_set<std::string_view> keywords = {
                "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break",
                "case", "catch", "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept",
                "const", "consteval", "constexpr", "constinit", "const_cast", "continue", "co_await",
                "co_return", "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast",
                "else", "enum", "explicit", "export", "extern", "false", "final", "float", "for", "friend",
                "goto", "if", "import", "inline", "int", "long", "module", "mutable", "namespace", "new",
                "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq", "override", "private",
                "protected", "public", "register", "reinterpret_cast", "requires", "return", "short",
                "signed", "sizeof", "static", "static_assert", "static_cast", "struct", "switch",
                "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid",
                "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while",
                "xor", "xor_eq",
                // Standard attribute names: written in attributes, never declared.
                "nodiscard", "maybe_unused", "deprecated", "fallthrough", "noreturn", "likely", "unlikely",
                "carries_dependency", "no_unique_address", "assume", "defined", "__has_include",
                "__has_cpp_attribute",
            };
            return keywords;
        }

        bool IsTrivia(TokenKind kind)
        {
            return kind == TokenKind::Whitespace || kind == TokenKind::LineComment ||
                kind == TokenKind::BlockComment;
        }

        bool IsAllCaps(std::string_view word)
        {
            if (word.size() < 2)
            {
                return false;
            }

            return std::all_of(word.begin(), word.end(),
                [](char c)
                {
                    return (c >= 'A' && c <= 'Z') ||(c >= '0' && c <= '9') || c == '_';
            });
        }

        std::size_t NextSignificant(const std::vector<Token> & tokens, std::size_t index)
        {
            while (index < tokens.size() && IsTrivia(tokens[index].kind))
            {
                ++index;
            }

            return index;
        }

        std::size_t PreviousSignificant(const std::vector<Token> & tokens, std::size_t index)
        {
            while (index > 0)
            {
                --index;
                if (!IsTrivia(tokens[index].kind))
                {
                    return index;
                }
            }

            return tokens.size();
        }

        // End (exclusive) of the preprocessor line that starts at `begin`,
        // honoring backslash continuations.
        std::size_t DirectiveEnd(std::string_view source, std::size_t begin)
        {
            std::size_t pos = begin;
            while (pos < source.size())
            {
                const std::size_t newline = source.find('\n', pos);
                if (newline == std::string_view::npos)
                {
                    return source.size();
                }

                std::size_t content_end = newline;
                if (content_end > pos && source[content_end - 1] == '\r')
                {
                    --content_end;
                }

                if (content_end > pos && source[content_end - 1] == '\\')
                {
                    pos = newline + 1;
                    continue;
                }

                return newline;
            }

            return source.size();
        }

        bool StartsLine(std::string_view source, std::size_t offset)
        {
            while (offset > 0)
            {
                const char c = source[offset - 1];
                if (c == '\n')
                {
                    return true;
                }

                if (c != ' ' && c != '\t' && c != '\r')
                {
                    return false;
                }

                --offset;
            }

            return true;
        }

        // Names a header file declares for its includers. Token-level and
        // approximate by design; every approximation errs towards listing
        // more names, which can only make the rule report less.
        //
        // Loose mode (project headers) lists every identifier of a
        // namespace-scope declaration, including the types it mentions.
        // Strict mode (system headers, whose layout is regular) lists only
        // names that are being declared, so that `uint32_t` appearing in the
        // declaration of something else does not tie unrelated headers together.
        HeaderSymbols ExtractSymbols(std::string_view source, bool strict)
        {
            HeaderSymbols symbols;
            symbols.readable = true;
            const auto &keywords = Keywords();
            const std::vector<Token> tokens = Lexer(source).Lex();
            auto text =[&](std::size_t i)
            {
                return source.substr(tokens[i].offset, tokens[i].length);
            };
            auto add =[&](std::string_view name)
            {
                if (!name.empty() && name.front() != '_' && name != "std" && !keywords.contains(name))
                {
                    symbols.names.emplace(name);
                }
            };

            // Scope stack: 'N' namespace/extern "C", 'R' record, 'E' enum, 'X' anything else
            // (function bodies, initializers). Empty means global scope.
            std::vector<char> scopes;
            std::vector<bool> paren_collects;
            char pending = 'X';
            bool saw_typedef = false;
            bool in_namespace_name = false;
            bool expect_enumerator = false;
            bool expect_record_name = false;
            std::size_t bracket_depth = 0;

            auto at_declaration_level =[&]
            {
                return scopes.empty() || scopes.back() == 'N';
            };

            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                const Token &token = tokens[i];
                if (IsTrivia(token.kind))
                {
                    continue;
                }

                if (token.kind == TokenKind::Punctuation && text(i) == "#" && StartsLine(source, token.offset))
                {
                    const std::size_t end = DirectiveEnd(source, token.offset);
                    const std::size_t word = NextSignificant(tokens, i + 1);
                    if (word < tokens.size() && tokens[word].offset < end &&
                        tokens[word].kind == TokenKind::Identifier && text(word) == "define")
                    {
                        const std::size_t name = NextSignificant(tokens, word + 1);
                        if (name < tokens.size() && tokens[name].offset < end &&
                            tokens[name].kind == TokenKind::Identifier)
                        {
                            add(text(name));
                        }
                    }

                    while (i + 1 < tokens.size() && tokens[i + 1].offset < end)
                    {
                        ++i;
                    }

                    continue;
                }

                if (token.kind == TokenKind::Punctuation)
                {
                    const std::string_view punctuation = text(i);
                    for (const char c: punctuation)
                    {
                        if (c == '[')
                        {
                            ++bracket_depth;
                        }
                        else if (c == ']' && bracket_depth > 0)
                        {
                            --bracket_depth;
                        }
                    }

                    if (punctuation == "{" || punctuation == ";" || punctuation == ":" || punctuation == "=" ||
                        punctuation == "(" || punctuation == ")" || punctuation == ",")
                    {
                        expect_record_name = false;
                    }

                    if (punctuation == "{")
                    {
                        if (at_declaration_level())
                        {
                            scopes.push_back(pending);
                            expect_enumerator = pending == 'E';
                            pending = 'X';
                        }
                        else
                        {
                            scopes.push_back('X');
                        }

                        in_namespace_name = false;
                    }
                    else if (punctuation == "}")
                    {
                        if (!scopes.empty())
                        {
                            const char closed = scopes.back();
                            scopes.pop_back();
                            if (closed == 'N' || closed == 'X')
                            {
                                saw_typedef = false;
                                pending = 'X';
                            }
                        }

                        expect_enumerator = false;
                    }
                    else if (!scopes.empty() && scopes.back() == 'E' && punctuation == ",")
                    {
                        expect_enumerator = true;
                    }
                    else if (at_declaration_level())
                    {
                        if (punctuation == "(")
                        {
                            const std::size_t previous = PreviousSignificant(tokens, i);
                            const bool macro_call = previous < tokens.size() &&
                                tokens[previous].kind == TokenKind::Identifier && IsAllCaps(text(previous));
                            paren_collects.push_back(saw_typedef || macro_call);
                        }
                        else if (punctuation == ")")
                        {
                            if (!paren_collects.empty())
                            {
                                paren_collects.pop_back();
                            }
                        }
                        else if (punctuation == ";")
                        {
                            saw_typedef = false;
                            in_namespace_name = false;
                            pending = 'X';
                            paren_collects.clear();
                        }
                        else if (punctuation == "=")
                        {
                            in_namespace_name = false;
                        }
                    }

                    continue;
                }

                if (token.kind != TokenKind::Identifier)
                {
                    continue;
                }

                const std::string_view word = text(i);
                if (!scopes.empty() && scopes.back() == 'E')
                {
                    if (expect_enumerator && bracket_depth == 0)
                    {
                        add(word);
                        expect_enumerator = false;
                    }

                    continue;
                }

                if (!at_declaration_level())
                {
                    continue;
                }

                if (word == "namespace")
                {
                    pending = 'N';
                    in_namespace_name = true;
                    continue;
                }

                if (word == "extern")
                {
                    const std::size_t next = NextSignificant(tokens, i + 1);
                    if (next < tokens.size() && tokens[next].kind == TokenKind::StringLiteral)
                    {
                        pending = 'N';
                    }

                    continue;
                }

                if (word == "enum")
                {
                    pending = 'E';
                    expect_record_name = true;
                    continue;
                }

                if ((word == "class" || word == "struct" || word == "union") && pending != 'E' && pending != 'N')
                {
                    pending = 'R';
                    expect_record_name = true;
                    continue;
                }

                if (word == "typedef")
                {
                    saw_typedef = true;
                    continue;
                }

                if (word == "operator")
                {
                    if (paren_collects.empty())
                    {
                        symbols.implicit_use = true;
                    }

                    continue;
                }

                if (word == "template")
                {
                    // Skip the parameter list: its names are not declarations.
                    const std::size_t open = NextSignificant(tokens, i + 1);
                    if (open < tokens.size() && tokens[open].kind == TokenKind::Punctuation &&
                        text(open).starts_with("<"))
                    {
                        std::size_t depth = 0;
                        std::size_t j = open;
                        for (; j < tokens.size(); ++j)
                        {
                            if (tokens[j].kind != TokenKind::Punctuation)
                            {
                                continue;
                            }

                            for (const char c: text(j))
                            {
                                if (c == '<')
                                {
                                    ++depth;
                                }
                                else if (c == '>' && depth > 0)
                                {
                                    --depth;
                                }
                            }

                            if (depth == 0)
                            {
                                break;
                            }
                        }

                        const std::size_t first = NextSignificant(tokens, open + 1);
                        if (first < tokens.size() && text(first) == ">")
                        {
                            symbols.implicit_use = true;
                        }

                        i = j;
                    }

                    continue;
                }

                if (bracket_depth > 0 || in_namespace_name)
                {
                    continue;
                }

                const std::size_t next = NextSignificant(tokens, i + 1);
                const std::string_view next_text = next < tokens.size() ? text(next) : std::string_view();
                if (next_text == "::")
                {
                    continue;
                }

                if (expect_record_name)
                {
                    // `class EXPORT_MACRO Name final : Base {` lists the macro too.
                    if (word != "final")
                    {
                        add(word);
                    }

                    continue;
                }

                const bool in_parens = !paren_collects.empty();
                const bool collecting = in_parens && paren_collects.back();
                if (!strict)
                {
                    if (!in_parens || collecting)
                    {
                        add(word);
                    }

                    continue;
                }

                if (collecting)
                {
                    add(word);
                }
                else if (in_parens)
                {
                    // Declarator nested in parentheses: `void (*signal(int, ...))(int);`
                    if (next_text == "(")
                    {
                        add(word);
                    }
                }
                else if (next_text == "(" || next_text == "=" || next_text == ";" || next_text == "{" ||
                    next_text == "[" || next_text == "," || next_text == ")" || next_text == ":" ||
                    next_text == "final" || next_text == "asm" || next_text == "alignas" ||
                    next_text.starts_with("__"))
                {
                    add(word);
                }
            }

            return symbols;
        }

        // Namespace-scope `class`/`struct`/`union` declarations of a header: what
        // a forward declaration would have to repeat.
        std::vector<RecordDecl> ExtractRecords(std::string_view source)
        {
            std::vector<RecordDecl> records;
            const std::vector<Token> tokens = Lexer(source).Lex();
            auto text = [&](std::size_t i)
            {
                return source.substr(tokens[i].offset, tokens[i].length);
            };

            struct Frame
            {
                bool is_namespace = false;
                std::size_t names_added = 0;
            };

            std::vector<Frame> scopes;
            std::vector<std::string> namespace_names;
            bool in_template = false;
            bool skip_statement = false;
            auto at_declaration_level = [&]
            {
                return std::all_of(scopes.begin(), scopes.end(), [](const Frame &f) { return f.is_namespace; });
            };

            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                const Token &token = tokens[i];
                if (IsTrivia(token.kind))
                {
                    continue;
                }

                if (token.kind == TokenKind::Punctuation && text(i) == "#" && StartsLine(source, token.offset))
                {
                    const std::size_t end = DirectiveEnd(source, token.offset);
                    while (i + 1 < tokens.size() && tokens[i + 1].offset < end)
                    {
                        ++i;
                    }

                    continue;
                }

                if (token.kind == TokenKind::Punctuation)
                {
                    const std::string_view punctuation = text(i);
                    if (punctuation == "{")
                    {
                        scopes.push_back({});
                        in_template = false;
                    }
                    else if (punctuation == "}")
                    {
                        if (!scopes.empty())
                        {
                            const Frame closed = scopes.back();
                            scopes.pop_back();
                            namespace_names.resize(namespace_names.size() - closed.names_added);
                            skip_statement = false;
                        }
                    }
                    else if (punctuation == ";")
                    {
                        in_template = false;
                        skip_statement = false;
                    }

                    continue;
                }

                if (token.kind != TokenKind::Identifier || !at_declaration_level())
                {
                    continue;
                }

                const std::string_view word = text(i);
                const std::size_t previous = PreviousSignificant(tokens, i);
                const std::string_view previous_text = previous < tokens.size() ? text(previous) : std::string_view();
                if (word == "namespace" && previous_text != "using")
                {
                    std::vector<std::string> names;
                    std::size_t j = i + 1;
                    for (; j < tokens.size(); ++j)
                    {
                        if (IsTrivia(tokens[j].kind))
                        {
                            continue;
                        }

                        const std::string_view piece = text(j);
                        if (piece == "{" || piece == "=" || piece == ";")
                        {
                            break;
                        }

                        if (tokens[j].kind == TokenKind::Identifier)
                        {
                            names.emplace_back(piece);
                        }
                    }

                    if (j < tokens.size() && text(j) == "{")
                    {
                        if (names.empty())
                        {
                            names.emplace_back("(anonymous)");
                        }

                        if (previous_text == "inline")
                        {
                            names.emplace_back("(inline)");
                        }

                        scopes.push_back({true, names.size()});
                        namespace_names.insert(namespace_names.end(), names.begin(), names.end());
                    }

                    i = j;
                    continue;
                }

                if (word == "extern")
                {
                    const std::size_t next = NextSignificant(tokens, i + 1);
                    const std::size_t brace = next < tokens.size() ? NextSignificant(tokens, next + 1) : tokens.size();
                    if (next < tokens.size() && tokens[next].kind == TokenKind::StringLiteral &&
                        brace < tokens.size() && text(brace) == "{")
                    {
                        scopes.push_back({true, 0});
                        i = brace;
                    }

                    continue;
                }

                if (word == "template")
                {
                    in_template = true;
                    const std::size_t open = NextSignificant(tokens, i + 1);
                    if (open < tokens.size() && text(open).starts_with("<"))
                    {
                        std::size_t depth = 0;
                        std::size_t j = open;
                        for (; j < tokens.size(); ++j)
                        {
                            if (tokens[j].kind != TokenKind::Punctuation)
                            {
                                continue;
                            }

                            for (const char c: text(j))
                            {
                                if (c == '<')
                                {
                                    ++depth;
                                }
                                else if (c == '>' && depth > 0)
                                {
                                    --depth;
                                }
                            }

                            if (depth == 0)
                            {
                                break;
                            }
                        }

                        i = j;
                    }

                    continue;
                }

                if (word == "friend" || word == "typedef" || word == "using")
                {
                    skip_statement = true;
                    continue;
                }

                if ((word != "class" && word != "struct" && word != "union") || skip_statement ||
                    previous_text == "enum")
                {
                    continue;
                }

                // class [[attr]] EXPORT Name final : Base {   |   class Name;
                std::vector<std::string_view> names;
                std::size_t j = i + 1;
                std::size_t bracket_depth = 0;
                std::string_view stop;
                for (; j < tokens.size(); ++j)
                {
                    if (IsTrivia(tokens[j].kind))
                    {
                        continue;
                    }

                    const std::string_view piece = text(j);
                    if (tokens[j].kind == TokenKind::Punctuation)
                    {
                        for (const char c: piece)
                        {
                            if (c == '[')
                            {
                                ++bracket_depth;
                            }
                            else if (c == ']' && bracket_depth > 0)
                            {
                                --bracket_depth;
                            }
                        }

                        if (bracket_depth == 0 && piece != "[" && piece != "]" && piece != "[[" && piece != "]]")
                        {
                            stop = piece;
                            break;
                        }

                        continue;
                    }

                    if (tokens[j].kind == TokenKind::Identifier && bracket_depth == 0 && piece != "final" &&
                        !Keywords().contains(piece))
                    {
                        names.push_back(piece);
                    }
                }

                const bool definition = stop == "{" || stop == ":";
                const bool declaration = stop == ";" && names.size() == 1;
                if (names.empty() || (!definition && !declaration))
                {
                    continue;
                }

                RecordDecl record;
                record.name = std::string(names.back());
                record.keyword = std::string(word);
                for (const auto & piece: namespace_names)
                {
                    if (!record.ns.empty())
                    {
                        record.ns += "::";
                    }

                    record.ns += piece;
                }

                record.forwardable = !in_template && record.ns.find('(') == std::string::npos;
                records.push_back(std::move(record));
                i = j - 1;
            }

            return records;
        }

        struct UseClassification
        {
            // Names with at least one use that needs the complete type.
            std::unordered_set<std::string_view> blocked;
            // Declarators of the form `N *name` / `N &name`.
            std::unordered_set<std::string_view> pointer_names;
            // Pointer/reference declared by name is dereferenced in the file.
            bool dereferenced = false;
        };

        bool IsPointerPunctuation(std::string_view text)
        {
            return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c == '*' || c == '&'; });
        }

        // Which uses of `interest` names a forward declaration could not satisfy.
        // A use is forward-declarable when the name is followed by `*` or `&`
        // (after optional cv-qualifiers), or is itself a `class N;` declaration.
        // Everything inside function bodies and initializers is treated as a
        // use of the complete type.
        UseClassification ClassifyUses(const ParseTree &tree, const std::unordered_set<std::string_view> &interest)
        {
            UseClassification result;
            const std::string_view source = tree.Source();
            const auto &tokens = tree.Tokens();
            const auto &directives = tree.Directives();
            auto text = [&](std::size_t i)
            {
                return source.substr(tokens[i].offset, tokens[i].length);
            };

            std::vector<char> scopes; // 'N' namespace, 'R' record, 'E' enum, 'X' body/initializer
            char pending = 'X';
            std::size_t body_depth = 0;
            std::size_t cursor = 0;
            std::vector<std::pair<std::size_t, std::size_t>> bodies;
            std::size_t body_start = 0;
            auto in_body = [&] { return body_depth > 0; };
            // `struct S {` / `enum E : int {` open a record body; `struct S *p`
            // and `struct S s;` inside a declaration do not.
            auto opens_body = [&](std::size_t from)
            {
                for (std::size_t j = from + 1; j < tokens.size(); ++j)
                {
                    if (tokens[j].kind != TokenKind::Punctuation)
                    {
                        continue;
                    }

                    const std::string_view piece = text(j);
                    if (piece == "{" || piece == ":")
                    {
                        return true;
                    }

                    if (piece == ";" || piece == "(" || piece == ")" || piece == "=" || piece == "," ||
                        IsPointerPunctuation(piece))
                    {
                        return false;
                    }
                }

                return false;
            };

            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                const Token &token = tokens[i];
                if (IsTrivia(token.kind))
                {
                    continue;
                }

                while (cursor < directives.size() &&
                    directives[cursor].offset + directives[cursor].length <= token.offset)
                {
                    ++cursor;
                }

                if (cursor < directives.size() && directives[cursor].offset <= token.offset)
                {
                    if (token.kind == TokenKind::Identifier && directives[cursor].kind != DirectiveKind::Include &&
                        directives[cursor].kind != DirectiveKind::Pragma && interest.contains(text(i)))
                    {
                        result.blocked.insert(text(i));
                    }

                    continue;
                }

                if (token.kind == TokenKind::Punctuation)
                {
                    const std::string_view punctuation = text(i);
                    if (punctuation == "{")
                    {
                        const bool declaration_level = !in_body() && (scopes.empty() || scopes.back() != 'E');
                        const char kind = declaration_level ? pending : 'X';
                        scopes.push_back(kind);
                        if (kind == 'X' && !in_body())
                        {
                            body_start = i;
                        }

                        if (kind == 'X')
                        {
                            ++body_depth;
                        }

                        pending = 'X';
                    }
                    else if (punctuation == "}")
                    {
                        if (!scopes.empty())
                        {
                            const char closed = scopes.back();
                            scopes.pop_back();
                            if (closed == 'X' && body_depth > 0 && --body_depth == 0)
                            {
                                bodies.emplace_back(body_start, i);
                            }
                        }

                        pending = 'X';
                    }
                    else if (punctuation == ";")
                    {
                        pending = 'X';
                    }

                    continue;
                }

                if (token.kind != TokenKind::Identifier)
                {
                    continue;
                }

                const std::string_view word = text(i);
                if (!in_body())
                {
                    if (word == "namespace")
                    {
                        pending = 'N';
                    }
                    else if (word == "extern")
                    {
                        const std::size_t next = NextSignificant(tokens, i + 1);
                        if (next < tokens.size() && tokens[next].kind == TokenKind::StringLiteral)
                        {
                            pending = 'N';
                        }
                    }
                    else if (word == "enum" && opens_body(i))
                    {
                        pending = 'E';
                    }
                    else if ((word == "class" || word == "struct" || word == "union") && pending != 'E' &&
                        pending != 'N' && opens_body(i))
                    {
                        pending = 'R';
                    }
                }

                if (!interest.contains(word))
                {
                    continue;
                }

                if (in_body() || (!scopes.empty() && scopes.back() == 'E'))
                {
                    result.blocked.insert(word);
                    continue;
                }

                const std::size_t previous = PreviousSignificant(tokens, i);
                const std::string_view previous_text = previous < tokens.size() ? text(previous) : std::string_view();
                std::size_t next = NextSignificant(tokens, i + 1);
                const bool elaborated = previous_text == "class" || previous_text == "struct" ||
                    previous_text == "union";
                if (elaborated && next < tokens.size() && text(next) == ";")
                {
                    continue; // `class N;` / `friend class N;`
                }

                while (next < tokens.size() && (text(next) == "const" || text(next) == "volatile"))
                {
                    next = NextSignificant(tokens, next + 1);
                }

                if (next >= tokens.size() || !IsPointerPunctuation(text(next)))
                {
                    result.blocked.insert(word);
                    continue;
                }

                while (next < tokens.size() && (IsPointerPunctuation(text(next)) || text(next) == "const" ||
                    text(next) == "volatile"))
                {
                    next = NextSignificant(tokens, next + 1);
                }

                if (next < tokens.size() && tokens[next].kind == TokenKind::Identifier &&
                    !Keywords().contains(text(next)))
                {
                    result.pointer_names.insert(text(next));
                }
            }

            // Dereferencing a pointer/reference by name needs the complete type,
            // including in member initializer lists outside any body.
            if (!result.pointer_names.empty())
            {
                auto unsafe_token = [&](std::size_t i)
                {
                    const std::string_view piece = text(i);
                    return piece == "." || piece == "->" || piece == "[" || piece.find('*') != std::string_view::npos ||
                        piece == "delete" || piece == "dynamic_cast" || piece == "static_cast" ||
                        piece == "reinterpret_cast" || piece == "const_cast" || piece == "typeid" ||
                        piece == "sizeof";
                };

                std::size_t directive_cursor = 0;
                for (std::size_t i = 0; i < tokens.size() && !result.dereferenced; ++i)
                {
                    while (directive_cursor < directives.size() &&
                        directives[directive_cursor].offset + directives[directive_cursor].length <= tokens[i].offset)
                    {
                        ++directive_cursor;
                    }

                    // `#include <widget.hpp>` must not read as `widget.hpp`.
                    if ((directive_cursor < directives.size() &&
                        directives[directive_cursor].offset <= tokens[i].offset) ||
                        tokens[i].kind != TokenKind::Identifier || !result.pointer_names.contains(text(i)))
                    {
                        continue;
                    }

                    const std::size_t next = NextSignificant(tokens, i + 1);
                    if (next < tokens.size() && (text(next) == "." || text(next) == "->" || text(next) == "["))
                    {
                        result.dereferenced = true;
                    }
                }

                for (const auto &[begin, end]: bodies)
                {
                    bool mentions = false;
                    bool unsafe = false;
                    for (std::size_t i = begin; i <= end && i < tokens.size(); ++i)
                    {
                        if (IsTrivia(tokens[i].kind))
                        {
                            continue;
                        }

                        if (tokens[i].kind == TokenKind::Identifier && result.pointer_names.contains(text(i)))
                        {
                            mentions = true;
                        }

                        if (unsafe_token(i))
                        {
                            unsafe = true;
                        }
                    }

                    if (mentions && unsafe)
                    {
                        result.dereferenced = true;
                        break;
                    }
                }
            }

            return result;
        }

        std::string ReadWholeFile(const std::filesystem::path & path, std::size_t max_bytes)
        {
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            if (ec || size > max_bytes)
            {
                return {};
            }

            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                return {};
            }

            std::string content(static_cast<std::size_t>(size), '\0');
            in.read(content.data(), static_cast<std::streamsize>(content.size()));
            content.resize(static_cast<std::size_t>(in.gcount()));
            return content;
        }

        std::int64_t MTimeOf(const std::filesystem::path & path)
        {
            std::error_code ec;
            const auto time = std::filesystem::last_write_time(path, ec);
            return ec ? 0 : static_cast<std::int64_t>(time.time_since_epoch().count());
        }

        std::uintmax_t SizeOf(const std::filesystem::path & path)
        {
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            return ec ? 0 : size;
        }

        // Process-wide: the same <vector> is extracted once across documents.
        std::shared_ptr<const HeaderSymbols> SymbolsFor(const std::filesystem::path & path, bool strict)
        {
            struct Cached
            {
                std::uintmax_t size = 0;
                std::int64_t mtime = 0;
                std::shared_ptr<const HeaderSymbols> symbols;
            };

            static std::mutex mutex;
            static std::unordered_map<std::string, Cached> cache;
            const std::string key = (strict ? "S|" : "L|") + path.string();
            const auto size = SizeOf(path);
            const auto mtime = MTimeOf(path);
            {
                const std::lock_guard<std::mutex> lock(mutex);
                if (const auto found = cache.find(key);
                    found != cache.end() && found->second.size == size && found->second.mtime == mtime)
                {
                    return found->second.symbols;
                }
            }

            constexpr std::size_t max_bytes = 4u << 20;
            std::shared_ptr<const HeaderSymbols> symbols;
            const std::string content = ReadWholeFile(path, max_bytes);
            if (content.empty() && size != 0)
            {
                symbols = std::make_shared<const HeaderSymbols>();
            }
            else
            {
                HeaderSymbols extracted = ExtractSymbols(content, strict);
                if (!strict)
                {
                    extracted.records = ExtractRecords(content);
                }

                symbols = std::make_shared<const HeaderSymbols>(std::move(extracted));
            }

            const std::lock_guard<std::mutex> lock(mutex);
            if (cache.size() > 8192)
            {
                cache.clear();
            }

            cache[key] = {size, mtime, symbols};
            return symbols;
        }

        // `#ifndef X` / `#define X` ... `#endif` wrapping the whole file.
        bool HasIncludeGuard(const std::vector<PreprocessorDirective> & directives)
        {
            if (directives.size() < 3 || directives.front().kind != DirectiveKind::Ifndef ||
                directives[1].kind != DirectiveKind::Define || directives.back().kind != DirectiveKind::Endif)
            {
                return false;
            }

            std::size_t depth = 0;
            for (std::size_t i = 0; i < directives.size(); ++i)
            {
                switch (directives[i].kind)
                {
                case DirectiveKind::If:
                case DirectiveKind::Ifdef:
                case DirectiveKind::Ifndef:
                    ++depth;
                    break;
                case DirectiveKind::Endif:
                    depth = depth == 0 ? 0 : depth - 1;
                    if (depth == 0 && i + 1 != directives.size())
                    {
                        return false;
                    }

                    break;
                default:
                    break;
                }
            }

            return true;
        }

        struct IncludeDirective
        {
            std::size_t directive_offset = 0;
            std::size_t directive_length = 0;
            std::size_t target_offset = 0;
            std::size_t target_length = 0;
            std::string target;
            bool conditional = false;
            bool keep = false;
        };

        // Direct `#include "x"` / `#include <x>` directives, in source order.
        // Macro includes and `#include_next` have no literal target to judge.
        std::vector<IncludeDirective> DirectIncludes(const ParseTree &tree)
        {
            const std::string_view source = tree.Source();
            const auto &directives = tree.Directives();

            const std::size_t base_depth = HasIncludeGuard(directives) ? 1 : 0;

            std::vector<IncludeDirective> result;
            std::size_t depth = 0;
            for (const auto & directive: directives)
            {
                switch (directive.kind)
                {
                case DirectiveKind::If:
                case DirectiveKind::Ifdef:
                case DirectiveKind::Ifndef:
                    ++depth;
                    continue;
                case DirectiveKind::Endif:
                    depth = depth == 0 ? 0 : depth - 1;
                    continue;
                case DirectiveKind::Include:
                    break;
                default:
                    continue;
                }

                const std::string_view body = source.substr(directive.offset, directive.length);
                std::size_t pos = body.find('#');
                if (pos == std::string_view::npos)
                {
                    continue;
                }

                ++pos;
                while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t'))
                {
                    ++pos;
                }

                constexpr std::string_view keyword = "include";
                if (body.substr(pos, keyword.size()) != keyword)
                {
                    continue;
                }

                pos += keyword.size();
                if (pos < body.size() && (std::isalnum(static_cast<unsigned char>(body[pos])) || body[pos] == '_'))
                {
                    continue; // include_next and friends
                }

                const std::size_t open = body.find_first_of("<\"", pos);
                if (open == std::string_view::npos)
                {
                    continue;
                }

                // Only whitespace may sit between the keyword and the delimiter.
                if (body.substr(pos, open - pos).find_first_not_of(" \t") != std::string_view::npos)
                {
                    continue;
                }

                const char close_char = body[open] == '<' ? '>' : '"';
                const std::size_t close = body.find(close_char, open + 1);
                if (close == std::string_view::npos)
                {
                    continue;
                }

                IncludeDirective include;
                include.directive_offset = directive.offset;
                include.directive_length = directive.length;
                include.target_offset = directive.offset + open;
                include.target_length = close - open + 1;
                include.target = std::string(body.substr(open, close - open + 1));
                include.conditional = depth > base_depth;
                const std::string_view trailing = body.substr(close + 1);
                include.keep = trailing.find("IWYU pragma: keep") != std::string_view::npos ||
                    trailing.find("IWYU pragma: export") != std::string_view::npos;
                result.push_back(std::move(include));
            }

            return result;
        }

        std::string Lowercase(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(),
                [](unsigned char c)
                {
                    return static_cast<char>(std::tolower(c));
            });
            return text;
        }

        // Headers whose job is not to be named: they are used implicitly by
        // language features (braced lists, <=>, typeid, new, coroutines,
        // structured bindings) or textually included.
        bool IsImplicitUseHeader(std::string_view target, const std::filesystem::path & header)
        {
            static const std::unordered_set<std::string_view> implicit = {
                "<initializer_list>", "<compare>", "<typeinfo>", "<new>", "<coroutine>", "<tuple>",
                "<version>", "<exception>", "<cstdarg>", "<stdarg.h>", "<csetjmp>", "<setjmp.h>",
            };
            if (implicit.contains(target))
            {
                return true;
            }

            const std::string extension = Lowercase(header.extension().string());
            return extension == ".inc" || extension == ".def" || extension == ".inl" || extension == ".tpp" ||
                extension == ".ipp" || extension == ".tcc";
        }

        // Configuration headers every standard header drags in: their names say
        // nothing about which include is needed.
        bool IsAmbientConfigHeader(const std::filesystem::path & file)
        {
            const std::string name = file.filename().string();
            return name == "c++config.h" || name == "os_defines.h" || name == "cpu_defines.h";
        }

        bool IsSystemFile(const std::filesystem::path & file, const std::vector<std::filesystem::path> & system_dirs)
        {
            const auto normalized = file.lexically_normal();
            return std::any_of(system_dirs.begin(), system_dirs.end(), [&](const std::filesystem::path &dir)
                {
                    const auto relative = normalized.lexically_relative(dir);
                    return !relative.empty() && *relative.begin() != "..";
            });
        }

        // The include closure of one directive depends only on the target, the
        // search configuration and the headers on disk. Typing in the include block
        // changes the file's fingerprint on every keystroke, and re-walking the
        // closure of <string>, <vector>, ... (hundreds of headers each) took about
        // half a second per rebuild. Complete closures are therefore memoized
        // process-wide and revalidated by stat.
        struct CachedClosure
        {
            std::vector<std::filesystem::path> files;
            std::vector<std::pair<std::uintmax_t, std::int64_t>> stamps;
        };

        std::string ClosureKey(const std::filesystem::path & base_dir, std::string_view target,
            const CompileCommand *command)
        {
            std::string key;
            const bool quoted = !target.empty() && target.front() == '"';
            if (quoted)
            {
                key += base_dir.lexically_normal().generic_string();
            }

            key += '\0';
            key += target;
            key += '\0';
            if (command != nullptr)
            {
                if (!command->arguments.empty())
                {
                    key += command->arguments.front();
                }

                key += '\0';
                for (const auto & dir: command->include_directories)
                {
                    key += dir.generic_string();
                    key += '\1';
                }

                key += '\0';
                for (const auto & dir: command->quote_directories)
                {
                    key += dir.generic_string();
                    key += '\1';
                }
            }

            return key;
        }

        std::mutex & ClosureMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        std::unordered_map<std::string, CachedClosure> & ClosureCache()
        {
            static std::unordered_map<std::string, CachedClosure> cache;
            return cache;
        }

        // Closure of `#include <target>` seen from base_dir; `complete` is false
        // for anything that could not be fully resolved (never cached).
        std::vector<std::filesystem::path> ResolveClosure(const std::filesystem::path & base_dir,
            const std::string &target, const CompileCommand *command, const IncludeIndex::Limits &limits,
            bool &complete)
        {
            const std::string key = ClosureKey(base_dir, target, command);
            {
                const std::lock_guard<std::mutex> lock(ClosureMutex());
                const auto found = ClosureCache().find(key);
                if (found != ClosureCache().end())
                {
                    const CachedClosure &cached = found->second;
                    bool fresh = true;
                    for (std::size_t i = 0; i < cached.files.size() && fresh; ++i)
                    {
                        fresh = SizeOf(cached.files[i]) == cached.stamps[i].first &&
                            MTimeOf(cached.files[i]) == cached.stamps[i].second;
                    }

                    if (fresh)
                    {
                        complete = true;
                        return cached.files;
                    }
                }
            }

            ResolveReport report;
            auto files = IncludeIndex::ResolveHeaders(base_dir, "#include " + target + "\n", command, limits, &report);
            complete = report.complete && !files.empty();
            if (complete)
            {
                CachedClosure cached;
                cached.files = files;
                for (const auto & path: files)
                {
                    cached.stamps.emplace_back(SizeOf(path), MTimeOf(path));
                }

                const std::lock_guard<std::mutex> lock(ClosureMutex());
                if (ClosureCache().size() > 1024)
                {
                    ClosureCache().clear();
                }

                ClosureCache()[key] = std::move(cached);
            }

            return files;
        }

        std::string PathKey(const std::filesystem::path & path)
        {
            return path.lexically_normal().generic_string();
        }

    } // namespace

    // Forward declarations replacing the include, one line each, or nothing
    // when some use of `matched` needs more than a declaration.
    static std::optional<std::vector<std::string>> ForwardDeclarations(const ParseTree &tree,
        const IncludeProfile::Entry &entry, const std::vector<std::string_view> &matched)
    {
        std::vector<RecordDecl> chosen;
        for (const std::string_view name: matched)
        {
            const RecordDecl *found = nullptr;
            for (const auto & provider: entry.providers)
            {
                for (const auto & record: provider->records)
                {
                    if (record.name != name)
                    {
                        continue;
                    }

                    if (!record.forwardable || (found != nullptr &&
                        (found->ns != record.ns || found->keyword != record.keyword)))
                    {
                        return std::nullopt;
                    }

                    found = &record;
                }
            }

            if (found == nullptr)
            {
                return std::nullopt;
            }

            chosen.push_back(*found);
        }

        const std::unordered_set<std::string_view> interest(matched.begin(), matched.end());
        const UseClassification uses = ClassifyUses(tree, interest);
        if (!uses.blocked.empty() || uses.dereferenced)
        {
            return std::nullopt;
        }

        std::vector<std::string> lines;
        for (const auto & record: chosen)
        {
            std::string line = record.ns.empty() ? "" : "namespace " + record.ns + " { ";
            line += record.keyword + " " + record.name + ";";
            line += record.ns.empty() ? "" : " }";
            lines.push_back(std::move(line));
        }

        return lines;
    }

    std::string IncludeAnalyzer::Fingerprint(const std::filesystem::path & file, const ParseTree &tree,
        const CompileCommand *command)
    {
        const std::filesystem::path base_dir = file.has_parent_path() ? file.parent_path() : std::filesystem::path();
        return IncludeIndex::IncludeFingerprint(base_dir, tree.Source(), command);
    }

    std::shared_ptr<const IncludeProfile> IncludeAnalyzer::BuildProfile(const std::filesystem::path & file,
        const ParseTree &tree, const CompileCommand *command)
    {
        auto profile = std::make_shared<IncludeProfile>();
        profile->fingerprint = Fingerprint(file, tree, command);
        const std::filesystem::path base_dir = file.has_parent_path() ? file.parent_path() : std::filesystem::path();
        const auto includes = DirectIncludes(tree);
        profile->entries.resize(includes.size());

        // Standard-library operators and specializations always belong to a type
        // the file has to name anyway; only project headers can be "used" through
        // an operator alone.
        std::vector<std::filesystem::path> system_dirs;
        for (const auto & dir: IncludeIndex::SystemIncludes(
            command != nullptr && !command->arguments.empty() ? std::string_view(command->arguments.front())
            : std::string_view()))
        {
            system_dirs.push_back(dir.lexically_normal());
        }


        IncludeIndex::Limits limits;
        limits.max_headers = 1024;
        limits.follow_include_next = true;

        struct Closure
        {
            std::vector<std::filesystem::path> files;
            std::unordered_set<std::string> keys;
            bool complete = false;
        };

        std::vector<Closure> closures(includes.size());
        for (std::size_t i = 0; i < includes.size(); ++i)
        {
            profile->entries[i].target = includes[i].target;
            if (includes[i].conditional)
            {
                continue;
            }

            closures[i].files = ResolveClosure(base_dir, includes[i].target, command, limits, closures[i].complete);
            for (const auto & path: closures[i].files)
            {
                closures[i].keys.insert(PathKey(path));
            }
        }

        // Cycle: the analyzed file is reachable from its own include.
        std::error_code absolute_ec;
        const std::filesystem::path self = std::filesystem::absolute(file, absolute_ec).lexically_normal();
        for (std::size_t i = 0; i < includes.size() && !absolute_ec; ++i)
        {
            for (const auto & path: closures[i].files)
            {
                std::error_code equivalent_ec;
                if (path.filename() == self.filename() && std::filesystem::equivalent(path, self, equivalent_ec) &&
                    !equivalent_ec)
                {
                    profile->entries[i].circular = true;
                    break;
                }
            }
        }

        const std::string extension = Lowercase(file.extension().string());
        profile->is_header_file = extension == ".h" || extension == ".hh" || extension == ".hpp" ||
            extension == ".hxx" || extension == ".h++";
        const std::string primary_stem = Lowercase(file.stem().string());
        for (std::size_t i = 0; i < includes.size(); ++i)
        {
            auto &entry = profile->entries[i];
            const Closure &closure = closures[i];
            if (!closure.complete)
            {
                continue;
            }

            const std::filesystem::path & header = closure.files.front();
            const std::string header_key = PathKey(header);
            if (Lowercase(header.stem().string()) == primary_stem || IsImplicitUseHeader(entry.target, header))
            {
                continue;
            }

            // Names reachable through another direct include belong to that
            // include: <iostream> must not count as used because of <string>.
            std::unordered_set<std::string> covered;
            for (std::size_t j = 0; j < includes.size(); ++j)
            {
                if (j == i || closures[j].files.empty())
                {
                    continue;
                }

                const std::string other_key = PathKey(closures[j].files.front());
                if (other_key == header_key ||!closure.keys.contains(other_key) ||
                    closures[j].keys.contains(header_key))
                {
                    continue;
                }

                covered.insert(closures[j].keys.begin(), closures[j].keys.end());
            }

            bool usable = true;
            for (const auto & path: closure.files)
            {
                const std::string key = PathKey(path);
                const bool direct = key == header_key;
                if (!direct && (covered.contains(key) || IsAmbientConfigHeader(path)))
                {
                    continue;
                }

                auto symbols = SymbolsFor(path, IsSystemFile(path, system_dirs));
                if (!symbols->readable || (direct && symbols->implicit_use && !IsSystemFile(header, system_dirs)))
                {
                    usable = false;
                    break;
                }

                entry.providers.push_back(std::move(symbols));
            }

            if (!usable)
            {
                entry.providers.clear();
                continue;
            }

            entry.eligible = true;
            entry.project_header = !IsSystemFile(header, system_dirs);
        }

        std::unordered_set<std::string> stamped;
        for (const auto & closure: closures)
        {
            for (const auto & path: closure.files)
            {
                if (stamped.insert(PathKey(path)).second)
                {
                    profile->stamps.push_back({path, SizeOf(path), MTimeOf(path)});
                }
            }
        }

        return profile;
    }

    bool IncludeAnalyzer::IsFresh(const IncludeProfile &profile)
    {
        return std::all_of(profile.stamps.begin(), profile.stamps.end(),
            [](const IncludeProfile::FileStamp & stamp)
            {
                return SizeOf(stamp.path) == stamp.size && MTimeOf(stamp.path) == stamp.mtime;
        });
    }

    std::vector<Diagnostic> IncludeAnalyzer::Analyze(const ParseTree &tree,
        const IncludeProfile &profile)
    {
        std::vector<Diagnostic> diagnostics;
        const auto includes = DirectIncludes(tree);
        if (includes.size() != profile.entries.size())
        {
            return diagnostics;
        }

        const std::string_view source = tree.Source();
        const auto &tokens = tree.Tokens();
        const auto &directives = tree.Directives();
        const auto &keywords = Keywords();

        // Every identifier of the file outside #include lines. Comments and
        // literals are separate token kinds, so they never count.
        std::unordered_set<std::string_view> used;
        const bool guarded = HasIncludeGuard(directives);
        std::size_t cursor = 0;
        bool after_member_access = false;
        for (const auto & token: tokens)
        {
            if (token.kind == TokenKind::Whitespace || token.kind == TokenKind::LineComment ||
                token.kind == TokenKind::BlockComment)
            {
                continue;
            }

            // `object.size()` names a member, which no header declares at
            // namespace scope; counting it would tie every container header
            // to std::size.
            const bool member_name = after_member_access;
            after_member_access = false;
            if (token.kind == TokenKind::Punctuation)
            {
                const std::string_view punctuation = source.substr(token.offset, token.length);
                after_member_access = punctuation == "." || punctuation == "->";
                continue;
            }

            if (token.kind != TokenKind::Identifier || member_name)
            {
                continue;
            }

            while (cursor < directives.size() && directives[cursor].offset + directives[cursor].length <= token.offset)
            {
                ++cursor;
            }

            if (cursor < directives.size() && directives[cursor].offset <= token.offset)
            {
                const bool guard_line = guarded && cursor < 2;
                if (directives[cursor].kind == DirectiveKind::Include ||
                    directives[cursor].kind == DirectiveKind::Pragma || guard_line)
                {
                    continue;
                }
            }

            const std::string_view word = source.substr(token.offset, token.length);
            if (!keywords.contains(word))
            {
                used.insert(word);
            }
        }

        // A file with nothing but includes (an umbrella header) re-exports them.
        if (used.empty() && std::none_of(profile.entries.begin(), profile.entries.end(),
            [](const IncludeProfile::Entry &entry) { return entry.circular; }))
        {
            return diagnostics;
        }

        LineTable lines;
        lines.Build(source);
        for (std::size_t i = 0; i < includes.size(); ++i)
        {
            const auto &include = includes[i];
            const auto &entry = profile.entries[i];
            if (entry.circular && !include.conditional && entry.target == include.target)
            {
                const auto position = lines.Lookup(include.target_offset);
                diagnostics.push_back({RuleId::CircularInclude, Severity::Error, "cpp/no-circular-include",
                    "circular include: " + include.target + " includes this file again, directly or indirectly",
                    include.target_offset, include.target_length, position.line, position.column, false, {}});
                continue;
            }

            if (!entry.eligible || include.conditional || include.keep || entry.target != include.target)
            {
                continue;
            }

            // Names of this header the file actually uses.
            std::vector<std::string_view> matched;
            for (const std::string_view word: used)
            {
                if (std::any_of(entry.providers.begin(), entry.providers.end(),
                    [&](const auto &provider) { return provider->names.contains(word); }))
                {
                    matched.push_back(word);
                }
            }

            std::sort(matched.begin(), matched.end());
            const auto position = lines.Lookup(include.target_offset);
            if (matched.empty())
            {
                Diagnostic diagnostic{RuleId::UnusedInclude, Severity::Warning, "cpp/no-unused-include",
                    "included header " + include.target + " is not used directly",
                    include.target_offset, include.target_length, position.line, position.column, true,
                    RemoveDirectiveLine(source, include.directive_offset, include.directive_length)};
                diagnostic.fix_is_safe = false;
                diagnostic.fix_title = "Remove unused include " + include.target;
                diagnostics.push_back(std::move(diagnostic));
                continue;
            }

            if (!profile.is_header_file || !entry.project_header)
            {
                continue;
            }

            if (auto replacement = ForwardDeclarations(tree, entry, matched))
            {
                std::string names;
                for (const std::string_view word: matched)
                {
                    names += (names.empty() ? "" : ", ");
                    names += word;
                }

                const bool crlf = source.find("\r\n") != std::string_view::npos;
                std::string text;
                for (const auto & line: *replacement)
                {
                    text += line;
                    text += crlf ? "\r\n" : "\n";
                }

                auto edit = RemoveDirectiveLine(source, include.directive_offset, include.directive_length);
                edit.replacement = std::move(text);
                Diagnostic diagnostic{RuleId::PreferForwardDeclaration, Severity::Warning,
                    "cpp/prefer-forward-declaration",
                    "include of " + include.target + " is only needed for pointers or references to " + names +
                    "; forward declare " + (matched.size() == 1 ? "it" : "them") + " instead",
                    include.target_offset, include.target_length, position.line, position.column, true,
                    std::move(edit)};
                diagnostic.fix_is_safe = false;
                diagnostic.fix_title = "Forward declare " + names + " instead of including " + include.target;
                diagnostics.push_back(std::move(diagnostic));
            }
        }

        return diagnostics;
    }

} // namespace heimdall
