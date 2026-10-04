#include <Heimdall/LineTable.hpp>
#include <Heimdall/Lexer.hpp>
#include <Heimdall/Preprocessor.hpp>
#include <Heimdall/RuleEngine.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace heimdall
{

    namespace
    {

        bool IsInDirective(std::size_t offset, const std::vector<PreprocessorDirective> & directives,
            std::size_t & cursor)
        {
            while (cursor < directives.size() && directives[cursor].offset + directives[cursor].length <= offset)
            {
                ++cursor;
            }

            return cursor < directives.size() && directives[cursor].offset <= offset &&
                offset < directives[cursor].offset + directives[cursor].length;
        }

        Diagnostic MakeDiagnostic(RuleId rule, std::string code, std::string message, std::size_t offset,
            std::size_t length, LineTable::Position position, TextEdit fix, bool has_fix = true)
        {
            return {rule, Severity::Warning, std::move(code), std::move(message), offset, length,
                position.line, position.column, has_fix, std::move(fix)};
        }

        struct Suppression
        {
            std::uint32_t line;
            std::string_view codes;
        };

        bool Suppresses(std::string_view codes, std::string_view code)
        {
            while (!codes.empty())
            {
                while (!codes.empty() && (codes.front() == ' ' || codes.front() == '\t' || codes.front() == ','))
                {
                    codes.remove_prefix(1);
                }

                const auto end = codes.find_first_of(" ,\t");
                const auto item = codes.substr(0, end);
                if (item == code || item == "*")
                {
                    return true;
                }

                if (end == std::string_view::npos)
                {
                    break;
                }

                codes.remove_prefix(end + 1);
            }

            return false;
        }

        std::vector<Suppression> FindSuppressions(std::string_view source,
            const std::vector<Token> & tokens,
            const LineTable &lines)
        {
            constexpr std::string_view line_marker = "heimdall-disable-line";
            constexpr std::string_view next_marker = "heimdall-disable-next-line";
            std::vector<Suppression> result;
            for (const auto & token: tokens)
            {
                if (token.kind != TokenKind::LineComment && token.kind != TokenKind::BlockComment)
                {
                    continue;
                }

                const auto text = source.substr(token.offset, token.length);
                auto marker = text.find(next_marker);
                std::uint32_t target = lines.Lookup(token.offset).line + 1;
                std::size_t marker_length = next_marker.size();
                if (marker == std::string_view::npos)
                {
                    marker = text.find(line_marker);
                    target = lines.Lookup(token.offset).line;
                    marker_length = line_marker.size();
                }

                if (marker == std::string_view::npos)
                {
                    continue;
                }

                std::size_t begin = marker + marker_length;
                while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == ':'))
                {
                    ++begin;
                }

                auto end = text.find("*/", begin);
                if (end == std::string_view::npos)
                {
                    end = text.size();
                }

                result.push_back({target, text.substr(begin, end - begin)});
            }

            return result;
        }

        std::size_t NextSignificant(const std::vector<Token> & tokens, std::size_t index)
        {
            while (index < tokens.size() &&
                (tokens[index].kind == TokenKind::Whitespace ||
                tokens[index].kind == TokenKind::LineComment ||
                tokens[index].kind == TokenKind::BlockComment))
            {
                ++index;
            }

            return index;
        }

        std::size_t PrevSignificant(const std::vector<Token> & tokens, std::size_t index)
        {
            while (index > 0)
            {
                --index;
                if (tokens[index].kind != TokenKind::Whitespace &&
                    tokens[index].kind != TokenKind::LineComment &&
                    tokens[index].kind != TokenKind::BlockComment)
                {
                    return index;
                }
            }

            return tokens.size();
        }

        std::string_view PunctuationText(std::string_view source, const Token &token)
        {
            return source.substr(token.offset, token.length);
        }

        // Literal include target with its delimiters, e.g. <vector> or
        // "app.h". Returns nullopt for #include_next, macro includes
        // (#include MACRO) and anything without a literal target.
        struct IncludeTarget
        {
            std::string_view text;
            bool angle;
        };

        std::optional<IncludeTarget> ReadIncludeTarget(std::string_view source,
            const PreprocessorDirective & directive)
        {
            std::string_view body = source.substr(directive.offset, directive.length);
            while (!body.empty() && (body.front() == ' ' || body.front() == '\t'))
            {
                body.remove_prefix(1);
            }

            if (!body.empty() && body.front() == '#')
            {
                body.remove_prefix(1);
            }

            while (!body.empty() && (body.front() == ' ' || body.front() == '\t'))
            {
                body.remove_prefix(1);
            }

            std::size_t name_length = 0;
            while (name_length < body.size() &&
                ((body[name_length] >= 'a' && body[name_length] <= 'z') ||
                    (body[name_length] >= 'A' && body[name_length] <= 'Z') ||
                    body[name_length] == '_'))
            {
                ++name_length;
            }

            if (body.substr(0, name_length) != "include")
            {
                return std::nullopt;
            }

            body.remove_prefix(name_length);

            const auto open = body.find_first_of("<\"");
            if (open == std::string_view::npos)
            {
                return std::nullopt;
            }

            const auto close_char = body[open] == '<' ? '>' : '"';
            const auto close = body.find(close_char, open + 1);
            if (close == std::string_view::npos)
            {
                return std::nullopt;
            }

            return IncludeTarget{body.substr(open, close - open + 1), body[open] == '<'};
        }

        bool CaseInsensitiveLess(std::string_view left, std::string_view right)
        {
            const auto common = std::min(left.size(), right.size());
            for (std::size_t i = 0; i < common; ++i)
            {
                const auto a = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(left[i])));
                const auto b = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(right[i])));
                if (a != b)
                {
                    return a < b;
                }
            }
            return left.size() < right.size();
        }

        // Tokens between the alias and the ';' must be only array
        // subscripts: zero or more [ <bound> ] groups. Anything else
        // (attributes, initializers, function declarators) is left to
        // a real parser.
        bool IsPlainArrayDeclarator(const std::vector<Token> & tokens, std::string_view source,
            std::size_t declarator, std::size_t end)
        {
            std::size_t j = NextSignificant(tokens, declarator + 1);
            while (j < end)
            {
                if (tokens[j].kind != TokenKind::Punctuation ||
                    PunctuationText(source, tokens[j]) != "[")
                {
                    return false;
                }

                const auto bound = NextSignificant(tokens, j + 1);
                if (bound >= end ||
                    (tokens[bound].kind != TokenKind::Number &&
                    tokens[bound].kind != TokenKind::Identifier))
                {
                    return false;
                }

                const auto close = NextSignificant(tokens, bound + 1);
                if (close >= end || tokens[close].kind != TokenKind::Punctuation ||
                    PunctuationText(source, tokens[close]) != "]")
                {
                    return false;
                }

                j = NextSignificant(tokens, close + 1);
            }

            return true;
        }

        bool IsWordChar(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_';
        }

        // First uppercase TODO/FIXME/XXX whole-word marker in a comment,
        // or nullopt. Matching is case-sensitive: lowercase prose
        // ("todo list", "fixme later?") stays quiet.
        std::optional<std::string_view> FindTodoMarker(std::string_view text)
        {
            constexpr std::string_view markers[] = {"TODO", "FIXME", "XXX"};
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                for (const auto marker : markers)
                {
                    if (i + marker.size() > text.size() || text.substr(i, marker.size()) != marker)
                    {
                        continue;
                    }

                    const bool left_ok = i == 0 || !IsWordChar(text[i - 1]);
                    const bool right_ok = i + marker.size() == text.size() ||
                        !IsWordChar(text[i + marker.size()]);
                    if (left_ok && right_ok)
                    {
                        return marker;
                    }
                }
            }

            return std::nullopt;
        }

        // Numeric literals that need no name: 0 and 1 in any base or
        // spelling (0x0, 0b1, 1u, 0.0, 1.0f, 1e0...). Anything else,
        // including user-defined literal suffixes and unparseable
        // spellings, is treated as a magic number.
        bool IsTrivialNumericLiteral(std::string_view text)
        {
            std::string clean;
            clean.reserve(text.size());
            for (const char c : text)
            {
                if (c != '\'')
                {
                    clean.push_back(c);
                }
            }

            if (clean.empty())
            {
                return false;
            }

            const bool hex = clean.size() > 2 && clean[0] == '0' &&
                (clean[1] == 'x' || clean[1] == 'X');
            const bool binary = clean.size() > 2 && clean[0] == '0' &&
                (clean[1] == 'b' || clean[1] == 'B');
            bool is_float = false;
            if (hex)
            {
                is_float = clean.find('.') != std::string::npos ||
                    clean.find('p') != std::string::npos || clean.find('P') != std::string::npos;
            }
            else if (!binary)
            {
                is_float = clean.find('.') != std::string::npos ||
                    clean.find('e') != std::string::npos || clean.find('E') != std::string::npos;
            }

            errno = 0;
            if (is_float)
            {
                char *end = nullptr;
                const double value = std::strtod(clean.c_str(), &end);
                if (end == clean.c_str() || errno == ERANGE)
                {
                    return false;
                }

                const std::string_view suffix(end);
                if (!suffix.empty() && suffix != "f" && suffix != "F" &&
                    suffix != "l" && suffix != "L")
                {
                    return false;
                }

                return value == 0.0 || value == 1.0;
            }

            const char *digits = clean.c_str();
            int base = 10;
            if (hex || binary)
            {
                digits += 2;
                base = hex ? 16 : 2;
            }
            else if (clean.size() > 1 && clean[0] == '0' &&
                clean[1] >= '0' && clean[1] <= '9')
            {
                base = 8;
            }

            char *end = nullptr;
            const unsigned long long value = std::strtoull(digits, &end, base);
            if (end == digits || errno == ERANGE)
            {
                return false;
            }

            for (const char *p = end; *p != '\0'; ++p)
            {
                if (*p != 'u' && *p != 'U' && *p != 'l' && *p != 'L' &&
                    *p != 'z' && *p != 'Z')
                {
                    return false;
                }
            }

            return value <= 1;
        }

        bool IsPunctuation(const std::vector<Token> & tokens, std::string_view source,
            std::size_t index, std::string_view text)
        {
            return index < tokens.size() && tokens[index].kind == TokenKind::Punctuation &&
                PunctuationText(source, tokens[index]) == text;
        }

        // A literal that gives a named constant its value is not magic:
        // `constexpr std::size_t kN = 512;`, `const int kMax{100};`,
        // `enum E { A = 3 };`. The literal must be the whole initializer
        // (after an optional sign or parentheses), and the declaration
        // must contain const/constexpr/enum.
        bool IsNamedConstantInitializer(const std::vector<Token> & tokens, std::string_view source,
            std::size_t index)
        {
            std::size_t trigger = PrevSignificant(tokens, index);
            int parens = 0;
            for (int hops = 0; hops < 4 && trigger < tokens.size() &&
                tokens[trigger].kind == TokenKind::Punctuation; ++hops)
            {
                const auto text = PunctuationText(source, tokens[trigger]);
                if (text == "-" || text == "+")
                {
                    trigger = PrevSignificant(tokens, trigger);
                }
                else if (text == "(")
                {
                    ++parens;
                    trigger = PrevSignificant(tokens, trigger);
                }
                else
                {
                    break;
                }
            }

            if (trigger >= tokens.size() || tokens[trigger].kind != TokenKind::Punctuation)
            {
                return false;
            }

            const auto opener = PunctuationText(source, tokens[trigger]);
            if (opener != "=" && opener != "{")
            {
                return false;
            }

            std::size_t after = NextSignificant(tokens, index + 1);
            for (int depth = 0; depth < parens; ++depth)
            {
                if (!IsPunctuation(tokens, source, after, ")"))
                {
                    return false;
                }

                after = NextSignificant(tokens, after + 1);
            }

            if (after < tokens.size())
            {
                if (tokens[after].kind != TokenKind::Punctuation)
                {
                    return false;
                }

                const auto closer = PunctuationText(source, tokens[after]);
                if (closer != ";" && closer != "," && closer != "}" && closer != ")")
                {
                    return false;
                }
            }

            // Walk back to the start of the declaration looking for
            // const/constexpr. A `{` on the way means an enum body or a
            // braced scope: only an enum still names the value.
            std::size_t cursor = trigger;
            for (int steps = 0; steps < 25; ++steps)
            {
                cursor = PrevSignificant(tokens, cursor);
                if (cursor >= tokens.size())
                {
                    return false;
                }

                if (tokens[cursor].kind == TokenKind::Punctuation)
                {
                    const auto text = PunctuationText(source, tokens[cursor]);
                    if (text == "{")
                    {
                        for (int inner = 0; inner < 6; ++inner)
                        {
                            cursor = PrevSignificant(tokens, cursor);
                            if (cursor >= tokens.size())
                            {
                                return false;
                            }

                            if (tokens[cursor].kind == TokenKind::Punctuation)
                            {
                                const auto boundary = PunctuationText(source, tokens[cursor]);
                                if (boundary == "(" || boundary == ")" || boundary == ";" ||
                                    boundary == "}" || boundary == "{")
                                {
                                    return false;
                                }

                                continue;
                            }

                            if (tokens[cursor].kind == TokenKind::Identifier &&
                                source.substr(tokens[cursor].offset, tokens[cursor].length) == "enum")
                            {
                                return true;
                            }
                        }

                        return false;
                    }

                    if (text == ";" || text == "}" || text == "{")
                    {
                        return false;
                    }

                    continue;
                }

                if (tokens[cursor].kind == TokenKind::Identifier)
                {
                    const auto word = source.substr(tokens[cursor].offset, tokens[cursor].length);
                    if (word == "const" || word == "constexpr")
                    {
                        return true;
                    }
                }
            }

            return false;
        }

    } // namespace

    const std::vector<RuleInfo> & RuleCatalog()
    {
        static const std::vector<RuleInfo> catalog =
            {
            {
                RuleId::NullMacro, "cpp/no-null", "cpp", Severity::Warning, "lexical", true,
                    "use nullptr instead of NULL"
            },
            {RuleId::TrailingWhitespace, "format/no-trailing-whitespace", "format",
                Severity::Warning, "lexical", true, "remove trailing spaces and tabs"},
            {RuleId::MissingFinalNewline, "format/require-final-newline", "format",
                Severity::Warning, "lexical", true, "ensure the file ends with a newline"},
            {RuleId::EmptyCatch, "cpp/no-empty-catch", "cpp", Severity::Warning,
                "sintática", false, "catch block that ignores the exception"},
            {RuleId::DuplicateInclude, "cpp/no-duplicate-include", "cpp", Severity::Warning,
                "diretivas", false, "same header included more than once"},
            {RuleId::LegacyTypedef, "cpp/modernize-using", "cpp", Severity::Warning,
                "sintática", true, "replace typedef with a using alias"},
            {RuleId::UnusedInclude, "cpp/no-unused-include", "cpp", Severity::Warning,
                "semântica", false, "included header whose symbols are never used"},
            {RuleId::PreferForwardDeclaration, "cpp/prefer-forward-declaration", "cpp", Severity::Warning,
                "semântica", false, "header included only for pointers or references to its classes"},
            {RuleId::CircularInclude, "cpp/no-circular-include", "cpp", Severity::Error,
                "semântica", false, "include that leads back to the including file"},
            {RuleId::UnsortedIncludes, "cpp/sort-includes", "cpp", Severity::Warning,
                "diretivas", true, "includes out of the configured order"},
            {RuleId::TodoComment, "cpp/no-todo", "cpp", Severity::Warning,
                "lexical", false, "unresolved TODO, FIXME or XXX comment"},
            {RuleId::MagicNumber, "cpp/no-magic-numbers", "cpp", Severity::Warning,
                "sintática", false, "numeric literal without a named constant"},
        };
        return catalog;
    }

    const RuleInfo * FindRuleByCode(std::string_view code)
    {
        const auto &catalog = RuleCatalog();
        const auto it = std::find_if(catalog.begin(), catalog.end(),
            [code](const RuleInfo &info)
            {
                return info.code == code;
        });
        return it == catalog.end() ? nullptr : & *it;
    }

    const RuleInfo * FindRule(RuleId id)
    {
        const auto &catalog = RuleCatalog();
        const auto it = std::find_if(catalog.begin(), catalog.end(),
            [id](const RuleInfo &info)
            {
                return info.id == id;
        });
        return it == catalog.end() ? nullptr : & *it;
    }

    bool IsKnownRuleCode(std::string_view code)
    {
        return FindRuleByCode(code) != nullptr;
    }

    TextEdit RemoveDirectiveLine(std::string_view source, std::size_t offset, std::size_t length)
    {
        std::size_t begin = std::min(offset, source.size());
        while (begin > 0 && (source[begin - 1] == ' ' || source[begin - 1] == 0x09))
        {
            --begin;
        }

        std::size_t end = std::min(offset + length, source.size());
        std::size_t probe = end;
        while (probe < source.size() && (source[probe] == ' ' || source[probe] == 0x09))
        {
            ++probe;
        }

        if (probe == source.size())
        {
            end = probe;
        }
        else if (source[probe] == 0x0A)
        {
            end = probe + 1;
        }
        else if (source[probe] == 0x0D && probe + 1 < source.size() && source[probe + 1] == 0x0A)
        {
            end = probe + 2;
        }

        return {begin, end - begin, ""};
    }

    bool RuleEngine::RuleEnabled(std::string_view code, bool default_enabled) const
    {
        bool enabled = default_enabled;
        for (auto override = m_options.overrides.rbegin(); override != m_options.overrides.rend(); ++override)
        {
            if (override->code == code)
            {
                enabled = override->enabled;
                break;
            }
        }
        return enabled;
    }

    std::vector<Diagnostic> RuleEngine::Analyze(std::string_view source) const
    {
        const auto tokens = Lexer(source).Lex();
        const auto directives = Preprocessor().Process(source).directives;
        return AnalyzeImpl(source, tokens, directives);
    }

    std::vector<Diagnostic> RuleEngine::Analyze(const ParseTree &tree) const
    {
        return AnalyzeImpl(tree.Source(), tree.Tokens(), tree.Directives());
    }

    std::vector<Diagnostic> RuleEngine::AnalyzeImpl(std::string_view source,
        const std::vector<Token> & tokens,
        const std::vector<PreprocessorDirective> & directives) const
    {
        std::vector<Diagnostic> diagnostics;
        LineTable lines;
        lines.Build(source);

        if (m_options.null_macro)
        {
            std::size_t directive_cursor = 0;
            for (const auto & token: tokens)
            {
                if (token.kind != TokenKind::Identifier || IsInDirective(token.offset, directives,
                    directive_cursor))
                {
                    continue;
                }

                if (source.substr(token.offset, token.length) == "NULL")
                {
                    const auto position = lines.Lookup(token.offset);
                    diagnostics.push_back(MakeDiagnostic(
                        RuleId::NullMacro, "cpp/no-null", "use nullptr instead of NULL", token.offset, token.length,
                        position, {token.offset, token.length, "nullptr"}));
                }
            }
        }

        if (m_options.trailing_whitespace)
        {
            std::size_t line_start = 0;
            while (line_start < source.size())
            {
                std::size_t line_end = source.find('\n', line_start);
                if (line_end == std::string_view::npos)
                {
                    line_end = source.size();
                }

                std::size_t content_end = line_end;
                if (content_end > line_start && source[content_end - 1] == '\r')
                {
                    --content_end;
                }

                std::size_t trim_end = content_end;
                while (trim_end > line_start && (source[trim_end - 1] == ' ' || source[trim_end - 1] == '\t'))
                {
                    --trim_end;
                }

                if (trim_end < content_end)
                {
                    const auto position = lines.Lookup(trim_end);
                    diagnostics.push_back(MakeDiagnostic(
                        RuleId::TrailingWhitespace, "format/no-trailing-whitespace", "trailing whitespace", trim_end,
                        content_end - trim_end, position, {trim_end, content_end - trim_end, ""}));
                }

                line_start = line_end == source.size() ? source.size() : line_end + 1;
            }
        }

        if (m_options.final_newline && !source.empty() && source.back() != '\n')
        {
            const auto position = lines.Lookup(source.size());
            const bool crlf = source.find("\r\n") != std::string_view::npos;
            diagnostics.push_back(MakeDiagnostic(
                RuleId::MissingFinalNewline, "format/require-final-newline", "file must end with a newline",
                source.size(), 0,
                position, {source.size(), 0, crlf ? "\r\n" : "\n"}));
        }

        if (m_options.todo_comment)
        {
            for (const auto & token : tokens)
            {
                if (token.kind != TokenKind::LineComment &&
                    token.kind != TokenKind::BlockComment)
                {
                    continue;
                }

                const auto marker = FindTodoMarker(source.substr(token.offset, token.length));
                if (!marker)
                {
                    continue;
                }

                const auto position = lines.Lookup(token.offset);
                auto diagnostic = MakeDiagnostic(
                    RuleId::TodoComment, "cpp/no-todo",
                    std::string(*marker) + " comment should be resolved or tracked",
                    token.offset, token.length, position, {0, 0, ""}, false);
                diagnostics.push_back(std::move(diagnostic));
            }
        }

        if (m_options.magic_numbers)
        {
            std::size_t directive_cursor = 0;
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                const auto & token = tokens[i];
                if (token.kind != TokenKind::Number ||
                    IsInDirective(token.offset, directives, directive_cursor))
                {
                    continue;
                }

                const auto text = source.substr(token.offset, token.length);
                if (IsTrivialNumericLiteral(text) || IsNamedConstantInitializer(tokens, source, i))
                {
                    continue;
                }

                const auto position = lines.Lookup(token.offset);
                auto diagnostic = MakeDiagnostic(
                    RuleId::MagicNumber, "cpp/no-magic-numbers",
                    "magic number '" + std::string(text) + "' should use a named constant",
                    token.offset, token.length, position, {0, 0, ""}, false);
                diagnostics.push_back(std::move(diagnostic));
            }
        }

        if (m_options.empty_catch)
        {
            std::size_t directive_cursor = 0;
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                const auto &token = tokens[i];
                if (token.kind != TokenKind::Identifier ||
                    source.substr(token.offset, token.length) != "catch" ||
                    IsInDirective(token.offset, directives, directive_cursor))
                {
                    continue;
                }

                // catch ( <exception declaration> ) { }
                const std::size_t open_paren = NextSignificant(tokens, i + 1);
                if (open_paren >= tokens.size() || tokens[open_paren].kind != TokenKind::Punctuation ||
                    PunctuationText(source, tokens[open_paren]) != "(")
                {
                    continue;
                }

                std::size_t depth = 0;
                std::size_t close_paren = tokens.size();
                for (std::size_t j = open_paren; j < tokens.size(); ++j)
                {
                    const auto &candidate = tokens[j];
                    if (candidate.kind != TokenKind::Punctuation)
                    {
                        continue;
                    }

                    const auto text = PunctuationText(source, candidate);
                    if (text == "(")
                    {
                        ++depth;
                    }
                    else if (text == ")" && --depth == 0)
                    {
                        close_paren = j;
                        break;
                    }
                }

                if (close_paren == tokens.size())
                {
                    continue;
                }

                const std::size_t open_brace = NextSignificant(tokens, close_paren + 1);
                if (open_brace >= tokens.size() || tokens[open_brace].kind != TokenKind::Punctuation ||
                    PunctuationText(source, tokens[open_brace]) != "{")
                {
                    continue;
                }

                const std::size_t close_brace = NextSignificant(tokens, open_brace + 1);
                if (close_brace >= tokens.size() || tokens[close_brace].kind != TokenKind::Punctuation ||
                    PunctuationText(source, tokens[close_brace]) != "}")
                {
                    continue;
                }

                const auto offset = tokens[open_brace].offset;
                const auto length = tokens[close_brace].offset + tokens[close_brace].length - offset;

                const auto interior_begin = tokens[open_brace].offset + tokens[open_brace].length;
                const auto interior_end = tokens[close_brace].offset;
                TextEdit edit{interior_begin, interior_end - interior_begin, " throw; "};
                if (std::any_of(tokens.begin() + open_brace + 1, tokens.begin() + close_brace,
                    [](const Token &t)
                    {
                        return t.kind != TokenKind::Whitespace;
                }))
                {
                    edit = {interior_end, 0, "throw; "};
                }

                auto diagnostic = MakeDiagnostic(
                    RuleId::EmptyCatch, "cpp/no-empty-catch",
                    "empty catch block ignores the exception", offset, length,
                    lines.Lookup(offset), std::move(edit));
                diagnostic.fix_is_safe = false;
                diagnostic.fix_title = "Rethrow the exception in the empty catch block";
                diagnostics.push_back(std::move(diagnostic));
            }
        }

        if (m_options.duplicate_include)
        {
            // Only includes outside any conditional block are compared: the
            // same header in different #ifdef branches is usually intentional.
            std::size_t conditional_depth = 0;
            std::unordered_map<std::string_view, std::size_t> seen;
            for (const auto & directive: directives)
            {
                switch (directive.kind)
                {
                case DirectiveKind::If:
                case DirectiveKind::Ifdef:
                case DirectiveKind::Ifndef:
                    ++conditional_depth;
                    continue;
                case DirectiveKind::Endif:
                    conditional_depth = conditional_depth == 0 ? 0 : conditional_depth - 1;
                    continue;
                default:
                    break;
                }

                if (directive.kind != DirectiveKind::Include || conditional_depth != 0)
                {
                    continue;
                }

                // #include <target> or #include "target"; macro includes
                // (#include MACRO) have no literal target to compare.
                const auto target = ReadIncludeTarget(source, directive);
                if (!target)
                {
                    continue;
                }

                const auto offset = static_cast<std::size_t>(
                    target->text.data() - source.data());
                const auto[it, inserted] = seen.try_emplace(target->text, directive.offset);
                if (!inserted)
                {
                    // Not safe in batch: a #define/#undef between the includes
                    // may make the second one meaningful (X-macro headers).
                    auto diagnostic = MakeDiagnostic(
                        RuleId::DuplicateInclude, "cpp/no-duplicate-include",
                        "duplicate include of " + std::string(target->text), offset, target->text.length(),
                        lines.Lookup(offset), RemoveDirectiveLine(source, directive.offset, directive.length));
                    diagnostic.fix_is_safe = false;
                    diagnostic.fix_title = "Remove duplicate include of " + std::string(target->text);
                    diagnostics.push_back(std::move(diagnostic));
                }
            }
        }

        if (m_options.legacy_typedef)
        {
            std::size_t directive_cursor = 0;
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                const auto &token = tokens[i];
                if (token.kind != TokenKind::Identifier ||
                    source.substr(token.offset, token.length) != "typedef" ||
                    IsInDirective(token.offset, directives, directive_cursor))
                {
                    continue;
                }

                // Only plain declarators are rewritten: function pointers,
                // class definitions and multiple declarators need parsing
                // beyond the statement's token shape.
                std::size_t angle_depth = 0;
                bool has_paren = false;
                bool has_brace = false;
                bool has_comma = false;
                std::size_t declarator = 0;
                std::size_t end = tokens.size();
                for (std::size_t j = i + 1; j < tokens.size(); ++j)
                {
                    const auto &candidate = tokens[j];
                    if (candidate.kind == TokenKind::Whitespace ||
                        candidate.kind == TokenKind::LineComment ||
                        candidate.kind == TokenKind::BlockComment)
                    {
                        continue;
                    }

                    if (candidate.kind == TokenKind::Punctuation)
                    {
                        const auto text = PunctuationText(source, candidate);
                        if (text == ";")
                        {
                            end = j;
                            break;
                        }

                        if (text == "(" || text == ")")
                        {
                            has_paren = true;
                        }
                        else if (text == "{" || text == "}")
                        {
                            has_brace = true;
                        }
                        else if (text == "," && angle_depth == 0)
                        {
                            has_comma = true;
                        }

                        for (const char c: text)
                        {
                            if (c == '<')
                            {
                                ++angle_depth;
                            }
                            else if (c == '>')
                            {
                                angle_depth = angle_depth == 0 ? 0 : angle_depth - 1;
                            }
                        }
                    }
                    else if (candidate.kind == TokenKind::Identifier)
                    {
                        declarator = j;
                    }
                }

                if (end == tokens.size() || has_paren || has_brace || has_comma)
                {
                    continue;
                }

                // The alias is the identifier after which only
                // array subscripts follow.
                std::size_t alias = 0;
                for (std::size_t j = i + 1; j < end; ++j)
                {
                    if (tokens[j].kind != TokenKind::Identifier)
                    {
                        continue;
                    }

                    if (IsPlainArrayDeclarator(tokens, source, j, end))
                    {
                        alias = j;
                        break;
                    }
                }

                if (alias == 0)
                {
                    continue;
                }

                const auto name = source.substr(tokens[alias].offset, tokens[alias].length);
                auto before = source.substr(tokens[i + 1].offset,
                    tokens[alias].offset - tokens[i + 1].offset);
                while (!before.empty() && (before.front() == ' ' || before.front() == '\t' ||
                    before.front() == '\n' || before.front() == '\r'))
                {
                    before.remove_prefix(1);
                }

                while (!before.empty() && (before.back() == ' ' || before.back() == '\t' ||
                    before.back() == '\n' || before.back() == '\r'))
                {
                    before.remove_suffix(1);
                }

                const auto after_begin = tokens[alias].offset + tokens[alias].length;
                auto after = source.substr(after_begin, tokens[end].offset - after_begin);
                while (!after.empty() && (after.front() == ' ' || after.front() == '\t' ||
                    after.front() == '\n' || after.front() == '\r'))
                {
                    after.remove_prefix(1);
                }

                if (before.empty())
                {
                    continue;
                }

                const auto offset = tokens[i].offset;
                const auto length = tokens[end].offset + tokens[end].length - offset;
                std::string replacement = "using ";
                replacement.append(name);
                replacement += " = ";
                replacement.append(before);
                replacement.append(after);
                replacement += ";";
                diagnostics.push_back(MakeDiagnostic(
                    RuleId::LegacyTypedef, "cpp/modernize-using",
                    "replace typedef with a using alias", offset, length,
                    lines.Lookup(offset),
                    {offset, length, std::move(replacement)}));
            }
        }

        if (RuleEnabled("cpp/sort-includes", m_options.sort_includes))
        {
            // Literal includes only: #include_next and macro includes
            // have no target to order.
            struct IncludeEntry
            {
                std::size_t directive;
                IncludeTarget target;
            };
            std::vector<IncludeEntry> includes;
            for (std::size_t i = 0; i < directives.size(); ++i)
            {
                if (directives[i].kind != DirectiveKind::Include)
                {
                    continue;
                }

                auto target = ReadIncludeTarget(source, directives[i]);
                if (target)
                {
                    includes.push_back({i, *target});
                }
            }

            auto group_index = [this](const IncludeTarget &target)
            {
                const auto &order = m_options.include_order;
                const auto wanted = target.angle ? IncludeGroup::Angle
                    : IncludeGroup::Quote;
                const auto it = std::find(order.begin(), order.end(), wanted);
                return it == order.end() ? order.size()
                    : static_cast<std::size_t>(it - order.begin());
            };

            auto sorts_before = [&](const IncludeTarget &left,
                const IncludeTarget &right)
            {
                const auto left_group = group_index(left);
                const auto right_group = group_index(right);
                if (left_group != right_group)
                {
                    return left_group < right_group;
                }

                const auto left_name = left.text.substr(1, left.text.size() - 2);
                const auto right_name = right.text.substr(1, right.text.size() - 2);
                return m_options.include_case_insensitive
                    ? CaseInsensitiveLess(left_name, right_name)
                    : left_name < right_name;
            };

            // A block is a run of includes on adjacent lines. Blank
            // lines, comments and other directives start a new block,
            // so reordering never crosses them.
            std::size_t block_begin = 0;
            while (block_begin < includes.size())
            {
                std::size_t block_end = block_begin + 1;
                while (block_end < includes.size())
                {
                    const auto &previous = directives[includes[block_end - 1].directive];
                    const auto &current = directives[includes[block_end].directive];
                    if (current.offset != previous.offset + previous.length)
                    {
                        break;
                    }

                    ++block_end;
                }

                if (block_end - block_begin >= 2)
                {
                    std::vector<std::size_t> order(block_end - block_begin);
                    for (std::size_t i = 0; i < order.size(); ++i)
                    {
                        order[i] = i;
                    }

                    std::stable_sort(order.begin(), order.end(),
                        [&](std::size_t left, std::size_t right)
                        {
                            return sorts_before(includes[block_begin + left].target,
                                includes[block_begin + right].target);
                        });

                    // The identity permutation means the block already
                    // follows the configured order.
                    if (!std::is_sorted(order.begin(), order.end()))
                    {
                        const auto &first = directives[includes[block_begin].directive];
                        const auto &last = directives[includes[block_end - 1].directive];
                        const auto offset = first.offset;
                        const auto length = last.offset + last.length - offset;
                        std::string replacement;
                        for (const auto index: order)
                        {
                            const auto &directive = directives[includes[block_begin + index].directive];
                            replacement.append(source.substr(directive.offset, directive.length));
                        }

                        // Safe in batch: the configured order is the
                        // project's declared convention, and the fix
                        // only reorders includes inside one block.
                        diagnostics.push_back(MakeDiagnostic(
                            RuleId::UnsortedIncludes, "cpp/sort-includes",
                            "includes are not in the configured order", offset, length,
                            lines.Lookup(offset), {offset, length, std::move(replacement)}));
                    }
                }

                block_begin = block_end;
            }
        }

        return ApplyPolicy(std::move(diagnostics), source, tokens);
    }

    std::vector<Diagnostic> RuleEngine::ApplyPolicy(std::vector<Diagnostic> diagnostics,
        std::string_view source, const std::vector<Token> & tokens) const
    {
        LineTable lines;
        lines.Build(source);

        std::sort(diagnostics.begin(), diagnostics.end(),[](const Diagnostic &a, const Diagnostic &b)
            {
                return a.offset < b.offset;
        });

        for (auto & diagnostic: diagnostics)
        {
            for (auto override = m_options.overrides.rbegin(); override != m_options.overrides.rend(); ++override)
            {
                if (override->code == diagnostic.code)
                {
                    diagnostic.severity = override -> severity;
                    break;
                }
            }
        }

        std::erase_if(diagnostics,[this](const Diagnostic &diagnostic)
            {
                for (auto override = m_options.overrides.rbegin(); override != m_options.overrides.rend(); ++override)
                {
                    if (override->code == diagnostic.code)
                    {
                        return!override -> enabled;
                }
            }

                return false;
        });
        if (m_options.honor_suppressions && !diagnostics.empty())
        {
            const auto suppressions = FindSuppressions(source, tokens, lines);
            std::erase_if(diagnostics,[&suppressions](const Diagnostic &diagnostic)
                {
                    return std::any_of(suppressions.begin(), suppressions.end(),[&diagnostic](const Suppression &s)
                    {
                        return s.line == diagnostic.line &&
                        (s.codes.empty() || Suppresses(s.codes, diagnostic.code));
                });
            });
        }

        return diagnostics;
    }

    std::string RuleEngine::ApplyFixes(std::string_view source,
        const std::vector<Diagnostic> & diagnostics)
    {
        std::vector<const TextEdit * > edits;
        edits.reserve(diagnostics.size());
        for (const auto & diagnostic: diagnostics)
        {
            if (diagnostic.has_fix && diagnostic.fix_is_safe)
            {
                edits.push_back(&diagnostic.fix);
            }
        }

        std::sort(edits.begin(), edits.end(),[](const TextEdit *a, const TextEdit *b)
            {
                return a->offset > b->offset;
        });

        std::string result(source);
        std::size_t previous_start = source.size();
        for (const TextEdit * edit: edits)
        {
            const auto owner = std::find_if(diagnostics.begin(), diagnostics.end(),[edit](const Diagnostic &d)
                {
                    return d.has_fix && &d.fix == edit;
            });
            if (owner == diagnostics.end() || edit->offset < owner->offset ||
                edit->offset - owner->offset > owner->length ||
                edit->length > owner->length -(edit->offset - owner->offset) ||
                edit->offset > source.size() || edit->length > source.size() - edit->offset ||
                edit->offset + edit->length > previous_start)
            {
                continue;
            }

            result.replace(edit->offset, edit->length, edit->replacement);
            previous_start = edit->offset;
        }

        return result;
    }

} // namespace heimdall
