#include "WorkspaceFiles.hpp"

#include <Heimdall/ModuleUnit.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>

namespace heimdall::lsp
{

	bool IsWorkspaceSource(const std::filesystem::path& file)
	{
		if (file.stem().extension() == ".erb")
		{
			return false;
		}

		static constexpr std::string_view kExtensions[] = {
			".cpp", ".cc", ".cxx", ".c++", ".hpp", ".hh", ".hxx",
			".h", ".ipp", ".tpp", ".inl",
		};
		const std::string extension = file.extension().string();
		return std::ranges::find(kExtensions, extension) != std::end(kExtensions) ||
			IsModuleUnitExtension(extension);
	}

	bool IsSkippedDirectory(std::string_view name)
	{
		return name.starts_with('.') || name.starts_with("build") || name.starts_with("cmake-build") ||
			name == "node_modules" || name == "_deps" || name == "vcpkg_installed" ||
			name == "out";
	}

	bool IsDependencyDirectory(std::string_view name)
	{
		return name == "_deps" || name == "third_party" || name == "thirdparty" ||
			name == "external" || name == "extern" || name == "vendor";
	}

	bool IsDependencySource(const std::filesystem::path& root, const std::filesystem::path& file)
	{
		if (root.empty())
		{
			return false;
		}

		std::error_code error;
		const auto relative = std::filesystem::relative(file, root, error);
		if (error)
		{
			return false;
		}

		const std::string text = relative.generic_string();
		if (text.empty() || text == ".." || text.starts_with("../"))
		{
			return false;
		}

		for (const auto& component : relative)
		{
			if (IsDependencyDirectory(component.generic_string()))
			{
				return true;
			}
		}

		return false;
	}

	namespace
	{

		WorkspaceDiscovery DiscoverSourcesDetailed(
			const std::filesystem::path& root,
			std::stop_token stop,
			std::size_t maxFiles,
			bool skipDependencies)
		{
			WorkspaceDiscovery result;
			auto& files = result.files;
			if (stop.stop_requested())
			{
				result.cancelled = true;
				return result;
			}

			std::error_code error;
			if (root.empty() ||!std::filesystem::is_directory(root, error))
			{
				result.error = error;
				return result;
			}

			for (std::filesystem::recursive_directory_iterator
				it(
				root,
				std::filesystem::directory_options::none,
				error),
				end;
				!error && it != end; it.increment(error))
			{
				if (stop.stop_requested())
				{
					result.cancelled = true;
					break;
				}

				const auto name       = it->path().filename().string();
				const auto parentName = it->path().parent_path().filename().string();
				const bool buildParent = parentName.starts_with("build") ||
					parentName.starts_with("cmake-build") || parentName == "out";
				if (it->is_directory(error))
				{
					const bool buildDirectory = name.starts_with("build") ||
						name.starts_with("cmake-build") || name == "out";
					const bool dependencyDirectory = name == "_deps";
					if (skipDependencies && IsDependencyDirectory(name))
					{
						it.disable_recursion_pending();
					}
					else if ((buildParent && !dependencyDirectory) ||
						(parentName == "_deps" && !name.ends_with("-src")) ||
						(IsSkippedDirectory(name) && !buildDirectory && !dependencyDirectory))
					{
						it.disable_recursion_pending();
					}
				}
				else if (!buildParent && parentName != "_deps" && IsWorkspaceSource(it->path()) &&
					!(skipDependencies && IsDependencySource(root, it->path())))
				{
					if (files.size() == maxFiles)
					{
						result.truncated = true;
						break;
					}

					files.push_back(it->path());
				}
			}

			std::ranges::sort(files);
			result.error    = error;
			result.complete = !error && !result.cancelled && !result.truncated;
			return result;
		}

	} // namespace

	WorkspaceDiscovery DiscoverWorkspaceSourcesDetailed(
		const std::filesystem::path& root,
		std::stop_token stop,
		std::size_t maxFiles)
	{
		return DiscoverSourcesDetailed(root, stop, maxFiles, false);
	}

	WorkspaceDiscovery DiscoverProjectSourcesDetailed(
		const std::filesystem::path& root,
		std::stop_token stop,
		std::size_t maxFiles)
	{
		return DiscoverSourcesDetailed(root, stop, maxFiles, true);
	}

	std::vector<std::filesystem::path> DiscoverWorkspaceSources(
		const std::filesystem::path& root,
		const std::stop_token& stop,
		std::size_t maxFiles)
	{
		return DiscoverWorkspaceSourcesDetailed(root, stop, maxFiles).files;
	}

	std::vector<std::filesystem::path> DiscoverProjectSources(
		const std::filesystem::path& root,
		const std::stop_token& stop,
		std::size_t maxFiles)
	{
		return DiscoverProjectSourcesDetailed(root, stop, maxFiles).files;
	}

	std::optional<std::string> ReadWorkspaceFile(const std::filesystem::path& file)
	{
		std::error_code error;
		const auto size = std::filesystem::file_size(file, error);
		if (error || size > kMaxWorkspaceFileBytes)
		{
			return std::nullopt;
		}

		std::ifstream input(file, std::ios::binary);
		if (!input)
		{
			return std::nullopt;
		}

		return std::string((std::istreambuf_iterator<char>(input)),
			std::istreambuf_iterator<char>());
	}

	std::string FileKey(const std::filesystem::path& path)
	{
		std::string key = path.lexically_normal().generic_string();
#if defined(_WIN32)
		std::ranges::transform(key, key.begin(),[](char c)
			{
				return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			});
#endif
		return key;
	}

	bool IsUnderRoot(const std::filesystem::path& path, const std::filesystem::path& root)
	{
		if (root.empty())
		{
			return false;
		}

		const std::string key = FileKey(path);
		std::string prefix    = FileKey(root);
		if (!prefix.empty() && prefix.back() != '/')
		{
			prefix += '/';
		}

		return key.starts_with(prefix);
	}

} // namespace heimdall::lsp
