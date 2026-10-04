#include <Heimdall/LineTable.hpp>
#include <Heimdall/Lexer.hpp>
#include <Heimdall/Preprocessor.hpp>
#include <Heimdall/RuleEngine.hpp>

#include <algorithm>
#include <string_view>
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
            std::size_t length, LineTable::Position position, TextEdit fix)
        {
            return {rule, Severity::Warning, std::move(code), std::move(message), offset, length,
                position.line, position.column, true, std::move(fix)};
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

        std::vector<Suppression> FindSuppressions(std::string_view source, const std::vector<Token> &tokens,
            const LineTable &lines)
        {
            constexpr std::string_view line_marker = "heimdall-disable-line";
            constexpr std::string_view next_marker = "heimdall-disable-next-line";
            std::vector<Suppression> result;
            for (const auto &token: tokens)
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
                while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == ':' ))
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

    } // namespace

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
                RuleId::MissingFinalNewline, "format/require-final-newline", "file must end with a newline", source.size(), 0,
                position, {source.size(), 0, crlf ? "\r\n" : "\n"}));
        }

        std::sort(diagnostics.begin(), diagnostics.end(),[](const Diagnostic &a, const Diagnostic &b)
            {
                return a.offset < b.offset;
        });

        for (auto &diagnostic: diagnostics)
        {
            for (auto override = m_options.overrides.rbegin(); override != m_options.overrides.rend(); ++override)
            {
                if (override->code == diagnostic.code)
                {
                    diagnostic.severity = override->severity;
                    break;
                }
            }
        }
        std::erase_if(diagnostics, [this](const Diagnostic &diagnostic)
            {
                for (auto override = m_options.overrides.rbegin(); override != m_options.overrides.rend(); ++override)
                {
                    if (override->code == diagnostic.code)
                    {
                        return !override->enabled;
                    }
                }
                return false;
            });
        if (m_options.honor_suppressions && !diagnostics.empty())
        {
            const auto suppressions = FindSuppressions(source, tokens, lines);
            std::erase_if(diagnostics, [&suppressions](const Diagnostic &diagnostic)
                {
                    return std::any_of(suppressions.begin(), suppressions.end(), [&diagnostic](const Suppression &s)
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
                return a->offset > b -> offset;
        });

        std::string result(source);
        std::size_t previous_start = source.size();
        for (const TextEdit * edit: edits)
        {
            const auto owner = std::find_if(diagnostics.begin(), diagnostics.end(), [edit](const Diagnostic &d)
                {
                    return d.has_fix && &d.fix == edit;
                });
            if (owner == diagnostics.end() || edit->offset < owner->offset ||
                edit->offset - owner->offset > owner->length ||
                edit->length > owner->length - (edit->offset - owner->offset) ||
                edit->offset > source.size() || edit->length > source.size() - edit->offset ||
                edit->offset + edit->length > previous_start)
            {
                continue;
            }

            result.replace(edit -> offset, edit -> length, edit->replacement);
            previous_start = edit -> offset;
        }

        return result;
    }

} // namespace heimdall
