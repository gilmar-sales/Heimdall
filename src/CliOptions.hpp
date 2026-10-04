#pragma once

#include <Heimdall/CppStandard.hpp>
#include <Heimdall/RuleConfig.hpp>

#include <cstddef>
#include <filesystem>
#include <vector>

namespace heimdall::cli
{

    enum class Command
    {
        Lint,
        Check,
        Format,
        Parse
    };

    struct Options
    {
        Command command{};
        std::size_t jobs = 1;
        bool json = false;
        bool write = false;
        bool fix = false;
        bool semantic = false;
        bool std_override = false;
        heimdall::CppStandard standard = heimdall::CppStandard::Cpp20;
        std::filesystem::path compile_commands;
        std::filesystem::path rule_config;
        bool rule_config_explicit = false;
        heimdall::RuleOptions rule_options;
        std::vector<heimdall::RuleOverride> rule_overrides;
        std::vector<std::filesystem::path> inputs;
    };

    bool ParseStandardValue(std::string_view value, heimdall::CppStandard & standard);
    bool ParseOptions(int argc, char **argv, Options &options);

} // namespace heimdall::cli
