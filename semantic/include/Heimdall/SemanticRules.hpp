#pragma once

#include <Heimdall/CompileDatabase.hpp>
#include <Heimdall/FlowModel.hpp>
#include <Heimdall/IncludeAnalyzer.hpp>
#include <Heimdall/RuleEngine.hpp>
#include <Heimdall/SemanticModel.hpp>
#include <Heimdall/TypeModel.hpp>

#include <filesystem>
#include <vector>

namespace heimdall
{

    // What the project-level rules (F3) know beyond the one bound file: where it
    // lives and, when a compile command was available, the headers it includes.
    // Without a profile the rules still run but see no header: a name or base
    // from a header stays unknown and the rule stays silent about it.
    struct ProjectContext
    {
        std::filesystem::path file;
        const IncludeProfile* profile = nullptr;
        const CompileCommand* command = nullptr;
    };

    // Rules that run on a bound SemanticModel (see
    // docs/semantic-engine-architecture.md). A rule only reports what the model
    // knows for certain: any unresolved name, base or signature keeps it silent.
    class SemanticRules
    {
    public:
        // cpp/modernize-override: a member function that overrides a virtual
        // function of a base class declared in this translation unit but does not
        // say `override` (or `final`). Destructors are not reported yet.
        static std::vector<Diagnostic> AnalyzeOverride(const SemanticModel& model);

        // cpp/modernize-nullptr: an explicit conversion of a null constant to a
        // pointer type (`(T*)0`, `(T*)NULL`, `static_cast<T*>(0)`). The fix is a quick
        // fix only: `nullptr` can change overload resolution and `auto` deduction.
        static std::vector<Diagnostic> AnalyzeNullptr(const SemanticModel& model);

        // cpp/no-zero-as-null: `0`/`0L` where the model knows a pointer is meant:
        // initializing, assigning to or comparing with a variable declared with `*`,
        // and `return 0;` in a function whose written return type is a pointer.
        // Typedef'd pointers and arguments (overload resolution) are left alone.
        static std::vector<Diagnostic> AnalyzeZeroAsNull(const SemanticModel& model);

        // cpp/modernize-auto: a declaration whose type is spelled again, verbatim, by
        // its initializer: `T* p = new T`, `T x = static_cast<T>(y)` and
        // `std::unique_ptr<T> p = std::make_unique<T>()`. Iterator declarations need
        // the type of the container and are not reported. Class members are skipped.
        static std::vector<Diagnostic> AnalyzeAuto(const SemanticModel& model);

        // cpp/no-implicit-bool-conversion: an integer, floating-point or pointer
        // expression whose type the Typer knows used where a bool is expected: the
        // condition of `if`/`while`/`for`/`?:`, operands of `!`, `&&` and `||`.
        // Literals (`while (1)`) and `!!x` are intentional and left alone. No fix:
        // the comparison to write depends on the intent.
        static std::vector<Diagnostic> AnalyzeImplicitBool(const TypeModel& types);

        // cpp/modernize-range-loop: `for (int i = 0; i < c.size(); ++i)` over a local
        // array or standard sequence container whose body only reads `c[i]`. The
        // fix (quick fix only) writes the range-based for.
        static std::vector<Diagnostic> AnalyzeRangeLoop(const TypeModel& types);

        // cpp/modernize-loop-convert: `for (auto it = c.begin(); it != c.end(); ++it)`
        // over a standard container whose body only uses `*it` and `it->`. The fix
        // (quick fix only) writes the range-based for.
        static std::vector<Diagnostic> AnalyzeLoopConvert(const TypeModel& types);

        // cpp/modernize-const: a local variable declared with an initializer that
        // nothing ever writes, modifies or lets escape (F4 def-use). Reports values
        // and class objects of known type; references, pointers, parameters, loop
        // variables and anything with a constant initializer (which is
        // cpp/modernize-constexpr's) stay out. A class object is only counted as
        // untouched when every use is a read or a call to a member known to be const,
        // and returning it by name counts as a use that `const` would pessimize.
        // The fix (quick fix only) writes `const` before the type.
        static std::vector<Diagnostic> AnalyzeConst(const FlowModel& flow);

        // cpp/modernize-constexpr: `const T x = c;` whose initializer is a constant
        // expression (replace `const`), a local `T x = c;` that nothing modifies (add
        // `constexpr`), and a function that is internal (static, anonymous namespace),
        // inline or a static member, takes and returns literal types and whose body
        // only does things a constant evaluator accepts (F4 CFG). Everything the
        // engine cannot see (a callee, a global, a macro) keeps the rule silent. The fix
        // is a quick fix only.
        static std::vector<Diagnostic> AnalyzeConstexpr(const FlowModel& flow);

        // cpp/include-what-you-use: a name the file uses whose declaration comes
        // from a header the file only reaches through another include (a project
        // header found through the HeaderSummary of each header in the include
        // closure, or a standard-library name with a well-known header). The
        // quick fix adds the include. Needs context.profile; silent without it,
        // when some include was not found, or when the name is ambiguous.
        static std::vector<Diagnostic> AnalyzeIncludeWhatYouUse(const SemanticModel& model,
            const ProjectContext& context);

        // cpp/modernize-final: a polymorphic class that nothing derives from, and
        // an `override` that nothing overrides, in code whose derived classes are
        // all visible: classes in an anonymous namespace, or anything defined in a
        // source file (no other file can see the definition). The hierarchy is
        // completed with the HeaderSummary of the included project headers: a base
        // defined there decides whether the class is polymorphic. Header classes
        // outside an anonymous namespace are never reported: any other file may
        // derive from them. The fix (quick fix only) adds `final`.
        static std::vector<Diagnostic> AnalyzeFinal(const SemanticModel& model,
            const ProjectContext& context);

