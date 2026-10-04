#pragma once

#include <filesystem>
#include <vector>

namespace heimdall::cli
{

    bool IsSourceFile(const std::filesystem::path & path);
    bool CollectFiles(const std::vector<std::filesystem::path> & inputs,
        std::vector<std::filesystem::path> & files);

} // namespace heimdall::cli
