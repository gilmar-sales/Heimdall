#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <Heimdall/CompileDatabase.hpp>
#include <Heimdall/ModuleUnit.hpp>
#include <Heimdall/ParseTree.hpp>
#include <Heimdall/RuleEngine.hpp>
#include <Heimdall/SemanticAnalyzer.hpp>

#include "CliOptions.hpp"

namespace heimdall::cli
{

	struct SyntaxDiagnostic
	{
		std::uint32_t line   = 1;
		std::uint32_t column = 1;
		std::string code     = "syntax/parse-error";
		std::string message;
	};

	struct ParseNodeSummary
	{
		std::string kind;
		std::size_t parent = 0;
		std::size_t offset = 0;
		std::size_t length = 0;
	};

	struct FileResult
	{
		std::filesystem::path path;
		std::string error;
		std::vector<heimdall::Diagnostic> diagnostics;
		std::vector<heimdall::SemanticDiagnostic> semantic_diagnostics;
		std::vector<SyntaxDiagnostic> syntax_diagnostics;
		std::vector<ParseNodeSummary> nodes;
		/// Module facts of the file and the rule options it was analyzed with, kept for the
		/// cross-file module pass.
		heimdall::ModuleUnitInfo module;
		heimdall::RuleOptions rule_options;
		std::string standard_name = "c++20";
		std::string output;
		bool changed                  = false;
		bool has_semantic_context     = false;
		std::size_t type_count        = 0;
		std::size_t declaration_count = 0;
		std::size_t ambiguous_count   = 0;
	};

	std::string_view GrammarKindName(heimdall::GrammarKind kind);

	std::string_view StandardName(heimdall::CppStandard standard);

	heimdall::ParserOptions ParserOptionsForFile(
		const std::filesystem::path& path,
		const Options& options,
		const heimdall::CompileDatabase* database,
		std::string_view source);

	std::vector<SyntaxDiagnostic> ToSyntaxDiagnostics(
		std::string_view source,
		const std::vector<heimdall::GrammarDiagnostic>& grammar);

	void ProcessFile(
		const std::filesystem::path& path,
		const Options& options,
		const heimdall::CompileDatabase* database,
		FileResult& result);

	/// Adds the diagnostics that span files (import cycles, duplicate interfaces, partitions
	/// the primary interface does not export, and optionally unresolved imports) to the
	/// results of the files they are reported in, honoring each file's own rule options.
	void AnalyzeModuleGraph(std::vector<FileResult>& results);

	void RunParallel(
		const std::vector<std::filesystem::path>& files,
		const Options& options,
		const heimdall::CompileDatabase* database,
		std::vector<FileResult>& results);

} // namespace heimdall::cli
