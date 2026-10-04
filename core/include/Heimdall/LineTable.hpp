#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace heimdall
{

    // Maps byte offsets to 1-based (line, column) positions.
    // Built once per file; Lookup() is a binary search.
    class LineTable
    {
    public:
        struct Position
        {
            std::uint32_t line = 1;
            std::uint32_t column = 1;
        };

        void Build(std::string_view text);
        Position Lookup(std::size_t offset) const;
        std::size_t LineCount() const noexcept
        {
            return m_line_starts.size();
        }

    private:
        std::vector<std::size_t> m_line_starts;
    };

} // namespace heimdall
