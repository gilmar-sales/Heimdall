#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <Heimdall/CompileDatabase.hpp>
#include <Heimdall/Profiling.hpp>
#include <Heimdall/RuleConfig.hpp>
#include <Heimdall/Trace.hpp>

#include "CliOptions.hpp"
#include "FileDiscovery.hpp"
#include "Init.hpp"
#include "Pipeline.hpp"
#include "Reporting.hpp"

int main(int argc, char**argv)
{
	constexpr int kExitUsageError = 2;
	const auto tracePath          = heimdall::profiling::OutputPathFromEnvironment();
	const auto traceSession = tracePath
	? heimdall::profiling::Session::Start(*tracePath)
	: nullptr;
	if (tracePath && !traceSession)
	{
		std::cerr <<(heimdall::profiling::Available()
			? "heimdall: failed to start profiling; check trace settings and temporary storage\n"
			: "heimdall: HEIMDALL_TRACE ignored, build with -DHEIMDALL_PROFILING=ON\n");
	}

	heimdall::trace::NameThread("cli-main");
	const heimdall::trace::Scope traceheimdall(heimdall::trace::Category::Cli, "heimdall");
	heimdall::cli::Options options {};
	if (!heimdall::cli::ParseOptions(argc, argv, options))
	{
		std::cerr
		<< "usage: heimdall <lint|check|format|parse> [--jobs N] "
			"[--json|--fix|--fix-unsafe|--write] [--std <c++20|c++23|c++26>] "
			"[--compile-commands <path>] [--config <path>] <files-or-directories-or-globs...>\n";
		std::cerr << "       heimdall format [--pointer-alignment <left|right>] "
			"[--reference-alignment <left|right>] [--write] <files...>\n";
		std::cerr << "       heimdall init [directory] [--force]\n";
		std::cerr
		<< "       globs support '*', '**', '?' and '[...]': e.g. src/**/*.cpp, src/**.cpp\n";
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

	if (options.command == heimdall::cli::Command::Lint ||
		options.command == heimdall::cli::Command::Check)
	{
		// An explicit --config applies to every file. Otherwise each file
		// resolves its own `.heimdall.json` chain (file dir up to the git
		// root) in ProcessFile, so `heimdall lint subdir` honors
		// `subdir/.heimdall.json`.
		if (options.rule_config_explicit)
		{
			auto loaded = heimdall::LoadRuleConfiguration(options.rule_config);
			if (!loaded)
			{
				std::cerr << loaded.error() << '\n';
				return kExitUsageError;
			}

			options.rule_options = std::move(loaded->options);
			options.rule_options.overrides.insert(
				options.rule_options.overrides.end(), options.rule_overrides.begin(),
				options.rule_overrides.end());
		}
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
			if (loaded->has_include_order)
			{
				options.format_options.includeOrder = loaded->options.include_order;
				options.format_options.includeCaseInsensitive =
					loaded->options.include_case_insensitive;
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
	}

	std::vector<std::filesystem::path> files;
	bool collected = false;
	{
		const heimdall::trace::Scope traceCollectFiles(heimdall::trace::Category::Startup, "CollectFiles");
		collected = heimdall::cli::CollectFiles(options.inputs, files);
	}

	if (!collected)
	{
		return kExitUsageError;
	}

	heimdall::trace::Counter(heimdall::trace::Category::Startup, "input files",
		static_cast<std::int64_t>(files.size()));

	heimdall::CompileDatabase database;
	const heimdall::CompileDatabase* database_ptr = nullptr;
	if (!options.compile_commands.empty())
	{
		auto loaded = [&]
		{
			const heimdall::trace::Scope traceLoad(heimdall::trace::Category::Startup, "CompileDatabase::Load",
				[&]
				{
					return options.compile_commands.generic_string();
				});
			return heimdall::CompileDatabase::Load(options.compile_commands);
		}
		();
		if (!loaded)
		{
			std::cerr << loaded.error() << '\n';
			return kExitUsageError;
		}

		database     = std::move(*loaded);
		database_ptr = &database;
	}

	if (options.command == heimdall::cli::Command::Format && !options.write && files.size() != 1)
	{
		std::cerr << "format without --write requires exactly one file\n";
		return kExitUsageError;
	}

	std::vector<heimdall::cli::FileResult> results;
	{
		const heimdall::trace::Scope traceRunParallel(heimdall::trace::Category::Cli, "RunParallel");
		heimdall::cli::RunParallel(files, options, database_ptr, results);
	}

	if (options.command == heimdall::cli::Command::Lint ||
		options.command == heimdall::cli::Command::Check)
	{
		heimdall::cli::AnalyzeModuleGraph(results);
	}

	const heimdall::trace::Scope traceReportResults(heimdall::trace::Category::Cli, "ReportResults");
	return heimdall::cli::ReportResults(results, options);
}
