#pragma once

#include <Heimdall/LineTable.hpp>
#include <Heimdall/ParseTree.hpp>
#include <Heimdall/RuleEngine.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace heimdall::detail
{

    inline bool IsTrivia(const Token& token)
    {
        return token.kind == TokenKind::Whitespace || token.kind == TokenKind::LineComment ||
               token.kind == TokenKind::BlockComment;
    }

    inline std::string Lowercase(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    inline std::string Key(const std::filesystem::path& path)
    {
        return path.lexically_normal().generic_string();
    }

    inline void SortByOffset(std::vector<Diagnostic>& diagnostics)
    {
        std::stable_sort(
            diagnostics.begin(), diagnostics.end(),
            [](const Diagnostic& a, const Diagnostic& b) { return a.offset < b.offset; });
    }

    // Builds the diagnostics of the project-level rules; the line table is
    // built on first use.
    class Reporter
    {
      public:
        explicit Reporter(const ParseTree& tree) : m_tree(tree) {}

        Diagnostic Make(RuleId           rule,
                        std::string_view code,
                        std::string      message,
                        std::size_t      offset,
                        std::size_t      length,
                        TextEdit         fix,
                        std::string      title)
        {
            if (!m_lines_built)
            {
                m_lines.Build(m_tree.Source());
                m_lines_built = true;
            }

            const auto position = m_lines.Lookup(offset);
            Diagnostic diagnostic {
                rule,
                Severity::Warning,
                std::string(code),
                std::move(message),
                offset,
                length,
                position.line,
                position.column,
                true,
                std::move(fix)
            };
            // Both rules rest on what the model could see of the project: quick
            // fixes in the editor, never applied in batch.
            diagnostic.fix_is_safe = false;
            diagnostic.fix_title   = std::move(title);
            return diagnostic;
        }

      private:
        const ParseTree& m_tree;
        LineTable        m_lines;
        bool             m_lines_built = false;
    };

} // namespace heimdall::detail
