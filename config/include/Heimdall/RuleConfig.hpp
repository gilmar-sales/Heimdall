#pragma once

#include <Heimdall/RuleEngine.hpp>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace heimdall
{

    inline constexpr std::string_view RuleConfigFileName = ".heimdall.json";

    struct RuleConfiguration
    {
        RuleOptions options;
        bool root = false;
        bool has_suppressions = false;
    };

    std::expected<RuleConfiguration, std::string> LoadRuleConfiguration(const std::filesystem::path & path);
    std::expected<std::optional<RuleOptions>, std::string> FindRuleOptions(
        const std::filesystem::path & directory);

} // namespace heimdall
