#pragma once

#include <Heimdall/AnalysisContext.hpp>
#include <Heimdall/Completion.hpp>
#include <Heimdall/Navigation.hpp>
#include <Heimdall/SemanticRules.hpp>

namespace heimdall
{
    // Shared frontend-independent entry points. Existing tree-based APIs remain
    // available for batch consumers that do not need workspace infrastructure.
    class AnalysisFeatures
    {
      public:
        static std::vector<CompletionItem> Complete(const AnalysisContext& context,
                                                    std::size_t            offset,
                                                    const ScopeIndex*      external = nullptr);

        static std::optional<CompletionItem> Hover(const AnalysisContext& context,
                                                   std::size_t            offset,
                                                   const ScopeIndex*      external = nullptr);

        static std::vector<NavTarget> Definition(const AnalysisContext& context, std::size_t offset,
                                                 const ScopeIndex* external = nullptr);

        static std::vector<NavTarget> Implementation(const AnalysisContext& context,
                                                     std::size_t            offset,
                                                     const ScopeIndex*      external = nullptr);

        static std::vector<Diagnostic> Diagnostics(const AnalysisContext& context,
                                                   const RuleEngine&      engine,
                                                   bool                   semantic = true,
                                                   const ProjectContext&  project  = {});
    };
} // namespace heimdall
