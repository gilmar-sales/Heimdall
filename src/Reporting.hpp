#pragma once

#include "CliOptions.hpp"
#include "Pipeline.hpp"

#include <string>
#include <string_view>
#include <vector>
#include <filesystem>

namespace heimdall::cli
{

    std::string JsonEscape(std::string_view text);

    bool WriteFile(const std::filesystem::path& path, std::string_view data);

    // Prints diagnostics / parse trees and performs --fix/--write side effects.
    // Returns the process exit code (0 ok, 1 diagnostics, 2 io error).
    int ReportResults(const std::vector<FileResult>& results, const Options& options);

} // namespace heimdall::cli
