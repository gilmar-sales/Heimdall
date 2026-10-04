#include "FileDiscovery.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <system_error>

namespace heimdall::cli
{

    bool IsSourceFile(const std::filesystem::path & path)
    {
        const auto ext = path.extension().string();
        return ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".h" ||
            ext == ".hh" || ext == ".hpp" || ext == ".hxx";
    }

    bool CollectFiles(const std::vector<std::filesystem::path> & inputs,
        std::vector<std::filesystem::path> & files)
    {
        for (const auto & input: inputs)
        {
            std::error_code ec;
            if (!std::filesystem::exists(input, ec) || ec)
            {
                std::cerr << "path does not exist: " << input.string() << '\n';
                return false;
            }

            if (std::filesystem::is_regular_file(input, ec))
            {
                if (IsSourceFile(input))
                {
                    files.push_back(input);
                }
                else
                {
                    std::cerr << "skipping unsupported file: " << input.string() << '\n';
                }
            }
            else if (std::filesystem::is_directory(input, ec))
            {
                for (std::filesystem::recursive_directory_iterator it(input, ec), end; it != end && !ec;
                    it.increment(ec))
                {
                    if (it->is_regular_file(ec) && IsSourceFile(it->path()))
                    {
                        files.push_back(it->path());
                    }
                }

                if (ec)
                {
                    std::cerr << "error traversing directory " << input.string() << ": " << ec.message() << '\n';
                    return false;
                }
            }
            else
            {
                std::cerr << "not a regular file or directory: " << input.string() << '\n';
                return false;
            }
        }

        std::sort(files.begin(), files.end());
        files.erase(std::unique(files.begin(), files.end()), files.end());
        if (files.empty())
        {
            std::cerr << "no C++ source files found\n";
            return false;
        }

        return true;
    }

} // namespace heimdall::cli
