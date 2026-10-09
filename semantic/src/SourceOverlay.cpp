#include <Heimdall/SourceOverlay.hpp>
#include <algorithm>
#include <vector>

namespace heimdall
{
    std::string SourceOverlay::Key(const std::filesystem::path& path)
    {
        std::error_code error;
        const auto      absolute = std::filesystem::absolute(path, error);
        auto            key      = (error ? path : absolute).lexically_normal().generic_string();
#ifdef _WIN32
        for (auto& c : key)
        {
            if (c >= 'A' && c <= 'Z')
            {
                c += 'a' - 'A';
            }
        }
#endif
        return key;
    }

    void SourceOverlay::Add(
        const std::filesystem::path& path, std::shared_ptr<const std::string> source)
    {
        if (source)
        {
            m_sources.insert_or_assign(Key(path), std::move(source));
        }
    }

    std::shared_ptr<const std::string> SourceOverlay::Find(const std::filesystem::path& path) const
    {
        const auto found = m_sources.find(Key(path));
        return found == m_sources.end() ? nullptr : found->second;
    }

    std::string SourceOverlay::Fingerprint() const
    {
        // Exact, length-delimited content: cache equality cannot hide a dirty
        // buffer through an mtime collision or a content-hash collision.
        std::vector<std::string> keys;
        for (const auto& [key, source] : m_sources)
        {
            keys.push_back(key);
        }

        std::ranges::sort(keys);
        std::string result;
        for (const auto& key : keys)
        {
            const auto& source = *m_sources.at(key);
            result += std::to_string(key.size()) + ":" + key;
            result += std::to_string(source.size()) + ":" + source;
        }

        return result;
    }

    std::string SourceOverlay::Fingerprint(std::span<const std::filesystem::path> paths) const
    {
        SourceOverlay selected;
        for (const auto& path : paths)
        {
            selected.Add(path, Find(path));
        }

        return selected.Fingerprint();
    }
} // namespace heimdall
