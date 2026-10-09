#include "CliOptions.hpp"

#include <charconv>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace heimdall::cli
{

    bool ParseStandardValue(std::string_view value, heimdall::CppStandard& standard)
    {
        if (value == "c++20" || value == "gnu++20" || value == "c++2a" || value == "gnu++2a")
        {
            standard = heimdall::CppStandard::Cpp20;
            return true;
        }

        if (value == "c++23" || value == "gnu++23" || value == "c++2b" || value == "gnu++2b")
        {
            standard = heimdall::CppStandard::Cpp23;
            return true;
        }

        if (value == "c++26" || value == "gnu++26" || value == "c++2c" || value == "gnu++2c" ||
            value == "c++latest")
        {
            standard = heimdall::CppStandard::Cpp26;
            return true;
        }

        return false;
    }

    bool ParsePointerAlignment(std::string_view value, heimdall::PointerAlignment& alignment)
    {
        if (value == "left")
        {
            alignment = heimdall::PointerAlignment::Left;
            return true;
        }

        if (value == "right")
        {
            alignment = heimdall::PointerAlignment::Right;
            return true;
        }

        return false;
    }

    bool ParseReferenceAlignment(std::string_view value, heimdall::ReferenceAlignment& alignment)
    {
        if (value == "left")
        {
            alignment = heimdall::ReferenceAlignment::Left;
            return true;
        }

        if (value == "right")
        {
            alignment = heimdall::ReferenceAlignment::Right;
            return true;
        }

        return false;
    }

    bool ParseOptions(int argc, char** argv, Options& options)
    {
        constexpr int              kMinArgcForCommand        = 2;
        constexpr int              kFirstOptionIndex         = 2;
        constexpr std::string_view kStdPrefix                = "--std=";
        constexpr std::string_view kPointerAlignmentPrefix   = "--pointer-alignment=";
        constexpr std::string_view kReferenceAlignmentPrefix = "--reference-alignment=";
        if (argc < kMinArgcForCommand)
        {
            return false;
        }

        const std::string_view command = argv[1];
        if (command == "lint")
        {
            options.command = Command::Lint;
        }
        else if (command == "check")
        {
            options.command = Command::Check;
        }
        else if (command == "format")
        {
            options.command = Command::Format;
        }
        else if (command == "parse")
        {
            options.command = Command::Parse;
        }
        else if (command == "init")
        {
            options.command = Command::Init;
        }
        else
        {
            return false;
        }

        if (options.command == Command::Init)
        {
            for (int i = kFirstOptionIndex; i < argc; ++i)
            {
                const std::string_view arg = argv[i];
                if (arg == "--force" || arg == "-f")
                {
                    options.force = true;
                }
                else if (!arg.empty() && arg.front() == '-')
                {
                    std::cerr << "unknown option for init: " << arg << '\n';
                    return false;
                }
                else
                {
                    options.inputs.emplace_back(arg);
                }
            }

            if (options.inputs.size() > 1)
            {
                std::cerr << "init accepts at most one directory\n";
                return false;
            }

            return true;
        }

        options.jobs = std::max<std::size_t>(1u, std::thread::hardware_concurrency());

        for (int i = kFirstOptionIndex; i < argc; ++i)
        {
            const std::string_view arg = argv[i];
            if (arg == "--json")
            {
                options.json = true;
            }
            else if (arg == "--write")
            {
                options.write = true;
            }
            else if (arg == "--fix")
            {
                options.fix = true;
            }
            else if (arg == "--fix-unsafe")
            {
                options.fix        = true;
                options.fix_unsafe = true;
            }
            else if (arg == "--semantic")
            {
                options.semantic = true;
            }
            else if (arg == "--compile-commands" && i + 1 < argc)
            {
                options.compile_commands = argv[++i];
            }
            else if (arg == "--config" && i + 1 < argc)
            {
                options.rule_config          = argv[++i];
                options.rule_config_explicit = true;
            }
            else if (arg == "--rule" && i + 1 < argc)
            {
                const std::string_view value = argv[++i];
                const auto             equal = value.find('=');
                if (equal == std::string_view::npos || equal == 0)
                {
                    std::cerr << "invalid --rule value: " << value
                              << " (expected code=off|warning|error)\n";
                    return false;
                }

                heimdall::RuleOverride override;
                override.code      = value.substr(0, equal);
                const auto setting = value.substr(equal + 1);
                if (setting == "off")
                {
                    override.enabled = false;
                }
                else if (setting == "warning")
                {
                    override.severity = heimdall::Severity::Warning;
                }
                else if (setting == "error")
                {
                    override.severity = heimdall::Severity::Error;
                }
                else
                {
                    std::cerr << "invalid --rule value: " << value
                              << " (expected code=off|warning|error)\n";
                    return false;
                }

                options.rule_overrides.push_back(std::move(override));
            }
            else if ((arg == "--std" && i + 1 < argc) ||
                     (arg.starts_with(kStdPrefix) && arg.size() > kStdPrefix.size()))
            {
                const std::string_view value =
                    arg.starts_with(kStdPrefix) ? std::string_view(arg).substr(kStdPrefix.size())
                                                : std::string_view(argv[++i]);
                if (!ParseStandardValue(value, options.standard))
                {
                    std::cerr << "invalid --std value: " << value
                              << " (expected c++20, c++23 or c++26)\n";
                    return false;
                }

                options.std_override = true;
            }
            else if ((arg == "--pointer-alignment" && i + 1 < argc) ||
                     (arg.starts_with("--pointer-alignment=") &&
                      arg.size() > kPointerAlignmentPrefix.size()))
            {
                const std::string_view value =
                    arg.starts_with(kPointerAlignmentPrefix)
                        ? std::string_view(arg).substr(kPointerAlignmentPrefix.size())
                        : std::string_view(argv[++i]);
                if (!ParsePointerAlignment(value, options.pointer_alignment))
                {
                    std::cerr << "invalid --pointer-alignment value: " << value
                              << " (expected left or right)\n";
                    return false;
                }

                options.pointer_alignment_override = true;
            }
            else if ((arg == "--reference-alignment" && i + 1 < argc) ||
                     (arg.starts_with("--reference-alignment=") &&
                      arg.size() > kReferenceAlignmentPrefix.size()))
            {
                const std::string_view value =
                    arg.starts_with(kReferenceAlignmentPrefix)
                        ? std::string_view(arg).substr(kReferenceAlignmentPrefix.size())
                        : std::string_view(argv[++i]);
                if (!ParseReferenceAlignment(value, options.reference_alignment))
                {
                    std::cerr << "invalid --reference-alignment value: " << value
                              << " (expected left or right)\n";
                    return false;
                }

                options.reference_alignment_override = true;
            }
            else if (arg == "--jobs" && i + 1 < argc)
            {
                const std::string_view value = argv[++i];
                std::size_t            jobs  = 0;
                const auto             parsed =
                    std::from_chars(value.data(), value.data() + value.size(), jobs);
                if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size() ||
                    jobs == 0)
                {
                    std::cerr << "invalid --jobs value: " << value << '\n';
                    return false;
                }

                options.jobs = jobs;
            }
            else if (arg == "--force" || arg == "-f")
            {
                std::cerr << "--force is only supported by init\n";
                return false;
            }
            else if (!arg.empty() && arg.front() == '-')
            {
                std::cerr << "unknown option: " << arg << '\n';
                return false;
            }
            else
            {
                options.inputs.emplace_back(arg);
            }
        }

        if (options.inputs.empty())
        {
            std::cerr << "no input paths provided\n";
            return false;
        }

        if (options.json && (options.command == Command::Format))
        {
            std::cerr << "--json is only supported by lint/check/parse\n";
            return false;
        }

        if (options.json && options.fix)
        {
            std::cerr << "--json cannot be combined with --fix\n";
            return false;
        }

        if (options.write && options.command != Command::Format)
        {
            std::cerr << "--write is only supported by format\n";
            return false;
        }

        if (options.fix && options.command != Command::Lint)
        {
            std::cerr << "--fix is only supported by lint\n";
            return false;
        }

        if (options.semantic && options.compile_commands.empty())
        {
            std::cerr << "--semantic requires --compile-commands <path>\n";
            return false;
        }

        if (!options.rule_overrides.empty() && options.command != Command::Lint &&
            options.command != Command::Check)
        {
            std::cerr << "--rule is only supported by lint/check\n";
            return false;
        }

        if ((options.pointer_alignment_override || options.reference_alignment_override) &&
            options.command != Command::Format)
        {
            std::cerr
                << "--pointer-alignment and --reference-alignment are only supported by format\n";
            return false;
        }

        if (options.rule_config_explicit && options.command != Command::Lint &&
            options.command != Command::Check && options.command != Command::Format)
        {
            std::cerr << "--config is only supported by lint/check/format\n";
            return false;
        }

        for (const auto& override : options.rule_overrides)
        {
            if (!heimdall::IsKnownRuleCode(override.code))
            {
                std::cerr << "unknown rule code: " << override.code << '\n';
                return false;
            }
        }

        return true;
    }

} // namespace heimdall::cli
