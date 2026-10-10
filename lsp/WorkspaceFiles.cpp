#include "WorkspaceFiles.hpp"

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
        static constexpr std::string_view kExtensions[] = {
            ".cpp", ".cc", ".cxx", ".c++", ".hpp", ".hh", ".hxx",
            ".h",   ".ipp", ".tpp", ".inl", ".cppm", ".ixx",
        };
        const std::string extension = file.extension().string();
        return std::ranges::find(kExtensions, extension) != std::end(kExtensions);
    }

    bool IsSkippedDirectory(std::string_view name)
    {
        return name.starts_with('.') || name.starts_with("build") || name.starts_with("cmake-build") ||
               name == "node_modules" || name == "_deps" || name == "vcpkg_installed" ||
               name == "out";
    }

    std::vector<std::filesystem::path> DiscoverWorkspaceSources(
        const std::filesystem::path& root,
        std::stop_token stop,
        std::size_t maxFiles)
    {
        std::vector<std::filesystem::path> files;
        std::error_code error;
        if (root.empty() || !std::filesystem::is_directory(root, error))
        {
            return files;
        }

        for (std::filesystem::recursive_directory_iterator
                 it(root, std::filesystem::directory_options::skip_permission_denied, error),
             end;
             !error && it != end && files.size() < maxFiles; it.increment(error))
        {
            if (stop.stop_requested())
            {
                break;
            }

            if (it->is_directory(error))
            {
                if (IsSkippedDirectory(it->path().filename().string()))
                {
                    it.disable_recursion_pending();
                }
            }
            else if (IsWorkspaceSource(it->path()))
            {
                files.push_back(it->path());
            }
        }

        std::ranges::sort(files);
        return files;
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
        std::ranges::transform(key, key.begin(), [](char c)
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

        const std::string key        = FileKey(path);
        std::string prefix           = FileKey(root);
        if (!prefix.empty() && prefix.back() != '/')
        {
            prefix += '/';
        }

        return key.starts_with(prefix);
    }

} // namespace heimdall::lsp
