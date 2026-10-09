#include <Heimdall/AnalysisFeatures.hpp>

#include "SemanticRuleDispatch.hpp"

#include <algorithm>
#include <iterator>

namespace heimdall
{
    std::vector<CompletionItem> AnalysisFeatures::Complete(
        const AnalysisContext& context, std::size_t offset, const ScopeIndex* external)
    {
        if (!context.Valid())
            return {};
        return CompletionEngine::Complete(
            context.Syntax(), context.Snapshot().Options(context.Document()), offset, external);
    }

    std::optional<CompletionItem> AnalysisFeatures::Hover(
        const AnalysisContext& context, std::size_t offset, const ScopeIndex* external)
    {
        if (!context.Valid())
            return {};
        return CompletionEngine::Hover(
            context.Syntax(), context.Snapshot().Options(context.Document()), offset, external);
    }

    std::vector<NavTarget> AnalysisFeatures::Definition(
        const AnalysisContext& context, std::size_t offset, const ScopeIndex* external)
    {
        return context.Valid() ? Navigation::Definition(context.Syntax(), offset, external)
                               : std::vector<NavTarget> {};
    }

    std::vector<NavTarget> AnalysisFeatures::Implementation(
        const AnalysisContext& context, std::size_t offset, const ScopeIndex* external)
    {
        return context.Valid() ? Navigation::Implementation(context.Syntax(), offset, external)
                               : std::vector<NavTarget> {};
    }

    std::vector<Diagnostic> AnalysisFeatures::Diagnostics(const AnalysisContext& context,
                                                          const RuleEngine&      engine,
                                                          bool                   semantic,
                                                          const ProjectContext&  project)
    {
        if (!context.Valid())
            return {};
        auto diagnostics = engine.Analyze(context.Syntax());
        if (semantic)
        {
            auto analyzed = detail::AnalyzeSelectedSemantic(context, engine, project);
            analyzed      = engine.ApplyPolicy(std::move(analyzed), context.Syntax());
            diagnostics.insert(diagnostics.end(), std::make_move_iterator(analyzed.begin()),
                               std::make_move_iterator(analyzed.end()));
        }

        std::stable_sort(
            diagnostics.begin(), diagnostics.end(),
            [](const Diagnostic& a, const Diagnostic& b) { return a.offset < b.offset; });
        return diagnostics;
    }
} // namespace heimdall
