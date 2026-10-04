#pragma once

#include <filesystem>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace heimdall::lsp
{

    struct Position
    {
        std::size_t line = 0;
        std::size_t character = 0;
    };

    struct Document
    {
        std::string text;
        std::int64_t version = 0;
    };

    // Per-snapshot line index: byte offset <-> LSP (line, UTF-16 character).
    // Built once per document version; lookups are O(log L + line width) instead
    // of the previous O(file size) scan per diagnostic/position.
    class LineIndex
    {
    public:
        void Build(std::string_view text);
        Position ToPosition(std::size_t offset) const;
        std::size_t OffsetFromPosition(Position position) const;
        std::size_t LineCount() const noexcept
        {
            return m_line_starts.size();
        }

    private:
        static std::size_t Utf16Width(std::string_view text, std::size_t i, std::size_t stop) noexcept;
        std::string_view m_text;
        std::vector<std::uint32_t> m_line_starts;
    };

    Position ToPosition(std::string_view text, std::size_t offset);
    std::size_t OffsetFromPosition(std::string_view text, Position position);
    std::filesystem::path PathFromUri(std::string_view uri);
    std::string UriFromPath(const std::filesystem::path & path);
    void AppendPosition(Position position, std::string & out);

} // namespace heimdall::lsp
