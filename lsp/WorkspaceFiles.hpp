#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace heimdall::lsp
{

	/** Discovery evidence; an empty or limited result is not implicitly complete. */
	struct WorkspaceDiscovery
	{
		std::vector<std::filesystem::path> files;
		bool complete  = false;
		bool cancelled = false;
		bool truncated = false;
		std::error_code error;
	};

	inline constexpr std::size_t kMaxWorkspaceFiles            = 20000;
	inline constexpr std::uintmax_t kBytesPerKibibyte          = 1024;
	inline constexpr std::uintmax_t kBytesPerMebibyte          = kBytesPerKibibyte * kBytesPerKibibyte;
	inline constexpr std::uintmax_t kMaxWorkspaceFileMebibytes = 2;
	inline constexpr std::uintmax_t kMaxWorkspaceFileBytes =
		kMaxWorkspaceFileMebibytes* kBytesPerMebibyte;

	/** Discover files and preserve cancellation, truncation and traversal failures. */
	[[nodiscard]] WorkspaceDiscovery DiscoverWorkspaceSourcesDetailed(
		const std::filesystem::path& root,
		std::stop_token stop,
		std::size_t maxFiles = kMaxWorkspaceFiles);

	/** Project-only discovery for background diagnostics: like
	 * DiscoverWorkspaceSourcesDetailed but skips dependency sources
	 * (see IsDependencySource), so lint rules run on the open project and
	 * not on vendored/FetchContent code. The symbol index keeps using the
	 * full discovery above, so navigation and completion still see
	 * dependencies. */
	[[nodiscard]] WorkspaceDiscovery DiscoverProjectSourcesDetailed(
		const std::filesystem::path& root,
		std::stop_token stop,
		std::size_t maxFiles = kMaxWorkspaceFiles);

	/// C and C++ source or header extension that the workspace passes look at.
	[[nodiscard]] bool IsWorkspaceSource(const std::filesystem::path& file);

	/// Dependency directory name (FetchContent `_deps`, vendored
	/// `third_party`/`external`/`vendor` trees): never the open project.
	[[nodiscard]] bool IsDependencyDirectory(std::string_view name);

	/// True when `file` lives under a dependency directory below `root`.
	[[nodiscard]] bool IsDependencySource(
		const std::filesystem::path& root,
		const std::filesystem::path& file);

	/// Build output, VCS metadata and dependency caches are never descended into.
	[[nodiscard]] bool IsSkippedDirectory(std::string_view name);

	/// Sorted sources below `root`, at most `maxFiles`. Stops early (with what it has found so
	/// far) when `stop` is requested.
	[[nodiscard]] std::vector<std::filesystem::path> DiscoverWorkspaceSources(
		const std::filesystem::path& root,
		std::stop_token stop,
		std::size_t maxFiles = kMaxWorkspaceFiles);

	/// Sorted project sources below `root` (dependency sources excluded),
	/// at most `maxFiles`. Stops early (with what it has found so far) when
	/// `stop` is requested.
	[[nodiscard]] std::vector<std::filesystem::path> DiscoverProjectSources(
		const std::filesystem::path& root,
		std::stop_token stop,
		std::size_t maxFiles = kMaxWorkspaceFiles);

	/// Whole file, or nothing when it is unreadable or larger than `kMaxWorkspaceFileBytes`.
	[[nodiscard]] std::optional<std::string> ReadWorkspaceFile(const std::filesystem::path& file);

	/// Identity of a file independent of how a URI spells it (drive-letter case, `%3A`,
	/// separators, `..`); equal for every spelling of the same path.
	[[nodiscard]] std::string FileKey(const std::filesystem::path& path);

	[[nodiscard]] bool IsUnderRoot(
		const std::filesystem::path& path,
		const std::filesystem::path& root);

} // namespace heimdall::lsp
