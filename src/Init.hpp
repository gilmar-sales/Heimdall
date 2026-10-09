#pragma once

#include "CliOptions.hpp"

#include <filesystem>
#include <string>

namespace heimdall::cli
{

    std::string DefaultConfigText();

    /** @brief Creates <directory>/.heimdall.json from DefaultConfigText().
        @param options see @ref heimdall::cli::Options
        @param error
        @param created_path holding the attempted target path (if any)
        @returns true on success; on failure returns false with `error` set
     */
    bool RunInit(const Options& options, std::string& error, std::filesystem::path& created_path);

} // namespace heimdall::cli
