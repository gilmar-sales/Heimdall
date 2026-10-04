#include <Heimdall/LineTable.hpp>
#include <Heimdall/Lexer.hpp>
#include <Heimdall/Preprocessor.hpp>
#include <Heimdall/RuleEngine.hpp>

#include <algorithm>
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

        std::string_view PunctuationText(std::string_view source, const Token &token)
        {
            return source.substr(token.offset, token.length);
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
                diagnostics.push_back(MakeDiagnostic(
                    RuleId::EmptyCatch, "cpp/no-empty-catch",
                    "empty catch block ignores the exception", offset, length,
                    lines.Lookup(offset), {}, false));
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
                std::string_view body = source.substr(directive.offset, directive.length);
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
                    continue;
                }

                body.remove_prefix(name_length);

                const auto open = body.find_first_of("<\"");
                if (open == std::string_view::npos)
                {
                    continue;
                }

                const auto close_char = body[open] == '<' ? '>' : '"';
                const auto close = body.find(close_char, open + 1);
                if (close == std::string_view::npos)
                {
                    continue;
                }

                const auto target = body.substr(open, close - open + 1);
                const auto[it, inserted] = seen.try_emplace(target, directive.offset);
                if (!inserted)
                {
                    const auto offset = directive.offset + open;
                    diagnostics.push_back(MakeDiagnostic(
                        RuleId::DuplicateInclude, "cpp/no-duplicate-include",
                        "duplicate include of " + std::string(target), offset, target.length(),
                        lines.Lookup(offset), {}, false));
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
            if (diagnostic.has_fix)
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
