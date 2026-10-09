#pragma once

#include <Heimdall/Formatter.hpp>
#include <Heimdall/RuleEngine.hpp>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace heimdall
{

    inline constexpr std::string_view RuleConfigFileName = ".heimdall.json";

    struct RuleConfiguration
    {
        RuleOptions              options;
        FormatOptions            format_options;
        bool                     root                           = false;
        bool                     has_suppressions               = false;
        bool                     has_include_order              = false;
        bool                     has_doc_scope                  = false;
        bool                     has_pointer_alignment          = false;
        bool                     has_reference_alignment        = false;
        bool                     has_blank_line_between_methods = false;
        bool                     has_max_parameters_per_line    = false;
        std::vector<std::string> format_overrides;
    };

    std::expected<RuleConfiguration, std::string> LoadRuleConfiguration(
        const std::filesystem::path& path);

    std::expected<std::vector<RuleConfiguration>, std::string> FindConfigurations(
        const std::filesystem::path& directory);

    std::expected<std::optional<RuleOptions>, std::string> FindRuleOptions(
        const std::filesystem::path& directory);

    std::expected<std::optional<FormatOptions>, std::string> FindFormatOptions(
        const std::filesystem::path& directory);

} // namespace heimdall
