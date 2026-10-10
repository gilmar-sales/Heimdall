#include <algorithm>
#include <Heimdall/SourceOverlay.hpp>
#include <Heimdall/SourceSnapshot.hpp>
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
            m_sources.insert_or_assign(Key(path), Source { std::move(source), {} });
        }
    }

    void SourceOverlay::AddSnapshot(const std::filesystem::path&          path,
                                    std::shared_ptr<const SourceSnapshot> source)
    {
        if (source)
        {
            m_sources.insert_or_assign(Key(path), Source { {}, std::move(source) });
        }
    }

    std::shared_ptr<const std::string> SourceOverlay::Find(const std::filesystem::path& path) const
    {
        const auto found = m_sources.find(Key(path));
        if (found == m_sources.end())
        {
            return nullptr;
        }
        return found->second.pieces ? found->second.pieces->Materialize() : found->second.text;
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
            const auto& entry  = m_sources.at(key);
            const auto  owned  = entry.pieces ? entry.pieces->Materialize() : entry.text;
            const auto& source = *owned;
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

    std::string SourceOverlay::PathFingerprint() const
    {
        std::vector<std::string> keys;
        keys.reserve(m_sources.size());
        for (const auto& [key, source] : m_sources)
        {
            keys.push_back(key);
        }
        std::ranges::sort(keys);
        std::string result;
        for (const auto& key : keys)
        {
            result += std::to_string(key.size()) + ":" + key;
        }
        return result;
    }
} // namespace heimdall
