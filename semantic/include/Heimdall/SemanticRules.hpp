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

        // cpp/modernize-nullptr: an explicit conversion of a null constant to a
        // pointer type (`(T*)0`, `(T*)NULL`, `static_cast<T*>(0)`). The fix is a quick
        // fix only: `nullptr` can change overload resolution and `auto` deduction.
        static std::vector<Diagnostic> AnalyzeNullptr(const SemanticModel &model);

        // cpp/no-zero-as-null: `0`/`0L` where the model knows a pointer is meant:
        // initializing, assigning to or comparing with a variable declared with `*`,
        // and `return 0;` in a function whose written return type is a pointer.
        // Typedef'd pointers and arguments (overload resolution) are left alone.
        static std::vector<Diagnostic> AnalyzeZeroAsNull(const SemanticModel &model);

        // cpp/modernize-auto: a declaration whose type is spelled again, verbatim, by
        // its initializer: `T* p = new T`, `T x = static_cast<T>(y)` and
        // `std::unique_ptr<T> p = std::make_unique<T>()`. Iterator declarations need
        // the type of the container and are not reported. Class members are skipped.
        static std::vector<Diagnostic> AnalyzeAuto(const SemanticModel &model);

        // Every rule above, sorted by offset.
        static std::vector<Diagnostic> Analyze(const SemanticModel &model);
    };

} // namespace heimdall
