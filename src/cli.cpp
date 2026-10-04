#include "CliOptions.hpp"
#include "FileDiscovery.hpp"
#include "Init.hpp"
#include "Pipeline.hpp"
#include "Reporting.hpp"

#include <Heimdall/CompileDatabase.hpp>
#include <Heimdall/RuleConfig.hpp>

#include <filesystem>
#include <iostream>
#include <vector>

int main(int argc, char**argv)
{
    constexpr int kExitUsageError = 2;
    heimdall::cli::Options options{};
    if (!heimdall::cli::ParseOptions(argc, argv, options))
    {
        std::cerr << "usage: heimdall <lint|check|format|parse> [--jobs N] [--json|--fix|--fix-unsafe|--write] [--std <c++20|c++23|c++26>] [--compile-commands <path>] [--config <path>] <files-or-directories-or-globs...>\n";
        std::cerr << "       heimdall format [--pointer-alignment <left|right>] [--reference-alignment <left|right>] [--write] <files...>\n";
        std::cerr << "       heimdall init [directory] [--force]\n";
        std::cerr << "       globs support '*', '**', '?' and '[...]': e.g. src/**/*.cpp, src/**.cpp\n";
        return kExitUsageError;
    }

    if (options.command == heimdall::cli::Command::Init)
    {
        std::string error;
        std::filesystem::path created;
        if (!heimdall::cli::RunInit(options, error, created))
        {
            std::cerr << error << '\n';
            return kExitUsageError;
        }

        std::cout << "created " << created.string() << '\n';
        return 0;
    }

    if (options.command == heimdall::cli::Command::Lint || options.command == heimdall::cli::Command::Check)
    {
        if (options.rule_config_explicit)
        {
            auto loaded = heimdall::LoadRuleConfiguration(options.rule_config);
            if (!loaded)
            {
                std::cerr << loaded.error() << '\n';
                return kExitUsageError;
            }

            options.rule_options = std::move(loaded->options);
        }
        else
        {
            auto loaded = heimdall::FindRuleOptions(std::filesystem::current_path());
            if (!loaded)
            {
                std::cerr << loaded.error() << '\n';
                return kExitUsageError;
            }

            if (*loaded)
            {
                options.rule_options = std::move(* *loaded);
            }
        }

        options.rule_options.overrides.insert(options.rule_options.overrides.end(),
            options.rule_overrides.begin(), options.rule_overrides.end());
    }

    if (options.command == heimdall::cli::Command::Format)
    {
        if (options.rule_config_explicit)
        {
            auto loaded = heimdall::LoadRuleConfiguration(options.rule_config);
            if (!loaded)
            {
                std::cerr << loaded.error() << '\n';
                return kExitUsageError;
            }

            options.format_options = loaded->format_options;
        }
        else
        {
            auto loaded = heimdall::FindFormatOptions(std::filesystem::current_path());
            if (!loaded)
            {
                std::cerr << loaded.error() << '\n';
                return kExitUsageError;
            }

            if (*loaded)
            {
                options.format_options = * *loaded;
            }
        }

        // Explicit flags win over the config file.
        if (options.pointer_alignment_override)
        {
            options.format_options.pointer_alignment = options.pointer_alignment;
        }

        if (options.reference_alignment_override)
        {
            options.format_options.reference_alignment = options.reference_alignment;
        }
    }

    std::vector<std::filesystem::path> files;
    if (!heimdall::cli::CollectFiles(options.inputs, files))
    {
        return kExitUsageError;
    }

    heimdall::CompileDatabase database;
    const heimdall::CompileDatabase* database_ptr = nullptr;
    if (!options.compile_commands.empty())
    {
        auto loaded = heimdall::CompileDatabase::Load(options.compile_commands);
        if (!loaded)
        {
            std::cerr << loaded.error() << '\n';
            return kExitUsageError;
        }

        database = std::move(*loaded);
        database_ptr = &database;
    }

    if (options.command == heimdall::cli::Command::Format && !options.write && files.size() != 1)
    {
        std::cerr << "format without --write requires exactly one file\n";
        return kExitUsageError;
    }

    std::vector<heimdall::cli::FileResult> results;
    heimdall::cli::RunParallel(files, options, database_ptr, results);

    return heimdall::cli::ReportResults(results, options);
}
