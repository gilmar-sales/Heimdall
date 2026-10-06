#pragma once

#include <Heimdall/AnalysisFeatures.hpp>

namespace heimdall::detail
{
    // Internal native adapter; algorithms remain in semantic, never depend on analysis.
    std::vector<Diagnostic> AnalyzeSelectedSemantic(const AnalysisContext& context,
        const RuleEngine& engine, const ProjectContext& project);
}
