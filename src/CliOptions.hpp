#pragma once

#include <Heimdall/CppStandard.hpp>
#include <Heimdall/Formatter.hpp>
#include <Heimdall/RuleConfig.hpp>

#include <cstddef>
#include <filesystem>
#include <vector>
#include <string_view>

namespace heimdall::cli
{

    enum class Command
    {
        Lint,
        Check,
        Format,
        Parse,
        Init
    };

    struct Options
    {
        Command command{};
        std::size_t jobs = 1;
        bool json = false;
        bool write = false;
        bool fix = false;
        bool fix_unsafe = false;
        bool semantic = false;
        bool force = false;
        bool std_override = false;
        heimdall::CppStandard standard = heimdall::CppStandard::Cpp20;
        std::filesystem::path compile_commands;
        std::filesystem::path rule_config;
        bool rule_config_explicit = false;
        heimdall::RuleOptions rule_options;
        std::vector<heimdall::RuleOverride> rule_overrides;
        std::vector<std::filesystem::path> inputs;
        bool pointer_alignment_override = false;
        heimdall::PointerAlignment pointer_alignment = heimdall::PointerAlignment::Left;
        bool reference_alignment_override = false;
        heimdall::ReferenceAlignment reference_alignment = heimdall::ReferenceAlignment::Left;
        heimdall::FormatOptions format_options;
    };

    bool ParseStandardValue(std::string_view value, heimdall::CppStandard &standard);
    bool ParsePointerAlignment(std::string_view value, heimdall::PointerAlignment &alignment);
    bool ParseReferenceAlignment(std::string_view value, heimdall::ReferenceAlignment &alignment);
    bool ParseOptions(int argc, char **argv, Options &options);

} // namespace heimdall::cli