        // api/virtual-destructor: a struct/class that declares virtual functions
        // whose destructor is not virtual and is public (a protected one is the
        // accepted way to forbid deleting through the base) and that inherits no
        // virtual destructor. Silent when some base is not resolved and for `final`
        // classes. The quick fix (only when the destructor is declared) writes
        // `virtual`.
        static std::vector<Diagnostic> AnalyzeVirtualDestructor(const SemanticModel& model);

        // api/explicit-constructor: a constructor of a class defined in this file
        // callable with one argument (further parameters have defaults) that is
        // not `explicit`. Copy/move constructors, `std::initializer_list` and
        // variadic ones are left alone. The quick fix writes `explicit`.
        static std::vector<Diagnostic> AnalyzeExplicitConstructor(const SemanticModel& model);

        // api/overload-hiding: a member function that has the name of a virtual
        // function of a resolved base but a signature the derived class does not
        // match, so the base overload is hidden, and no `using Base::name;`. The
        // quick fix adds the using-declaration before the function.
        static std::vector<Diagnostic> AnalyzeOverloadHiding(const SemanticModel& model);

        // api/virtual-call-in-constructor: an unqualified (or `this->`) call to a
        // virtual member function in the body of a constructor or destructor
        // defined inside the class: it does not reach overrides of derived classes.
        // `final` classes and functions, lambdas and qualified calls are skipped.
        static std::vector<Diagnostic> AnalyzeVirtualCallInConstructor(const SemanticModel& model);

        // cpp/designated-init-order: `T{.b = 1, .a = 2}` where the members are declared
        // `a` then `b`. C++20 requires the declaration order. Needs the class to be
        // defined in this file with no bases; silent for anything unresolved. The quick
        // fix (never applied by --fix: it changes evaluation order) permutes the entries.
        static std::vector<Diagnostic> AnalyzeDesignatedInitOrder(const SemanticModel& model);

        // cpp/no-zero-as-null inside designated initializers: `T{.ptr = 0}` where the
        // member is declared with `*`. AnalyzeZeroAsNull includes these.
        static std::vector<Diagnostic> AnalyzeDesignatedZeroAsNull(const SemanticModel& model);

        // The `T{.ptr = 20}` part of AnalyzeIntegerToPointer.
        static std::vector<Diagnostic> AnalyzeDesignatedIntegerToPointer(const SemanticModel& model);

        // cpp/no-integer-to-pointer: a non-zero integer literal used as a pointer
        // (`T* p = 20;`, `p = 20;`, `T{.ptr = 20}`), which does not compile: only a null
        // constant converts implicitly. Error by default; no fix, since the address
        // (`reinterpret_cast<T*>(20)`) or the intended member is for the author to say.
        static std::vector<Diagnostic> AnalyzeIntegerToPointer(const SemanticModel& model);

        // doc/require-comment (opt-in): a class, enum or function inside `scope` (see
        // DocScope) with no Doxygen comment: `///`, `//!`, `/** */` or `/*! */` right
        // before it, or `///<` after a declaration that ends in `;`. Declarations local
        // to a function, out-of-class definitions, `= default`/`= delete`, overrides,
        // friends, forward declarations, `main` and explicit specializations are left
        // alone: their documentation lives elsewhere. No fix: the text is the author's.
        static std::vector<Diagnostic> AnalyzeRequireDocComment(const SemanticModel& model,
            DocScope scope = DocScope::Public);

        // doc/doxygen-style (opt-in): a function, class or enum inside `scope` whose
        // Doxygen comment breaks good practice:
        //  - no `@brief` (or, without it, a first paragraph), a brief of more than one
        //    sentence or line, or a brief not separated from the details by a blank line;
        //  - a function with an `@param` missing for a named parameter, an `@tparam`
        //    missing for a named template parameter, no `@return` (`@returns`, `@result`
        //    or `@retval`) when it returns a value, or no `@throws` (`@throw`,
        //    `@exception`) when its body has a `throw`;
        //  - an `@param`/`@tparam` that names nothing in the signature or is repeated;
        //  - a tag with no description;
        //  - the Qt styles `//!` and `/*! */` (Javadoc `///` and `/** */` is the
        //    convention), or `@` and `\` command spellings mixed in one comment.
        // Comments with `@copydoc`, `@inheritdoc` or `@overload` are skipped. No fix.
        static std::vector<Diagnostic> AnalyzeDoxygenStyle(const SemanticModel& model,
            DocScope scope = DocScope::Public);

        // The two rules above, for those the engine has enabled, with the engine's
        // DocScope. They are not part of Analyze: every undocumented declaration would
        // trip them, so the CLI and the language server add them only when a config
        // file or --rule asks for them.
        static std::vector<Diagnostic> AnalyzeDocumentation(const SemanticModel& model,
            const RuleEngine& engine);

        // Every rule above except the project-level ones, sorted by offset. The
        // overload without a TypeModel runs the Typer itself.
        static std::vector<Diagnostic> Analyze(const SemanticModel& model);

        static std::vector<Diagnostic> Analyze(const SemanticModel& model, const TypeModel& types);

        // The same plus the project-level rules, which need the file's context.
        static std::vector<Diagnostic> Analyze(const SemanticModel& model, const TypeModel& types,
            const ProjectContext& context);
    };

} // namespace heimdall
