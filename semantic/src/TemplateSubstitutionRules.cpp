#include <algorithm>
#include <string>
#include <vector>

#include <Heimdall/LineTable.hpp>
#include <Heimdall/SemanticRules.hpp>

namespace heimdall
{
	std::vector<Diagnostic> SemanticRules::AnalyzeTemplateSubstitution(const TypeModel& types)
	{
		const auto& tree  = types.Model().Tree();
		const auto& nodes = tree.NodesSoA();
		LineTable lines;
		lines.Build(tree.Source());
		std::vector<Diagnostic> diagnostics;
		for (const auto& call : types.TemplateCalls())
		{
			const auto state = call.result.state;
			if (state != SubstitutionState::HardError && state != SubstitutionState::NoMatch &&
				state != SubstitutionState::Ambiguous)
			{
				continue;
			}

			if (call.node >= nodes.size() || nodes.FirstToken(call.node) >= tree.Tokens().size())
			{
				continue;
			}

			const auto& token        = tree.Tokens()[nodes.FirstToken(call.node)];
			const auto[line, column] = lines.Lookup(token.offset);
			Diagnostic diagnostic {};
			diagnostic.rule     = RuleId::TemplateSubstitution;
			diagnostic.code     = "cpp/template-substitution";
			diagnostic.severity = Severity::Error;
			diagnostic.offset   = token.offset;
			diagnostic.length   = token.length;
			diagnostic.line     = line;
			diagnostic.column   = column;
			diagnostic.message = state == SubstitutionState::HardError
			? "template instantiation is invalid outside the immediate substitution context"
			: state == SubstitutionState::NoMatch
			? "no viable local overload remains after template deduction and substitution"
			: "local overload resolution is ambiguous after template substitution";
			diagnostics.push_back(std::move(diagnostic));
		}

		std::ranges::stable_sort(diagnostics, {}, &Diagnostic::offset);
		return diagnostics;
	}
}
