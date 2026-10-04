#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

namespace heimdall::cli
{

    bool IsSourceFile(const std::filesystem::path & path);
    // Matches a normalized ('/' separators) relative path against a glob
    // pattern. Supports `*` (any chars except '/'), `**` (any chars including
    // '/'), `**/` (zero or more directories), `?` and `[...]` classes.
    bool MatchGlobPattern(std::string_view pattern, std::string_view text);
    bool CollectFiles(const std::vector<std::filesystem::path> & inputs,
        std::vector<std::filesystem::path> & files);

} // namespace heimdall::cli
