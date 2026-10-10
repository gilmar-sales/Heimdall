#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace heimdall::lsp
{

    inline constexpr std::size_t kMaxWorkspaceFiles                = 20000;
    inline constexpr std::uintmax_t kBytesPerKibibyte              = 1024;
    inline constexpr std::uintmax_t kBytesPerMebibyte              = kBytesPerKibibyte * kBytesPerKibibyte;
    inline constexpr std::uintmax_t kMaxWorkspaceFileMebibytes     = 2;
    inline constexpr std::uintmax_t kMaxWorkspaceFileBytes =
        kMaxWorkspaceFileMebibytes * kBytesPerMebibyte;

    /// C and C++ source or header extension that the workspace passes look at.
    [[nodiscard]] bool IsWorkspaceSource(const std::filesystem::path& file);

    /// Build output, VCS metadata and dependency caches are never descended into.
    [[nodiscard]] bool IsSkippedDirectory(std::string_view name);

    /// Sorted sources below `root`, at most `maxFiles`. Stops early (with what it has found so
    /// far) when `stop` is requested.
    [[nodiscard]] std::vector<std::filesystem::path> DiscoverWorkspaceSources(
        const std::filesystem::path& root,
        std::stop_token stop,
        std::size_t maxFiles = kMaxWorkspaceFiles);

    /// Whole file, or nothing when it is unreadable or larger than `kMaxWorkspaceFileBytes`.
    [[nodiscard]] std::optional<std::string> ReadWorkspaceFile(const std::filesystem::path& file);

    /// Identity of a file independent of how a URI spells it (drive-letter case, `%3A`,
    /// separators, `..`); equal for every spelling of the same path.
    [[nodiscard]] std::string FileKey(const std::filesystem::path& path);

    [[nodiscard]] bool IsUnderRoot(const std::filesystem::path& path,
                                   const std::filesystem::path& root);

} // namespace heimdall::lsp
