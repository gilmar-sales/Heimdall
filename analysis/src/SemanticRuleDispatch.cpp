#include "SemanticRuleDispatch.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <optional>
#include <variant>

namespace heimdall::detail
{
    namespace
    {
        using BoundRule = std::vector<Diagnostic> (*)(const SemanticModel &);
        using TypedRule = std::vector<Diagnostic> (*)(const TypeModel &);
        using FlowRule = std::vector<Diagnostic> (*)(const FlowModel &);
        using ProjectRule = std::vector<Diagnostic> (*)(const SemanticModel &, const ProjectContext &);
        using DocumentationRule = std::vector<Diagnostic> (*)(const SemanticModel &, DocScope);
        using Callback = std::variant<BoundRule, TypedRule, FlowRule, ProjectRule, DocumentationRule>;

        struct Entry
        {
            RuleId id;
            Callback callback;
        };

        // Callback domains declare actual model requirements, without duplicating
        // catalog codes/severities or sending native rules through PluginContext.
        // Order matches the legacy aggregators, including ties at the same offset.
        constexpr std::array entries = {
            Entry{RuleId::ModernizeOverride, &SemanticRules::AnalyzeOverride},
            Entry{RuleId::ModernizeNullptr, &SemanticRules::AnalyzeNullptr},
            Entry{RuleId::NoZeroAsNull, &SemanticRules::AnalyzeZeroAsNull},
            Entry{RuleId::ModernizeAuto, &SemanticRules::AnalyzeAuto},
            Entry{RuleId::NoImplicitBoolConversion, &SemanticRules::AnalyzeImplicitBool},
            Entry{RuleId::ModernizeRangeLoop, &SemanticRules::AnalyzeRangeLoop},
            Entry{RuleId::ModernizeLoopConvert, &SemanticRules::AnalyzeLoopConvert},
            Entry{RuleId::ModernizeConst, &SemanticRules::AnalyzeConst},
            Entry{RuleId::ModernizeConstexpr, &SemanticRules::AnalyzeConstexpr},
            Entry{RuleId::ApiVirtualDestructor, &SemanticRules::AnalyzeVirtualDestructor},
            Entry{RuleId::ApiExplicitConstructor, &SemanticRules::AnalyzeExplicitConstructor},
            Entry{RuleId::ApiOverloadHiding, &SemanticRules::AnalyzeOverloadHiding},
            Entry{RuleId::ApiVirtualCallInConstructor, &SemanticRules::AnalyzeVirtualCallInConstructor},
            Entry{RuleId::DesignatedInitOrder, &SemanticRules::AnalyzeDesignatedInitOrder},
            Entry{RuleId::NoIntegerToPointer, &SemanticRules::AnalyzeIntegerToPointer},
            Entry{RuleId::ModernizeFinal, &SemanticRules::AnalyzeFinal},
            Entry{RuleId::IncludeWhatYouUse, &SemanticRules::AnalyzeIncludeWhatYouUse},
            Entry{RuleId::DocRequireComment, &SemanticRules::AnalyzeRequireDocComment},
            Entry{RuleId::DocDoxygenStyle, &SemanticRules::AnalyzeDoxygenStyle}
        };

        void Append(std::vector<Diagnostic> &all, std::vector<Diagnostic> part)
        {
            all.insert(all.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
        }

        bool ByOffset(const Diagnostic &a, const Diagnostic &b) { return a.offset < b.offset; }
    }

    std::vector<Diagnostic> AnalyzeSelectedSemantic(const AnalysisContext &context,
        const RuleEngine &engine, const ProjectContext &project)
    {
        std::array<bool, entries.size()> selected{};
        bool needs_binding = false;
        bool needs_types = false;
        bool needs_flow = false;
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            const auto &entry = entries[i];
            const auto *info = FindRule(entry.id);
            const bool documentation = std::holds_alternative<DocumentationRule>(entry.callback);
            selected[i] = info && engine.Enabled(info->code, !documentation);
            if (!selected[i]) continue;
            needs_binding = true;
            needs_flow |= std::holds_alternative<FlowRule>(entry.callback);
            needs_types |= std::holds_alternative<TypedRule>(entry.callback);
        }
        if (!needs_binding) return {};

        const auto &model = context.Semantic();
        const auto *types = needs_types || needs_flow ? &context.Types() : nullptr;
        std::optional<FlowModel> flow;
        if (needs_flow) flow.emplace(Flow::Build(*types));

        std::vector<Diagnostic> local, project_diagnostics, documentation;
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            if (!selected[i]) continue;
            const auto &callback = entries[i].callback;
            if (const auto *bound = std::get_if<BoundRule>(&callback)) Append(local, (*bound)(model));
            else if (const auto *typed = std::get_if<TypedRule>(&callback)) Append(local, (*typed)(*types));
            else if (const auto *flow_rule = std::get_if<FlowRule>(&callback)) Append(local, (*flow_rule)(*flow));
            else if (const auto *project_rule = std::get_if<ProjectRule>(&callback))
                Append(project_diagnostics, (*project_rule)(model, project));
            else if (const auto *doc = std::get_if<DocumentationRule>(&callback))
                Append(documentation, (*doc)(model, engine.DocumentationScope()));
        }
        // Keep the same staging as Analyze(model, types, project) followed by
        // AnalyzeDocumentation. Policy remains the caller's responsibility.
        std::stable_sort(local.begin(), local.end(), ByOffset);
        Append(local, std::move(project_diagnostics));
        std::stable_sort(local.begin(), local.end(), ByOffset);
        std::stable_sort(documentation.begin(), documentation.end(), ByOffset);
        Append(local, std::move(documentation));
        return local;
    }
}
