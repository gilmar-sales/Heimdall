#include <Heimdall/LineTable.hpp>

#include <algorithm>

namespace heimdall
{

    void LineTable::Build(std::string_view text)
    {
        m_line_starts.clear();
        m_line_starts.push_back(0);
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '\n' && i + 1 < text.size())
            {
                m_line_starts.push_back(i + 1);
            }
        }
    }

    LineTable::Position LineTable::Lookup(std::size_t offset) const
    {
        if (m_line_starts.empty())
        {
            return {};
        }

        auto it = std::upper_bound(m_line_starts.begin(), m_line_starts.end(), offset);
        if (it == m_line_starts.begin())
        {
            return { 1, static_cast<std::uint32_t>(offset + 1) };
        }

        --it;
        const std::size_t line = static_cast<std::size_t>(it - m_line_starts.begin());
        return { static_cast<std::uint32_t>(line + 1),
                 static_cast<std::uint32_t>(offset - *it + 1) };
    }

} // namespace heimdall
