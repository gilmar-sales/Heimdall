#include "CliOptions.hpp"
#include "FileDiscovery.hpp"
#include "Pipeline.hpp"
#include "Reporting.hpp"

#include <Heimdall/CompileDatabase.hpp>
#include <Heimdall/RuleConfig.hpp>

#include <filesystem>
#include <iostream>
#include <vector>

int main(int argc, char **argv)
{
    heimdall::cli::Options options
    {};
    if (!heimdall::cli::ParseOptions(argc, argv, options))
    {
        std::cerr << "usage: heimdall <lint|check|format|parse> [--jobs N] [--json|--fix|--write] [--std <c++20|c++23|c++26>] [--compile-commands <path>] [--config <path>] <files-or-directories...>\n";
        return 2;
    }

    if (options.command == heimdall::cli::Command::Lint || options.command == heimdall::cli::Command::Check)
    {
        if (options.rule_config_explicit)
        {
            auto loaded = heimdall::LoadRuleConfiguration(options.rule_config);
            if (!loaded)
            {
                std::cerr << loaded.error() << '\n';
                return 2;
            }
            options.rule_options = std::move(loaded->options);
        }
        else
        {
            auto loaded = heimdall::FindRuleOptions(std::filesystem::current_path());
            if (!loaded)
            {
                std::cerr << loaded.error() << '\n';
                return 2;
            }
            if (*loaded)
            {
                options.rule_options = std::move(**loaded);
            }
        }
        options.rule_options.overrides.insert(options.rule_options.overrides.end(),
            options.rule_overrides.begin(), options.rule_overrides.end());
    }

    std::vector<std::filesystem::path> files;
    if (!heimdall::cli::CollectFiles(options.inputs, files))
    {
        return 2;
    }

    heimdall::CompileDatabase database;
    const heimdall::CompileDatabase * database_ptr = nullptr;
    if (!options.compile_commands.empty())
    {
        auto loaded = heimdall::CompileDatabase::Load(options.compile_commands);
        if (!loaded)
        {
            std::cerr << loaded.error() << '\n';
            return 2;
        }

        database = std::move(*loaded);
        database_ptr = &database;
    }

    if (options.command == heimdall::cli::Command::Format && !options.write && files.size() != 1)
    {
        std::cerr << "format without --write requires exactly one file\n";
        return 2;
    }

    std::vector<heimdall::cli::FileResult> results;
    heimdall::cli::RunParallel(files, options, database_ptr, results);

    return heimdall::cli::ReportResults(results, options);
}
