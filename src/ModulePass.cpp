#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Heimdall/LineTable.hpp>
#include <Heimdall/ModuleGraph.hpp>
#include <Heimdall/ParseTree.hpp>
#include <Heimdall/RuleEngine.hpp>

#include "Pipeline.hpp"

namespace heimdall::cli
{

	namespace
	{

		constexpr std::string_view kUnresolvedCode = "cpp/module-unresolved-import";

		constexpr std::string_view CodeFor(ModuleIssueKind kind) noexcept
		{
			switch (kind)
			{
			case ModuleIssueKind::UnresolvedImport:
				return kUnresolvedCode;
			case ModuleIssueKind::ImportCycle:
				return "cpp/module-import-cycle";
			case ModuleIssueKind::DuplicateInterface:
				return "cpp/module-duplicate-interface";
			case ModuleIssueKind::PartitionNotExported:
				return "cpp/module-partition-export";
			}

			return {};
		}

		bool IsEnabled(const RuleEngine& engine, std::string_view code)
		{
			return code == kUnresolvedCode ? engine.OptInEnabled(code) : engine.Enabled(code);
		}

		std::string ReadFile(const std::filesystem::path& path)
		{
			std::ifstream in(path, std::ios::binary);
			return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
		}

		bool ParticipatesInModules(const FileResult& result)
		{
			return result.error.empty() &&
				(result.module.IsModuleUnit() ||!result.module.imports.empty());
		}

		Diagnostic ToDiagnostic(
			const ModuleIssue& issue,
			std::string_view code,
			const LineTable& lines)
		{
			const auto position = lines.Lookup(issue.offset);
			const auto* info    = FindRuleByCode(code);
			Diagnostic diagnostic {};
			diagnostic.rule     = info->id;
			diagnostic.severity = info->default_severity;
			diagnostic.code     = std::string(code);
			diagnostic.message  = issue.message;
			diagnostic.offset   = issue.offset;
			diagnostic.length   = issue.length;
			diagnostic.line     = position.line;
			diagnostic.column   = position.column;
			diagnostic.has_fix  = false;
			return diagnostic;
		}

	} // namespace

	void AnalyzeModuleGraph(std::vector<FileResult>& results)
	{
		ModuleGraph graph;
		for (const auto& result : results)
		{
			if (ParticipatesInModules(result))
			{
				graph.Set(result.path.generic_string(), result.module);
			}
		}

		if (graph.Size() == 0)
		{
			return;
		}

		ModuleCheckOptions check;
		check.reportUnresolved = true;
		const auto issues      = graph.Check(check);
		for (auto& result : results)
		{
			const auto key = result.path.generic_string();
			const RuleEngine engine(result.rule_options);
			std::vector<Diagnostic> found;
			std::string source;
			LineTable lines;
			for (const auto& issue : issues)
			{
				const auto code = CodeFor(issue.kind);
				if (issue.file != key ||!IsEnabled(engine, code))
				{
					continue;
				}

				if (source.empty())
				{
					source = ReadFile(result.path);
					lines.Build(source);
				}

				found.push_back(ToDiagnostic(issue, code, lines));
			}

			if (found.empty())
			{
				continue;
			}

			const auto tree = ParseTree::Parse(source);
			for (auto& diagnostic : engine.ApplyPolicy(std::move(found), tree))
			{
				result.diagnostics.push_back(std::move(diagnostic));
			}

			std::stable_sort(result.diagnostics.begin(), result.diagnostics.end(),
				[](const Diagnostic& a, const Diagnostic& b)
				{
					return a.offset < b.offset;
				});
		}
	}

} // namespace heimdall::cli
