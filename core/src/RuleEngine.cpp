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
#include <unordered_set>
#include <utility>

namespace heimdall
{

    namespace
    {

        constexpr std::size_t kMinPrefixedLen = 2;
        constexpr int kDecimalBase = 10;
        constexpr int kHexBase = 16;
        constexpr int kBinaryBase = 2;
        constexpr int kOctalBase = 8;
        constexpr std::size_t kHexPrefixLen = 2;
        constexpr int kMaxSignHops = 4;
        constexpr int kMaxDeclWalkSteps = 25;
        constexpr int kMaxEnumLookback = 6;
        constexpr std::size_t kIncludeDelimCount = 2;
        constexpr std::size_t kMinSortableBlock = 2;

        bool IsInDirective(std::size_t offset, const std::vector<PreprocessorDirective>& directives,
            std::size_t& cursor)
        {
            while (cursor < directives.size() && directives[cursor].offset + directives[cursor].length <= offset)
            {
                ++cursor;
            }

            return cursor < directives.size() && directives[cursor].offset <= offset &&
                offset < directives[cursor].offset + directives[cursor].length;
        }

        Diagnostic MakeDiagnostic(
            RuleId rule,
            std::string code,
            std::string message,
            std::size_t offset,
            std::size_t length,
            LineTable::Position position,
            TextEdit fix,
            bool has_fix = true)
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
            const std::vector<Token>& tokens,
            const LineTable& lines)
        {
            constexpr std::string_view line_marker = "heimdall-disable-line";
            constexpr std::string_view next_marker = "heimdall-disable-next-line";
            std::vector<Suppression> result;
            for (const auto& token : tokens)
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

        std::size_t NextSignificant(const std::vector<Token>& tokens, std::size_t index)
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

        std::size_t PrevSignificant(const std::vector<Token>& tokens, std::size_t index)
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

        bool IsWord(std::string_view source, const Token& token, std::string_view word)
        {
            return token.kind == TokenKind::Identifier && source.substr(token.offset, token.length) == word;
        }

        bool IsPunct(std::string_view source, const Token& token, std::string_view punct)
        {
            return token.kind == TokenKind::Punctuation && source.substr(token.offset, token.length) == punct;
        }

        // Index of the bracket closing the one at `open`, or tokens.size().
        // `open_text`/`close_text` are single-character punctuators; anything
        // unbalanced gives up so fixes never come from half-parsed code.
        std::size_t MatchBracket(const std::vector<Token>& tokens, std::string_view source,
            std::size_t open, std::string_view open_text, std::string_view close_text)
        {
            std::size_t depth = 0;
            for (std::size_t i = open; i < tokens.size(); ++i)
            {
                const auto& token = tokens[i];
                if (token.kind != TokenKind::Punctuation)
                {
                    continue;
                }

                const auto text = source.substr(token.offset, token.length);
                if (text == open_text)
                {
                    ++depth;
                }
                else if (text == close_text && --depth == 0)
                {
                    return i;
                }
            }

            return tokens.size();
        }

        std::string_view Trimmed(std::string_view text)
        {
            while (!text.empty() &&
                (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' || text.front() == '\r'))
            {
                text.remove_prefix(1);
            }

            while (!text.empty() &&
                (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' || text.back() == '\r'))
            {
                text.remove_suffix(1);
            }

            return text;
        }

        // A `new T(args)` / `new T` / `new T[n]` starting at the `new` keyword.
        struct NewExpression
        {
            std::size_t new_index = 0;
            std::string_view type;
            std::string_view args;
            bool has_parens = false;
            bool is_array = false;
            std::size_t end_index = 0; // last token of the expression
        };

        bool ParseNewExpression(const std::vector<Token>& tokens, std::string_view source,
            std::size_t new_index, NewExpression& out)
        {
            std::size_t i = NextSignificant(tokens, new_index + 1);
            const std::size_t type_begin = i;
            bool has_ident = false;
            while (i < tokens.size() && tokens[i].kind == TokenKind::Identifier)
            {
                has_ident = true;
                const std::size_t after = NextSignificant(tokens, i + 1);
                if (after < tokens.size() && IsPunct(source, tokens[after], "::"))
                {
                    i = NextSignificant(tokens, after + 1);
                    continue;
                }

                break;
            }

            if (!has_ident)
            {
                return false;
            }

            const std::size_t type_end = i;
            out.new_index = new_index;
            out.type = Trimmed(source.substr(tokens[type_begin].offset,
                tokens[type_end].offset + tokens[type_end].length - tokens[type_begin].offset));
            out.args = {};
            out.has_parens = false;
            out.is_array = false;
            const std::size_t after = NextSignificant(tokens, type_end + 1);
            if (after < tokens.size() && IsPunct(source, tokens[after], "["))
            {
                out.is_array = true;
                out.end_index = MatchBracket(tokens, source, after, "[", "]");
                return out.end_index < tokens.size();
            }

            if (after < tokens.size() && IsPunct(source, tokens[after], "("))
            {
                const std::size_t close = MatchBracket(tokens, source, after, "(", ")");
                if (close >= tokens.size())
                {
                    return false;
                }

                out.has_parens = true;
                out.end_index = close;
                const std::size_t args_begin = tokens[after].offset + tokens[after].length;
                out.args = Trimmed(source.substr(args_begin, tokens[close].offset - args_begin));
                return true;
            }

            out.end_index = type_end;
            return true;
        }

        std::string_view PunctuationText(std::string_view source, const Token& token)
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
            const PreprocessorDirective& directive)
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
        bool IsPlainArrayDeclarator(
            const std::vector<Token>& tokens,
            std::string_view source,
            std::size_t declarator,
            std::size_t end)
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
            return (c >= 'a' && c <= 'z') ||(c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_';
        }

        // First uppercase TODO/FIXME/XXX whole-word marker in a comment, // heimdall-disable-line cpp/no-todo -- documents the rule's own markers.
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

                    const bool left_ok = i == 0 ||!IsWordChar(text[i - 1]);
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

            const bool hex = clean.size() > kMinPrefixedLen && clean[0] == '0' &&
                (clean[1] == 'x' || clean[1] == 'X');
            const bool binary = clean.size() > kMinPrefixedLen && clean[0] == '0' &&
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
                char* end = nullptr;
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

            const char* digits = clean.c_str();
            int base = kDecimalBase;
            if (hex || binary)
            {
                digits += kHexPrefixLen;
                base = hex ? kHexBase : kBinaryBase;
            }
            else if (clean.size() > 1 && clean[0] == '0' &&
                clean[1] >= '0' && clean[1] <= '9')
            {
                base = kOctalBase;
            }

            char* end = nullptr;
            const unsigned long long value = std::strtoull(digits, &end, base);
            if (end == digits || errno == ERANGE)
            {
                return false;
            }

            for (const char * p = end; *p != '\0'; ++p)
            {
                if (*p != 'u' && *p != 'U' && *p != 'l' && *p != 'L' &&
                    *p != 'z' && *p != 'Z')
                {
                    return false;
                }
            }

            return value <= 1;
        }

        bool IsPunctuation(
            const std::vector<Token>& tokens,
            std::string_view source,
            std::size_t index,
            std::string_view text)
        {
            return index < tokens.size() && tokens[index].kind == TokenKind::Punctuation &&
                PunctuationText(source, tokens[index]) == text;
        }

        // A literal that gives a named constant its value is not magic:
        // `constexpr std::size_t kN = 512;`, `const int kMax{100};`,
        // `enum E { A = 3 };`. The literal must be the whole initializer
        // (after an optional sign or parentheses), and the declaration
        // must contain const/constexpr/constinit/enum. Note: constinit was
        // a former false positive (flagged `constinit int kX = 42;`); it
        // names its value exactly like const/constexpr, so it is exempt.
        bool IsNamedConstantInitializer(const std::vector<Token>& tokens, std::string_view source,
            std::size_t index)
        {
            std::size_t trigger = PrevSignificant(tokens, index);
            int parens = 0;
            for (int hops = 0; hops < kMaxSignHops && trigger < tokens.size() &&
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
            for (int steps = 0; steps < kMaxDeclWalkSteps; ++steps)
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
                        for (int inner = 0; inner < kMaxEnumLookback; ++inner)
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
                    if (word == "const" || word == "constexpr" || word == "constinit")
                    {
                        return true;
                    }
                }
            }

            return false;
        }

    } // namespace

    const std::vector<RuleInfo>& RuleCatalog()
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
            {RuleId::ModernizeOverride, "cpp/modernize-override", "cpp", Severity::Warning,
                "semântica", true, "overriding virtual function without override"},
            {RuleId::ModernizeNullptr, "cpp/modernize-nullptr", "cpp", Severity::Warning,
                "semântica", true, "explicit cast of a null constant to a pointer type"},
            {RuleId::NoZeroAsNull, "cpp/no-zero-as-null", "cpp", Severity::Warning,
                "semântica", true, "0 used as a null pointer"},
            {RuleId::ModernizeAuto, "cpp/modernize-auto", "cpp", Severity::Warning,
                "semântica", true, "explicit type that repeats the initializer"},
            {RuleId::ModernizeEmplace, "cpp/modernize-emplace", "cpp", Severity::Warning,
                "sintática", true, "push_back of a temporary that emplace_back can build in place"},
            {RuleId::ModernizeMakeUnique, "cpp/modernize-make-unique", "cpp", Severity::Warning,
                "sintática", true, "unique_ptr built from new instead of std::make_unique"},
            {RuleId::ModernizeMakeShared, "cpp/modernize-make-shared", "cpp", Severity::Warning,
                "sintática", true, "shared_ptr built from new instead of std::make_shared"},
            {RuleId::ModernizeSmartPtr, "cpp/modernize-smart-ptr", "cpp", Severity::Warning,
                "sintática", true, "ownership held in a raw pointer instead of a smart pointer"},
            {RuleId::NoNewDelete, "cpp/no-new-delete", "cpp", Severity::Warning,
                "sintática", false, "direct use of new or delete instead of RAII"},
            {RuleId::ModernizeSpan, "cpp/modernize-span", "cpp", Severity::Warning,
                "semântica", true, "pointer and size parameters that std::span can replace"},
            {RuleId::ModernizeStringView, "cpp/modernize-string-view", "cpp", Severity::Warning,
                "semântica", true, "const std::string parameter copied by value instead of std::string_view"},
            {RuleId::ModernizeAlgorithms, "cpp/modernize-algorithms", "cpp", Severity::Warning,
                "sintática", false, "loop that a standard algorithm can replace"},
            {RuleId::ModernizeStructuredBindings, "cpp/modernize-structured-bindings", "cpp", Severity::Warning,
                "sintática", true, "pair or tuple unpacked without structured bindings"},
            {RuleId::ModernizeAttributes, "cpp/modernize-attributes", "cpp", Severity::Warning,
                "semântica", true, "query function whose result should be [[nodiscard]]"},
            {RuleId::ModernizeConstevalConstexpr, "cpp/modernize-consteval-constexpr", "cpp", Severity::Warning,
                "semântica", true, "constant that could be constexpr"},
            {RuleId::NoImplicitBoolConversion, "cpp/no-implicit-bool-conversion", "cpp", Severity::Warning,
                "semântica", false, "integer, floating-point or pointer used as a condition"},
            {RuleId::ModernizeRangeLoop, "cpp/modernize-range-loop", "cpp", Severity::Warning,
                "semântica", true, "index loop replaceable by a range-based for"},
            {RuleId::ModernizeLoopConvert, "cpp/modernize-loop-convert", "cpp", Severity::Warning,
                "semântica", true, "iterator loop replaceable by a range-based for"},
            {RuleId::IncludeWhatYouUse, "cpp/include-what-you-use", "cpp", Severity::Warning,
                "semântica", false, "name used from a header that is only included transitively"},
            {RuleId::ModernizeFinal, "cpp/modernize-final", "cpp", Severity::Warning,
                "semântica", true, "class or virtual function that nothing can derive from or override"},
            {RuleId::ModernizeConst, "cpp/modernize-const", "cpp", Severity::Warning,
                "semântica", true, "local variable that is never modified and could be const"},
            {RuleId::ModernizeConstexpr, "cpp/modernize-constexpr", "cpp", Severity::Warning,
                "semântica", true, "variable or function that could be constexpr"},
            {RuleId::ApiVirtualDestructor, "api/virtual-destructor", "api", Severity::Warning,
                "semântica", true, "polymorphic class whose destructor is not virtual"},
            {RuleId::ApiMissingNodiscard, "api/missing-nodiscard", "api", Severity::Warning,
                "semântica", true, "function whose resource-like result should be [[nodiscard]]"},
            {RuleId::ApiPassByValue, "api/pass-by-value", "api", Severity::Warning,
                "semântica", true, "const reference parameter that is copied and could be taken by value"},
            {RuleId::ApiPassByConstReference, "api/pass-by-const-reference", "api", Severity::Warning,
                "semântica", true, "expensive parameter copied by value but only read"},
            {RuleId::ApiConstCorrectness, "api/const-correctness", "api", Severity::Warning,
                "semântica", true, "member function or reference parameter that could be const"},
            {RuleId::ApiUnsafeDowncast, "api/unsafe-downcast", "api", Severity::Warning,
                "semântica", false, "static_cast downcast without a runtime check"},
            {RuleId::ApiSlicing, "api/slicing", "api", Severity::Warning,
                "semântica", false, "derived object stored or returned by value as its base"},
            {RuleId::ApiImplicitConversion, "api/implicit-conversion", "api", Severity::Warning,
                "semântica", false, "implicit conversion operator that allows accidental conversions"},
            {RuleId::ApiExplicitConstructor, "api/explicit-constructor", "api", Severity::Warning,
                "semântica", true, "constructor callable with one argument that is not explicit"},
            {RuleId::ApiOverloadHiding, "api/overload-hiding", "api", Severity::Warning,
                "semântica", true, "derived function that hides the virtual overloads of a base"},
            {RuleId::ApiVirtualCallInConstructor, "api/virtual-call-in-constructor", "api", Severity::Warning,
                "semântica", false, "virtual call in a constructor or destructor"},
            {RuleId::DesignatedInitOrder, "cpp/designated-init-order", "cpp", Severity::Warning,
                "semântica", true, "designated initializers out of member declaration order"},
            {RuleId::NoIntegerToPointer, "cpp/no-integer-to-pointer", "cpp", Severity::Error,
                "semântica", false, "non-zero integer constant used as a pointer"},
            {RuleId::DocRequireComment, "doc/require-comment", "doc", Severity::Warning,
                "semântica", true, "public class, enum or function without a documentation comment (opt-in)"},
            {RuleId::DocDoxygenStyle, "doc/doxygen-style", "doc", Severity::Warning,
                "semântica", true,
                "Doxygen comment that breaks good practice: brief, @param, @tparam, @return, @throws, style (opt-in)"},
        };
        return catalog;
    }

    const RuleInfo* FindRuleByCode(std::string_view code)
    {
        const auto& catalog = RuleCatalog();
        const auto it = std::find_if(catalog.begin(), catalog.end(),
            [code](const RuleInfo& info)
            {
                return info.code == code;
        });
        return it == catalog.end() ? nullptr : & *it;
    }

    const RuleInfo* FindRule(RuleId id)
    {
        const auto& catalog = RuleCatalog();
        const auto it = std::find_if(catalog.begin(), catalog.end(),
            [id](const RuleInfo& info)
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
        while (begin > 0 && (source[begin - 1] == ' ' || source[begin - 1] == '\t'))
        {
            --begin;
        }

        std::size_t end = std::min(offset + length, source.size());
        std::size_t probe = end;
        while (probe < source.size() && (source[probe] == ' ' || source[probe] == '\t'))
        {
            ++probe;
        }

        if (probe == source.size())
        {
            end = probe;
        }
        else if (source[probe] == '\n')
        {
            end = probe + 1;
        }
        else if (source[probe] == '\r' && probe + 1 < source.size() && source[probe + 1] == '\n')
        {
            constexpr std::size_t kCrlfLen = 2;
            end = probe + kCrlfLen;
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
                enabled = override -> enabled;
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

    std::vector<Diagnostic> RuleEngine::Analyze(const ParseTree& tree) const
    {
        return AnalyzeImpl(tree.Source(), tree.Tokens(), tree.Directives());
    }

    std::vector<Diagnostic> RuleEngine::AnalyzeImpl(std::string_view source,
        const std::vector<Token>& tokens,
        const std::vector<PreprocessorDirective>& directives) const
    {
        std::vector<Diagnostic> diagnostics;
        LineTable lines;
        lines.Build(source);

        if (m_options.null_macro)
        {
            std::size_t directive_cursor = 0;
            for (const auto& token : tokens)
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
            for (const auto& token : tokens)
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
                const auto& token = tokens[i];
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
                const auto& token = tokens[i];
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
                    const auto& candidate = tokens[j];
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
                    [](const Token& t)
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
            for (const auto& directive : directives)
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
                const auto& token = tokens[i];
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
                    const auto& candidate = tokens[j];
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

                        for (const char c : text)
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

        // The new/delete family shares one scan so a `new` claimed by a
        // modernize rule (which names the better fix) is never also reported
        // by cpp/no-new-delete: one diagnostic per token.
        std::unordered_set<std::size_t> claimed_new;

        const bool make_unique_on =
            RuleEnabled("cpp/modernize-make-unique", m_options.modernize_make_unique);
        const bool make_shared_on =
            RuleEnabled("cpp/modernize-make-shared", m_options.modernize_make_shared);
        if (make_unique_on || make_shared_on)
        {
            struct SmartFactory
            {
                const char* code;
                RuleId id;
                bool enabled;
                const char* smart;
                const char* maker;
            };

            const SmartFactory factories[] = {
                {"cpp/modernize-make-unique", RuleId::ModernizeMakeUnique, make_unique_on,
                    "unique_ptr", "make_unique"},
                {"cpp/modernize-make-shared", RuleId::ModernizeMakeShared, make_shared_on,
                    "shared_ptr", "make_shared"},
            };
            std::size_t directive_cursor = 0;
            for (const auto& factory : factories)
            {
                if (!factory.enabled)
                {
                    continue;
                }

                for (std::size_t i = 0; i < tokens.size(); ++i)
                {
                    if (!IsWord(source, tokens[i], factory.smart) ||
                        IsInDirective(tokens[i].offset, directives, directive_cursor))
                    {
                        continue;
                    }

                    // `std::unique_ptr`: keep the qualification for the fix.
                    bool qualified = false;
                    std::size_t start = i;
                    const std::size_t maybe_scope = PrevSignificant(tokens, i);
                    if (maybe_scope < tokens.size() && IsPunct(source, tokens[maybe_scope], "::"))
                    {
                        const std::size_t maybe_std = PrevSignificant(tokens, maybe_scope);
                        if (maybe_std < tokens.size() && IsWord(source, tokens[maybe_std], "std"))
                        {
                            qualified = true;
                            start = maybe_std;
                        }
                    }

                    const std::size_t open_angle = NextSignificant(tokens, i + 1);
                    if (open_angle >= tokens.size() || !IsPunct(source, tokens[open_angle], "<"))
                    {
                        continue;
                    }

                    // Template arguments: plain nesting only. Anything fancier
                    // (`>>`, parens, braces, `;`) gives up silently and the
                    // `new` falls through to cpp/no-new-delete.
                    std::size_t depth = 0;
                    std::size_t close_angle = tokens.size();
                    for (std::size_t k = open_angle; k < tokens.size(); ++k)
                    {
                        if (tokens[k].kind != TokenKind::Punctuation)
                        {
                            continue;
                        }

                        const auto text = source.substr(tokens[k].offset, tokens[k].length);
                        if (text == "<")
                        {
                            ++depth;
                        }
                        else if (text == ">")
                        {
                            if (--depth == 0)
                            {
                                close_angle = k;
                                break;
                            }
                        }
                        else if (text == ";" || text == "{" || text == "}" || text == "(" || text == ")" ||
                            (text.size() > 1 && text.front() == '>'))
                        {
                            break;
                        }
                    }

                    if (close_angle >= tokens.size())
                    {
                        continue;
                    }

                    const auto template_args = Trimmed(source.substr(
                        tokens[open_angle].offset + tokens[open_angle].length,
                        tokens[close_angle].offset -
                            (tokens[open_angle].offset + tokens[open_angle].length)));
                    const std::size_t after = NextSignificant(tokens, close_angle + 1);
                    if (after >= tokens.size())
                    {
                        continue;
                    }

                    // Declaration form: `unique_ptr<T> name(new T(args))`.
                    bool is_declaration = false;
                    std::string_view declared_name;
                    std::size_t open_paren = after;
                    if (tokens[after].kind == TokenKind::Identifier && !IsWord(source, tokens[after], "new"))
                    {
                        const std::size_t maybe_paren = NextSignificant(tokens, after + 1);
                        if (maybe_paren < tokens.size() && IsPunct(source, tokens[maybe_paren], "("))
                        {
                            is_declaration = true;
                            declared_name =
                                source.substr(tokens[after].offset, tokens[after].length);
                            open_paren = maybe_paren;
                        }
                        else
                        {
                            continue;
                        }
                    }
                    else if (!IsPunct(source, tokens[after], "("))
                    {
                        continue;
                    }

                    const std::size_t maybe_new = NextSignificant(tokens, open_paren + 1);
                    if (maybe_new >= tokens.size() || !IsWord(source, tokens[maybe_new], "new") ||
                        IsInDirective(tokens[maybe_new].offset, directives, directive_cursor))
                    {
                        continue;
                    }

                    NewExpression created;
                    if (!ParseNewExpression(tokens, source, maybe_new, created) || created.is_array)
                    {
                        continue;
                    }

                    const std::size_t outer_close = NextSignificant(tokens, created.end_index + 1);
                    if (outer_close >= tokens.size() || !IsPunct(source, tokens[outer_close], ")"))
                    {
                        continue;
                    }

                    claimed_new.insert(maybe_new);
                    const std::size_t fix_begin = tokens[start].offset;
                    const std::size_t fix_end =
                        tokens[outer_close].offset + tokens[outer_close].length;
                    const std::string qualified_maker =
                        (qualified ? "std::" : "") + std::string(factory.maker);
                    const bool same_type = created.type == template_args;
                    if (same_type)
                    {
                        std::string replacement;
                        if (is_declaration)
                        {
                            replacement = "auto ";
                            replacement += declared_name;
                            replacement += " = ";
                        }

                        replacement += qualified_maker;
                        replacement += '<';
                        replacement += template_args;
                        replacement += ">(";
                        replacement += created.args;
                        replacement += ')';
                        Diagnostic diagnostic = MakeDiagnostic(factory.id, factory.code,
                            std::string("use ") + qualified_maker + " instead of " +
                                factory.smart + "(new ...)",
                            fix_begin, fix_end - fix_begin, lines.Lookup(fix_begin),
                            {fix_begin, fix_end - fix_begin, std::move(replacement)});
                        // make_unique changes overload/exception behavior for
                        // exotic types: offered as a quick fix, not in batch.
                        diagnostic.fix_is_safe = false;
                        diagnostic.fix_title = std::string("Replace with ") + qualified_maker + "<" +
                            std::string(template_args) + ">(...)";
                        diagnostics.push_back(std::move(diagnostic));
                    }
                    else
                    {
                        // `unique_ptr<Base>(new Derived)`: make_unique<Base>(args)
                        // would build the wrong object, so only the direction.
                        diagnostics.push_back(MakeDiagnostic(factory.id, factory.code,
                            std::string("use ") + qualified_maker + " instead of " +
                                factory.smart + "(new ...)",
                            fix_begin, fix_end - fix_begin, lines.Lookup(fix_begin),
                            {0, 0, std::string()}, false));
                    }
                }
            }
        }

        if (RuleEnabled("cpp/modernize-smart-ptr", m_options.modernize_smart_ptr))
        {
            std::size_t directive_cursor = 0;
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                if (!IsWord(source, tokens[i], "reset") ||
                    IsInDirective(tokens[i].offset, directives, directive_cursor))
                {
                    continue;
                }

                // `holder.reset(new T(args))`: unique vs shared is unknowable
                // here, so the direction only, without a fix.
                const std::size_t dot = PrevSignificant(tokens, i);
                if (dot >= tokens.size() || !IsPunct(source, tokens[dot], "."))
                {
                    continue;
                }

                const std::size_t holder = PrevSignificant(tokens, dot);
                if (holder >= tokens.size() || tokens[holder].kind != TokenKind::Identifier)
                {
                    continue;
                }

                const std::size_t open = NextSignificant(tokens, i + 1);
                if (open >= tokens.size() || !IsPunct(source, tokens[open], "("))
                {
                    continue;
                }

                const std::size_t maybe_new = NextSignificant(tokens, open + 1);
                if (maybe_new >= tokens.size() || !IsWord(source, tokens[maybe_new], "new"))
                {
                    continue;
                }

                NewExpression created;
                if (!ParseNewExpression(tokens, source, maybe_new, created) || created.is_array)
                {
                    continue;
                }

                const std::size_t close = NextSignificant(tokens, created.end_index + 1);
                if (close >= tokens.size() || !IsPunct(source, tokens[close], ")"))
                {
                    continue;
                }

                claimed_new.insert(maybe_new);
                const std::size_t begin = tokens[holder].offset;
                const std::size_t end = tokens[close].offset + tokens[close].length;
                diagnostics.push_back(MakeDiagnostic(RuleId::ModernizeSmartPtr,
                    "cpp/modernize-smart-ptr",
                    "resetting a smart pointer with new; assign std::make_unique/std::make_shared instead",
                    begin, end - begin, lines.Lookup(begin), {0, 0, std::string()}, false));
            }

            for (std::size_t m = 0; m < tokens.size(); ++m)
            {
                // Ownership taken in a raw pointer: `T *name = new T(args)`.
                if (!IsWord(source, tokens[m], "new") ||
                    IsInDirective(tokens[m].offset, directives, directive_cursor))
                {
                    continue;
                }

                const std::size_t equal = PrevSignificant(tokens, m);
                if (equal >= tokens.size() || !IsPunct(source, tokens[equal], "="))
                {
                    continue;
                }

                const std::size_t name = PrevSignificant(tokens, equal);
                if (name >= tokens.size() || tokens[name].kind != TokenKind::Identifier)
                {
                    continue;
                }

                const std::size_t star = PrevSignificant(tokens, name);
                if (star >= tokens.size() || !IsPunct(source, tokens[star], "*"))
                {
                    continue;
                }

                // The declared type: identifiers, `::`, cv-qualifiers and
                // simple `<...>` only. Anything else (notably a second `*`,
                // `&`, or an expression) is not a plain owning declaration.
                std::size_t run_begin = star;
                {
                    std::size_t cursor = PrevSignificant(tokens, star);
                    std::size_t angle_depth = 0;
                    bool ok = false;
                    while (cursor < tokens.size())
                    {
                        const auto& token = tokens[cursor];
                        if (token.kind == TokenKind::Identifier || IsWord(source, token, "const") ||
                            IsWord(source, token, "volatile"))
                        {
                            ok = true;
                            run_begin = cursor;
                            cursor = PrevSignificant(tokens, cursor);
                            continue;
                        }

                        if (IsPunct(source, token, "::"))
                        {
                            run_begin = cursor;
                            cursor = PrevSignificant(tokens, cursor);
                            continue;
                        }

                        if (IsPunct(source, token, ">"))
                        {
                            ++angle_depth;
                            run_begin = cursor;
                            cursor = PrevSignificant(tokens, cursor);
                            continue;
                        }

                        if (IsPunct(source, token, "<"))
                        {
                            if (angle_depth == 0)
                            {
                                break;
                            }

                            --angle_depth;
                            run_begin = cursor;
                            cursor = PrevSignificant(tokens, cursor);
                            continue;
                        }

                        break;
                    }

                    if (!ok || angle_depth != 0)
                    {
                        continue;
                    }
                }

                // A boundary must precede the type: `;` `{` `}` `(` `,` `:`
                // or a control keyword. Anything else (an identifier, `*`,
                // `&`, `>`, `]`, `)`) continues a larger declarator or an
                // expression, so this is not a declaration.
                const std::size_t before = PrevSignificant(tokens, run_begin);
                bool boundary = before >= tokens.size();
                if (!boundary && before < tokens.size())
                {
                    if (tokens[before].kind == TokenKind::Punctuation)
                    {
                        const auto text = source.substr(tokens[before].offset, tokens[before].length);
                        boundary = text == ";" || text == "{" || text == "}" || text == "(" ||
                            text == "," || text == ":";
                    }
                    else if (tokens[before].kind == TokenKind::Identifier)
                    {
                        const auto text = source.substr(tokens[before].offset, tokens[before].length);
                        boundary = text == "if" || text == "else" || text == "for" || text == "while" ||
                            text == "do" || text == "case" || text == "return";
                    }
                }

                if (!boundary)
                {
                    continue;
                }

                NewExpression created;
                if (!ParseNewExpression(tokens, source, m, created) || created.is_array)
                {
                    continue;
                }

                claimed_new.insert(m);
                const std::size_t fix_begin = tokens[run_begin].offset;
                const std::size_t fix_end =
                    tokens[created.end_index].offset + tokens[created.end_index].length;
                const auto declared_type = Trimmed(source.substr(fix_begin,
                    tokens[star].offset - fix_begin));
                const auto variable =
                    source.substr(tokens[name].offset, tokens[name].length);
                const bool same_type = created.type == declared_type;
                const bool has_constexpr = declared_type.find("constexpr") != std::string_view::npos;
                const bool is_builtin_new =
                    created.type == "int" || created.type == "char" || created.type == "short" ||
                    created.type == "long" || created.type == "float" || created.type == "double" ||
                    created.type == "bool" || created.type == "unsigned" || created.type == "signed";
                // `new T` without parens leaves scalars uninitialized while
                // `make_unique<T>()` value-initializes: only class types get
                // the fix there. `Base* p = new Derived` would build the wrong
                // object; constexpr cannot call make_unique here either.
                const bool fixable =
                    same_type && !has_constexpr && (created.has_parens || !is_builtin_new);
                if (fixable)
                {
                    const bool is_const = declared_type.find("const") != std::string_view::npos;
                    std::string replacement(is_const ? "const auto " : "auto ");
                    replacement += variable;
                    replacement += " = std::make_unique<";
                    replacement += created.type;
                    replacement += ">(";
                    replacement += created.args;
                    replacement += ')';
                    Diagnostic diagnostic = MakeDiagnostic(RuleId::ModernizeSmartPtr,
                        "cpp/modernize-smart-ptr",
                        "ownership held in raw pointer '" + std::string(variable) +
                            "'; use std::unique_ptr",
                        fix_begin, fix_end - fix_begin, lines.Lookup(fix_begin),
                        {fix_begin, fix_end - fix_begin, std::move(replacement)});
                    diagnostic.fix_is_safe = false;
                    diagnostic.fix_title =
                        "Use std::make_unique for '" + std::string(variable) + '\'';
                    diagnostics.push_back(std::move(diagnostic));
                }
                else
                {
                    diagnostics.push_back(MakeDiagnostic(RuleId::ModernizeSmartPtr,
                        "cpp/modernize-smart-ptr",
                        "ownership held in raw pointer '" + std::string(variable) +
                            "'; use a smart pointer",
                        fix_begin, fix_end - fix_begin, lines.Lookup(fix_begin),
                        {0, 0, std::string()}, false));
                }
            }
        }

        if (RuleEnabled("cpp/no-new-delete", m_options.no_new_delete))
        {
            std::size_t directive_cursor = 0;
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                const auto& token = tokens[i];
                const bool is_new = token.tok == Tok::KwNew ||
                    (token.kind == TokenKind::Identifier &&
                    source.substr(token.offset, token.length) == "new");
                const bool is_delete = !is_new &&
                    (token.tok == Tok::KwDelete ||
                    (token.kind == TokenKind::Identifier &&
                    source.substr(token.offset, token.length) == "delete"));
                if ((!is_new && !is_delete) ||
                    IsInDirective(token.offset, directives, directive_cursor))
                {
                    continue;
                }

                // `operator new` / `operator delete` overloads are the
                // customization point, not manual memory management.
                const std::size_t previous = PrevSignificant(tokens, i);
                if (previous < tokens.size() && IsWord(source, tokens[previous], "operator"))
                {
                    continue;
                }

                if (is_new)
                {
                    // Placement-new is an allocator building block, and a
                    // claimed `new` already has a better rule on it.
                    const std::size_t next = NextSignificant(tokens, i + 1);
                    if (next < tokens.size() && IsPunct(source, tokens[next], "("))
                    {
                        continue;
                    }

                    if (claimed_new.contains(i))
                    {
                        continue;
                    }

                    diagnostics.push_back(MakeDiagnostic(RuleId::NoNewDelete, "cpp/no-new-delete",
                        "direct use of 'new'; prefer RAII (std::make_unique, containers, values)",
                        token.offset, token.length, lines.Lookup(token.offset),
                        {0, 0, std::string()}, false));
                }
                else
                {
                    std::size_t end = i;
                    const std::size_t maybe_bracket = NextSignificant(tokens, i + 1);
                    if (maybe_bracket < tokens.size() && IsPunct(source, tokens[maybe_bracket], "["))
                    {
                        const std::size_t close = MatchBracket(tokens, source, maybe_bracket, "[", "]");
                        if (close < tokens.size())
                        {
                            end = close;
                        }
                    }

                    const std::size_t length =
                        tokens[end].offset + tokens[end].length - token.offset;
                    diagnostics.push_back(MakeDiagnostic(RuleId::NoNewDelete, "cpp/no-new-delete",
                        "direct use of 'delete'; ownership belongs in a smart pointer",
                        token.offset, length, lines.Lookup(token.offset),
                        {0, 0, std::string()}, false));
                }
            }
        }

        if (RuleEnabled("cpp/modernize-emplace", m_options.modernize_emplace))
        {
            std::size_t directive_cursor = 0;
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                if (tokens[i].kind != TokenKind::Punctuation ||
                    IsInDirective(tokens[i].offset, directives, directive_cursor))
                {
                    continue;
                }

                const auto dot = source.substr(tokens[i].offset, tokens[i].length);
                if (dot != "." && dot != "->")
                {
                    continue;
                }

                const std::size_t method = NextSignificant(tokens, i + 1);
                if (method >= tokens.size() || tokens[method].kind != TokenKind::Identifier)
                {
                    continue;
                }

                const auto method_name = source.substr(tokens[method].offset, tokens[method].length);
                std::string_view emplacer;
                if (method_name == "push_back")
                {
                    emplacer = "emplace_back";
                }
                else if (method_name == "push_front")
                {
                    emplacer = "emplace_front";
                }
                else
                {
                    continue;
                }

                const std::size_t open = NextSignificant(tokens, method + 1);
                if (open >= tokens.size() || !IsPunct(source, tokens[open], "("))
                {
                    continue;
                }

                const std::size_t first = NextSignificant(tokens, open + 1);
                if (first >= tokens.size())
                {
                    continue;
                }

                std::string_view inner;
                std::size_t fix_end = tokens.size();
                if (IsPunct(source, tokens[first], "{"))
                {
                    // `v.push_back({a, b})` -> `v.emplace_back(a, b)`.
                    const std::size_t close_brace = MatchBracket(tokens, source, first, "{", "}");
                    if (close_brace >= tokens.size())
                    {
                        continue;
                    }

                    const std::size_t close_paren = NextSignificant(tokens, close_brace + 1);
                    if (close_paren >= tokens.size() || !IsPunct(source, tokens[close_paren], ")"))
                    {
                        continue;
                    }

                    const std::size_t inner_begin = tokens[first].offset + tokens[first].length;
                    inner = Trimmed(source.substr(inner_begin, tokens[close_brace].offset - inner_begin));
                    fix_end = tokens[close_paren].offset + tokens[close_paren].length;
                }
                else
                {
                    // `v.push_back(T(a, b))` -> `v.emplace_back(a, b)`: a type
                    // run (identifiers, `::`, balanced `<...>`) then `(args)`.
                    std::size_t cursor = first;
                    std::size_t angle_depth = 0;
                    bool saw_ident = false;
                    bool shape_ok = true;
                    while (cursor < tokens.size())
                    {
                        const auto& token = tokens[cursor];
                        if (token.kind == TokenKind::Identifier)
                        {
                            saw_ident = true;
                            cursor = NextSignificant(tokens, cursor + 1);
                            continue;
                        }

                        if (token.kind != TokenKind::Punctuation)
                        {
                            shape_ok = false;
                            break;
                        }

                        const auto text = source.substr(token.offset, token.length);
                        if (text == "::")
                        {
                            cursor = NextSignificant(tokens, cursor + 1);
                            continue;
                        }

                        if (text == "<")
                        {
                            ++angle_depth;
                            cursor = NextSignificant(tokens, cursor + 1);
                            continue;
                        }

                        if (text == ">")
                        {
                            if (angle_depth == 0)
                            {
                                shape_ok = false;
                                break;
                            }

                            --angle_depth;
                            cursor = NextSignificant(tokens, cursor + 1);
                            continue;
                        }

                        break;
                    }

                    if (!shape_ok || !saw_ident || angle_depth != 0 || cursor >= tokens.size() ||
                        !IsPunct(source, tokens[cursor], "("))
                    {
                        continue;
                    }

                    // A lone `push_back(x)` is not a construction.
                    if (cursor == first)
                    {
                        continue;
                    }

                    // `push_back(new X)` stays with the memory rules.
                    if (IsWord(source, tokens[first], "new"))
                    {
                        continue;
                    }

                    const std::size_t close_ctor = MatchBracket(tokens, source, cursor, "(", ")");
                    if (close_ctor >= tokens.size())
                    {
                        continue;
                    }

                    const std::size_t close_paren = NextSignificant(tokens, close_ctor + 1);
                    if (close_paren >= tokens.size() || !IsPunct(source, tokens[close_paren], ")"))
                    {
                        continue;
                    }

                    const std::size_t inner_begin =
                        tokens[cursor].offset + tokens[cursor].length;
                    inner = Trimmed(source.substr(inner_begin, tokens[close_ctor].offset - inner_begin));
                    fix_end = tokens[close_paren].offset + tokens[close_paren].length;
                }

                const std::size_t fix_begin = tokens[method].offset;
                std::string replacement(emplacer);
                replacement += '(';
                replacement += inner;
                replacement += ')';
                Diagnostic diagnostic = MakeDiagnostic(RuleId::ModernizeEmplace,
                    "cpp/modernize-emplace",
                    std::string("use ") + std::string(emplacer) + " instead of " +
                        std::string(method_name) + " with a temporary",
                    fix_begin, fix_end - fix_begin, lines.Lookup(fix_begin),
                    {fix_begin, fix_end - fix_begin, std::move(replacement)});
                // Explicit constructors, narrowing and initializer_list
                // overloads differ: quick fix only.
                diagnostic.fix_is_safe = false;
                diagnostic.fix_title = std::string("Replace with ") + std::string(emplacer);
                diagnostics.push_back(std::move(diagnostic));
            }
        }

        if (RuleEnabled("cpp/modernize-structured-bindings", m_options.modernize_structured_bindings))
        {
            std::size_t directive_cursor = 0;
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                // `std::tie(a, b) = expr;` -> `auto [a, b] = expr;`.
                if (!IsWord(source, tokens[i], "tie") ||
                    IsInDirective(tokens[i].offset, directives, directive_cursor))
                {
                    continue;
                }

                std::size_t start = i;
                const std::size_t maybe_scope = PrevSignificant(tokens, i);
                if (maybe_scope < tokens.size() && IsPunct(source, tokens[maybe_scope], "::"))
                {
                    const std::size_t maybe_std = PrevSignificant(tokens, maybe_scope);
                    if (maybe_std < tokens.size() && IsWord(source, tokens[maybe_std], "std"))
                    {
                        start = maybe_std;
                    }
                }

                const std::size_t open = NextSignificant(tokens, i + 1);
                if (open >= tokens.size() || !IsPunct(source, tokens[open], "("))
                {
                    continue;
                }

                const std::size_t close = MatchBracket(tokens, source, open, "(", ")");
                if (close >= tokens.size())
                {
                    continue;
                }

                // Bindings only: plain identifiers, at least two.
                std::vector<std::string_view> bound;
                bool shape_ok = true;
                for (std::size_t k = NextSignificant(tokens, open + 1); k < close;)
                {
                    if (tokens[k].kind != TokenKind::Identifier)
                    {
                        shape_ok = false;
                        break;
                    }

                    bound.push_back(source.substr(tokens[k].offset, tokens[k].length));
                    const std::size_t after = NextSignificant(tokens, k + 1);
                    if (after == close)
                    {
                        break;
                    }

                    if (!IsPunct(source, tokens[after], ","))
                    {
                        shape_ok = false;
                        break;
                    }

                    k = NextSignificant(tokens, after + 1);
                }

                if (!shape_ok || bound.size() < 2)
                {
                    continue;
                }

                const std::size_t equal = NextSignificant(tokens, close + 1);
                if (equal >= tokens.size() || !IsPunct(source, tokens[equal], "="))
                {
                    continue;
                }

                // The assigned expression runs to `;` without braces.
                const std::size_t value_begin = NextSignificant(tokens, equal + 1);
                std::size_t semi = value_begin;
                bool expr_ok = value_begin < tokens.size();
                while (expr_ok && semi < tokens.size())
                {
                    if (tokens[semi].kind != TokenKind::Punctuation)
                    {
                        semi = NextSignificant(tokens, semi + 1);
                        continue;
                    }

                    const auto text = source.substr(tokens[semi].offset, tokens[semi].length);
                    if (text == ";")
                    {
                        break;
                    }

                    if (text == "{" || text == "}")
                    {
                        expr_ok = false;
                        break;
                    }

                    semi = NextSignificant(tokens, semi + 1);
                }

                if (!expr_ok || semi >= tokens.size())
                {
                    continue;
                }

                const std::size_t fix_begin = tokens[start].offset;
                const std::size_t fix_end = tokens[semi].offset + tokens[semi].length;
                std::string replacement = "auto [";
                for (std::size_t b = 0; b < bound.size(); ++b)
                {
                    replacement += bound[b];
                    if (b + 1 < bound.size())
                    {
                        replacement += ", ";
                    }
                }

                replacement += "] = ";
                replacement += Trimmed(source.substr(tokens[value_begin].offset,
                    tokens[semi].offset - tokens[value_begin].offset));
                replacement += ';';
                Diagnostic diagnostic = MakeDiagnostic(RuleId::ModernizeStructuredBindings,
                    "cpp/modernize-structured-bindings",
                    "unpack with a structured binding instead of std::tie", fix_begin,
                    fix_end - fix_begin, lines.Lookup(fix_begin),
                    {fix_begin, fix_end - fix_begin, std::move(replacement)});
                // `tie` assigns to existing variables while `auto [...]`
                // declares copies: the user confirms the change.
                diagnostic.fix_is_safe = false;
                diagnostic.fix_title = "Replace std::tie with a structured binding";
                diagnostics.push_back(std::move(diagnostic));
            }

            // Repeated `.first` / `.second` on the same object: suggest one
            // decomposition. Reported once per object, on the second access.
            {
                std::unordered_map<std::string_view, bool> saw_first;
                std::unordered_map<std::string_view, bool> saw_second;
                std::unordered_set<std::string_view> reported;
                std::size_t cursor = 0;
                for (std::size_t i = 0; i < tokens.size(); ++i)
                {
                    if (tokens[i].kind != TokenKind::Punctuation ||
                        IsInDirective(tokens[i].offset, directives, cursor))
                    {
                        continue;
                    }

                    const auto access = source.substr(tokens[i].offset, tokens[i].length);
                    if (access != "." && access != "->")
                    {
                        continue;
                    }

                    const std::size_t member = NextSignificant(tokens, i + 1);
                    if (member >= tokens.size() || tokens[member].kind != TokenKind::Identifier)
                    {
                        continue;
                    }

                    const auto member_name =
                        source.substr(tokens[member].offset, tokens[member].length);
                    const bool is_first = member_name == "first";
                    if (!is_first && member_name != "second")
                    {
                        continue;
                    }

                    const std::size_t object = PrevSignificant(tokens, i);
                    if (object >= tokens.size() || tokens[object].kind != TokenKind::Identifier)
                    {
                        continue;
                    }

                    const auto name = source.substr(tokens[object].offset, tokens[object].length);
                    if (is_first)
                    {
                        saw_first[name] = true;
                    }
                    else
                    {
                        saw_second[name] = true;
                    }

                    const bool both = saw_first.contains(name) && saw_second.contains(name);
                    if (!both || reported.contains(name))
                    {
                        continue;
                    }

                    reported.insert(name);
                    const std::size_t begin = tokens[object].offset;
                    const std::size_t end = tokens[member].offset + tokens[member].length;
                    diagnostics.push_back(MakeDiagnostic(RuleId::ModernizeStructuredBindings,
                        "cpp/modernize-structured-bindings",
                        "member access to '" + std::string(name) +
                            ".first/.second; consider a structured binding",
                        begin, end - begin, lines.Lookup(begin), {0, 0, std::string()}, false));
                }
            }
        }

        if (RuleEnabled("cpp/modernize-algorithms", m_options.modernize_algorithms))
        {
            std::size_t directive_cursor = 0;
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                if (!IsWord(source, tokens[i], "for") ||
                    IsInDirective(tokens[i].offset, directives, directive_cursor))
                {
                    continue;
                }

                const std::size_t open = NextSignificant(tokens, i + 1);
                if (open >= tokens.size() || !IsPunct(source, tokens[open], "("))
                {
                    continue;
                }

                const std::size_t close = MatchBracket(tokens, source, open, "(", ")");
                if (close >= tokens.size())
                {
                    continue;
                }

                // Range-based `for` has a top-level `:` and no `;`.
                bool has_colon = false;
                bool has_semi = false;
                {
                    std::size_t depth = 0;
                    for (std::size_t k = open; k <= close; ++k)
                    {
                        if (tokens[k].kind != TokenKind::Punctuation)
                        {
                            continue;
                        }

                        const auto text = source.substr(tokens[k].offset, tokens[k].length);
                        if (text == "(" || text == "[" || text == "{")
                        {
                            ++depth;
                        }
                        else if (text == ")" || text == "]" || text == "}")
                        {
                            --depth;
                        }
                        else if (depth == 1 && text == ":")
                        {
                            has_colon = true;
                        }
                        else if (depth == 1 && text == ";")
                        {
                            has_semi = true;
                        }
                    }
                }

                const bool is_range_for = has_colon && !has_semi;
                // Loop variable: last identifier before the `:` of a range-for.
                std::string_view loop_variable;
                if (is_range_for)
                {
                    for (std::size_t k = open; k <= close; ++k)
                    {
                        if (tokens[k].kind == TokenKind::Identifier)
                        {
                            loop_variable =
                                source.substr(tokens[k].offset, tokens[k].length);
                        }
                        else if (tokens[k].kind == TokenKind::Punctuation &&
                            source.substr(tokens[k].offset, tokens[k].length) == ":")
                        {
                            break;
                        }
                    }
                }

                const std::size_t body_begin = NextSignificant(tokens, close + 1);
                if (body_begin >= tokens.size())
                {
                    continue;
                }

                std::size_t body_end = tokens.size();
                if (IsPunct(source, tokens[body_begin], "{"))
                {
                    body_end = MatchBracket(tokens, source, body_begin, "{", "}");
                    if (body_end >= tokens.size())
                    {
                        continue;
                    }
                }
                else
                {
                    // Single-statement body: runs to `;` at nesting depth 0.
                    std::size_t paren = 0;
                    std::size_t bracket = 0;
                    std::size_t brace = 0;
                    for (std::size_t k = body_begin; k < tokens.size(); ++k)
                    {
                        if (tokens[k].kind != TokenKind::Punctuation)
                        {
                            continue;
                        }

                        const auto text = source.substr(tokens[k].offset, tokens[k].length);
                        if (text == "(")
                        {
                            ++paren;
                        }
                        else if (text == ")")
                        {
                            if (paren == 0)
                            {
                                break;
                            }

                            --paren;
                        }
                        else if (text == "[")
                        {
                            ++bracket;
                        }
                        else if (text == "]" && bracket > 0)
                        {
                            --bracket;
                        }
                        else if (text == "{")
                        {
                            ++brace;
                        }
                        else if (text == "}" && brace > 0)
                        {
                            --brace;
                        }
                        else if (text == ";" && paren == 0 && bracket == 0 && brace == 0)
                        {
                            body_end = k;
                            break;
                        }
                    }

                    if (body_end >= tokens.size())
                    {
                        continue;
                    }
                }

                bool has_plus_equal = false;
                bool plus_equal_on_loop_variable = false;
                bool has_if = false;
                bool has_increment = false;
                bool has_push_back = false;
                bool has_escape = false;
                for (std::size_t k = body_begin; k <= body_end && k < tokens.size(); ++k)
                {
                    if (tokens[k].kind == TokenKind::Identifier)
                    {
                        const auto text = source.substr(tokens[k].offset, tokens[k].length);
                        has_if = has_if || text == "if";
                        has_push_back = has_push_back || text == "push_back";
                        has_escape = has_escape || text == "break" || text == "return" ||
                            text == "goto" || text == "throw";
                    }
                    else if (tokens[k].kind == TokenKind::Punctuation)
                    {
                        const auto text = source.substr(tokens[k].offset, tokens[k].length);
                        if (text == "++" || text == "--")
                        {
                            has_increment = true;
                        }
                        else if (text == "+=")
                        {
                            has_plus_equal = true;
                            const std::size_t target = PrevSignificant(tokens, k);
                            if (target < tokens.size() && !loop_variable.empty() &&
                                IsWord(source, tokens[target], loop_variable))
                            {
                                plus_equal_on_loop_variable = true;
                            }
                        }
                    }
                }

                if (has_escape)
                {
                    continue;
                }

                const std::size_t loop_begin = tokens[i].offset;
                const std::size_t loop_end =
                    tokens[body_end].offset + tokens[body_end].length;
                const std::size_t loop_length = loop_end - loop_begin;
                if (is_range_for && has_plus_equal && !plus_equal_on_loop_variable)
                {
                    diagnostics.push_back(MakeDiagnostic(RuleId::ModernizeAlgorithms,
                        "cpp/modernize-algorithms",
                        "loop accumulates a value; consider std::accumulate", loop_begin,
                        loop_length, lines.Lookup(loop_begin), {0, 0, std::string()}, false));
                }

                if (has_if && has_push_back)
                {
                    diagnostics.push_back(MakeDiagnostic(RuleId::ModernizeAlgorithms,
                        "cpp/modernize-algorithms",
                        "conditional push_back in a loop; consider std::copy_if", loop_begin,
                        loop_length, lines.Lookup(loop_begin), {0, 0, std::string()}, false));
                }
                else if (has_if && has_increment)
                {
                    diagnostics.push_back(MakeDiagnostic(RuleId::ModernizeAlgorithms,
                        "cpp/modernize-algorithms",
                        "conditional increment in a loop; consider std::count_if", loop_begin,
                        loop_length, lines.Lookup(loop_begin), {0, 0, std::string()}, false));
                }
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

            auto group_index =[this](const IncludeTarget& target)
            {
                const auto& order = m_options.include_order;
                const auto wanted = target.angle ? IncludeGroup::Angle
                : IncludeGroup::Quote;
                const auto it = std::find(order.begin(), order.end(), wanted);
                return it == order.end() ? order.size()
                : static_cast<std::size_t>(it - order.begin());
            };

            auto sorts_before =[&](const IncludeTarget& left,
                const IncludeTarget& right)
            {
                const auto left_group = group_index(left);
                const auto right_group = group_index(right);
                if (left_group != right_group)
                {
                    return left_group < right_group;
                }

                const auto left_name = left.text.substr(1, left.text.size() - kIncludeDelimCount);
                const auto right_name = right.text.substr(1, right.text.size() - kIncludeDelimCount);
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
                    const auto& previous = directives[includes[block_end - 1].directive];
                    const auto& current = directives[includes[block_end].directive];
                    if (current.offset != previous.offset + previous.length)
                    {
                        break;
                    }

                    ++block_end;
                }

                if (block_end - block_begin >= kMinSortableBlock)
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
                        const auto& first = directives[includes[block_begin].directive];
                        const auto& last = directives[includes[block_end - 1].directive];
                        const auto offset = first.offset;
                        const auto length = last.offset + last.length - offset;
                        std::string replacement;
                        for (const auto index : order)
                        {
                            const auto& directive = directives[includes[block_begin + index].directive];
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
        std::string_view source, const std::vector<Token>& tokens) const
    {
        LineTable lines;
        lines.Build(source);

        std::sort(diagnostics.begin(), diagnostics.end(),[](const Diagnostic& a, const Diagnostic& b)
            {
                return a.offset < b.offset;
        });

        for (auto& diagnostic : diagnostics)
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

        std::erase_if(diagnostics,[this](const Diagnostic& diagnostic)
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
            std::erase_if(diagnostics,[&suppressions](const Diagnostic& diagnostic)
                {
                    return std::any_of(suppressions.begin(), suppressions.end(),[&diagnostic](const Suppression& s)
                    {
                        return s.line == diagnostic.line &&
                        (s.codes.empty() || Suppresses(s.codes, diagnostic.code));
                });
            });
        }

        return diagnostics;
    }

    std::string RuleEngine::ApplyFixes(std::string_view source,
        const std::vector<Diagnostic>& diagnostics, bool include_unsafe)
    {
        std::vector<const TextEdit*> edits;
        edits.reserve(diagnostics.size());
        for (const auto& diagnostic : diagnostics)
        {
            if (diagnostic.has_fix && (diagnostic.fix_is_safe || include_unsafe))
            {
                edits.push_back(&diagnostic.fix);
            }
        }

        std::sort(edits.begin(), edits.end(),[](const TextEdit* a, const TextEdit* b)
            {
                return a->offset > b->offset;
        });

        std::string result(source);
        std::size_t previous_start = source.size();
        for (const TextEdit* edit : edits)
        {
            const auto owner = std::find_if(diagnostics.begin(), diagnostics.end(),[edit](const Diagnostic& d)
                {
                    return d.has_fix && &d.fix == edit;
            });
            // Safe fixes must stay within their diagnostic. Unsafe ones, applied
            // only on request, may edit elsewhere (a specifier before the type, a
            // keyword after the parameters) but are still bounds-checked.
            const bool in_range = owner != diagnostics.end() && edit->offset >= owner->offset &&
                edit->offset - owner->offset <= owner->length &&
                edit->length <= owner->length -(edit->offset - owner->offset);
            if (owner == diagnostics.end() ||(!in_range && owner->fix_is_safe) ||
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
