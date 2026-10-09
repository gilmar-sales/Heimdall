#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <span>
#include <unordered_map>

namespace heimdall
{
    // Construct before publishing through shared_ptr<const SourceOverlay>.
    // Open buffers win over disk, including buffers of not-yet-created files.
    class SourceOverlay
    {
    public:
        void Add(const std::filesystem::path& path, std::shared_ptr<const std::string> source);

        std::shared_ptr<const std::string> Find(const std::filesystem::path& path) const;

        std::string Fingerprint() const;

        std::string Fingerprint(std::span<const std::filesystem::path> paths) const;

        bool Empty() const noexcept
        {
            return m_sources.empty();
        }

    private:
        static std::string Key(const std::filesystem::path& path);

        std::unordered_map<std::string, std::shared_ptr<const std::string>> m_sources;
    };
}
