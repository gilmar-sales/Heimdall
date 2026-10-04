#pragma once

#include "CliOptions.hpp"

#include <filesystem>
#include <string>

namespace heimdall::cli
{

    std::string DefaultConfigText();

    // Creates <directory>/.heimdall.json from DefaultConfigText().
    // Returns true on success; on failure returns false with `error` set
    // and `created_path` holding the attempted target path (if any).
    bool RunInit(const Options &options, std::string &error, std::filesystem::path &created_path);

} // namespace heimdall::cli
