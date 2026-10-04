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
            if (edit->offset > source.size() || edit->length > source.size() - edit->offset ||
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
