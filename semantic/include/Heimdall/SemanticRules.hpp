#pragma once

#include <Heimdall/RuleEngine.hpp>
#include <Heimdall/SemanticModel.hpp>

#include <vector>

namespace heimdall
{

    // Rules that run on a bound SemanticModel (see
    // docs/semantic-engine-architecture.md). A rule only reports what the model
    // knows for certain: any unresolved name, base or signature keeps it silent.
    class SemanticRules
    {
    public:
        // cpp/modernize-override: a member function that overrides a virtual
        // function of a base class declared in this translation unit but does not
        // say `override` (or `final`). Destructors are not reported yet.
        static std::vector<Diagnostic> AnalyzeOverride(const SemanticModel &model);
    };

} // namespace heimdall
