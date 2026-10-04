#include "Document.hpp"

#include <algorithm>

namespace heimdall::lsp
{

    void LineIndex::Build(std::string_view text)
    {
        m_text = text;
        m_line_starts.clear();
        m_line_starts.push_back(0);
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '\n')
            {
                m_line_starts.push_back(static_cast<std::uint32_t>(i + 1));
            }
        }
    }

    void LineIndex::Update(std::string_view text, std::size_t offset, std::size_t old_length,
        std::size_t new_length)
    {
        if (m_line_starts.empty())
        {
            Build(text);
            return;
        }

        m_text = text;
        const std::size_t old_end = offset + old_length;
        // Line starts s with offset < s <= old_end sat right after a removed newline.
        const auto first = std::upper_bound(m_line_starts.begin(), m_line_starts.end(),
            static_cast<std::uint32_t>(offset));
        const auto last = std::upper_bound(first, m_line_starts.end(), static_cast<std::uint32_t>(old_end));
        const auto position = m_line_starts.erase(first, last) - m_line_starts.begin();

        std::vector<std::uint32_t> fresh;
        for (std::size_t i = offset; i < offset + new_length && i < text.size(); ++i)
        {
            if (text[i] == '\n')
            {
                fresh.push_back(static_cast<std::uint32_t>(i + 1));
            }
        }

        const std::ptrdiff_t delta =
            static_cast<std::ptrdiff_t>(new_length) - static_cast<std::ptrdiff_t>(old_length);
        auto tail = m_line_starts.begin() + position;
        for (; tail != m_line_starts.end(); ++tail)
        {
            *tail = static_cast<std::uint32_t>(static_cast<std::ptrdiff_t>(*tail) + delta);
        }

        m_line_starts.insert(m_line_starts.begin() + position, fresh.begin(), fresh.end());
    }

    std::size_t LineIndex::Utf16Width(std::string_view text, std::size_t i, std::size_t stop) noexcept
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if ((c & 0x80) == 0)
        {
            return 1;
        }

        std::size_t width = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        if (i + width > stop)
        {
            width = stop - i;
        }

        std::uint32_t codepoint = c & (width == 2 ? 0x1f : width == 3 ? 0x0f : 0x07);
        for (std::size_t j = 1; j < width; ++j)
        {
            codepoint = (codepoint << 6) |(static_cast<unsigned char>(text[i + j]) & 0x3f);
        }

        if (codepoint > 0xffff)
        {
            return 2;
        }

        return 1;
    }

    Position LineIndex::ToPosition(std::size_t offset) const
    {
        Position position;
        if (m_line_starts.empty())
        {
            return position;
        }

        const std::size_t stop = offset < m_text.size() ? offset : m_text.size();
        // Binary search for the line start, then walk only that line's width.
        std::size_t line = 0;
        {
            std::size_t lo = 0;
            std::size_t hi = m_line_starts.size();
            while (lo + 1 < hi)
            {
                const std::size_t mid = (lo + hi) / 2;
                if (static_cast<std::size_t>(m_line_starts[mid]) <= stop)
                {
                    lo = mid;
                }
                else
                {
                    hi = mid;
                }
            }

            line = lo;
        }
        position.line = line;
        std::size_t character = 0;
        for (std::size_t i = m_line_starts[line]; i < stop;)
        {
            const unsigned char c = static_cast<unsigned char>(m_text[i]);
            if ((c & 0x80) == 0)
            {
                ++character;
                ++i;
            }
            else
            {
                character += Utf16Width(m_text, i, stop);
                std::size_t width = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
                if (i + width > stop)
                {
                    width = stop - i;
                }

                i += width;
            }
        }

        position.character = character;
        return position;
    }

    std::size_t LineIndex::OffsetFromPosition(Position position) const
    {
        if (m_line_starts.empty())
        {
            return 0;
        }

        // Past the last line clamps to EOF (mirrors the old linear scan).
        if (position.line >= m_line_starts.size())
        {
            return m_text.size();
        }

        const std::size_t line = position.line;
        std::size_t i = m_line_starts[line];
        const std::size_t line_end =
            line + 1 < m_line_starts.size() ? m_line_starts[line + 1] - 1 : m_text.size();
        std::size_t character = 0;
        while (i < line_end && character < position.character)
        {
            const unsigned char c = static_cast<unsigned char>(m_text[i]);
            if (c == '\n')
            {
                break;
            } // past end-of-line clamps to the newline itself

            if ((c & 0x80) == 0)
            {
                ++character;
                ++i;
            }
            else
            {
                std::size_t width = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
                if (i + width > line_end)
                {
                    width = line_end - i;
                }

                character += Utf16Width(m_text, i, i + width);
                i += width;
            }
        }

        return i;
    }

    Position ToPosition(std::string_view text, std::size_t offset)
    {
        LineIndex index;
        index.Build(text);
        return index.ToPosition(offset);
    }

    std::filesystem::path PathFromUri(std::string_view uri)
    {
        constexpr std::string_view prefix = "file://";
        if (uri.starts_with(prefix))
        {
            uri.remove_prefix(prefix.size());
        }

        std::string decoded;
        decoded.reserve(uri.size());
        auto hex =[](char c) -> int
        {
            if (c >= '0' && c <= '9')
            {
                return c - '0';
            }

            if (c >= 'a' && c <= 'f')
            {
                return c - 'a' + 10;
            }

            if (c >= 'A' && c <= 'F')
            {
                return c - 'A' + 10;
            }

            return -1;
        };
        for (std::size_t i = 0; i < uri.size(); ++i)
        {
            if (uri[i] == '%' && i + 2 < uri.size() && hex(uri[i + 1]) >= 0 && hex(uri[i + 2]) >= 0)
            {
                decoded += static_cast<char>(hex(uri[i + 1]) * 16 + hex(uri[i + 2]));
                i += 2;
            }
            else
            {
                decoded += uri[i];
            }
        }
#if defined(_WIN32)
        if (decoded.size() >= 3 && decoded[0] == '/' && decoded[2] == ':')
        {
            decoded.erase(decoded.begin());
        }

        std::replace(decoded.begin(), decoded.end(), '/', '\\');
#endif
        return std::filesystem::path(decoded);
    }

    std::string UriFromPath(const std::filesystem::path& path)
    {
        std::string generic = path.generic_string();
        std::string uri = "file://";
        if (generic.empty() || generic.front() != '/')
        {
            uri += '/'; // drive-letter paths: file:///C:/dir
        }

        constexpr std::string_view digits = "0123456789ABCDEF";
        for (const char c: generic)
        {
            const unsigned char byte = static_cast<unsigned char>(c);
            const bool plain = (byte >= 'A' && byte <= 'Z') ||(byte >= 'a' && byte <= 'z') ||
                (byte >= '0' && byte <= '9') || c == '-' || c == '.' || c == '_' || c == '~' || c == '/' ||
                c == ':';
            if (plain)
            {
                uri += c;
            }
            else
            {
                uri += '%';
                uri += digits[byte >> 4];
                uri += digits[byte & 15];
            }
        }

        return uri;
    }

    void AppendPosition(Position position, std::string& out)
    {
        out += "{\"line\":" + std::to_string(position.line) + ",\"character\":" +
            std::to_string(position.character) + "}";
    }

    std::size_t OffsetFromPosition(std::string_view text, Position position)
    {
        LineIndex index;
        index.Build(text);
        return index.OffsetFromPosition(position);
    }

} // namespace heimdall::lsp
