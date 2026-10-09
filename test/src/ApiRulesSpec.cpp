#include <Heimdall/RuleEngine.hpp>
#include <Heimdall/SemanticRules.hpp>

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace
{

    using Analyzer = std::vector<heimdall::Diagnostic> (*)(const heimdall::SemanticModel&);

    std::vector<heimdall::Diagnostic> RunRule(Analyzer analyzer, const std::string& source)
    {
        const auto tree  = heimdall::ParseTree::Parse(source);
        const auto model = heimdall::Binder::Bind(tree);
        return analyzer(model);
    }

    std::vector<heimdall::Diagnostic> VirtualDestructor(const std::string& source)
    {
        return RunRule(heimdall::SemanticRules::AnalyzeVirtualDestructor, source);
    }

    std::vector<heimdall::Diagnostic> ExplicitConstructor(const std::string& source)
    {
        return RunRule(heimdall::SemanticRules::AnalyzeExplicitConstructor, source);
    }

    std::vector<heimdall::Diagnostic> OverloadHiding(const std::string& source)
    {
        return RunRule(heimdall::SemanticRules::AnalyzeOverloadHiding, source);
    }

    std::vector<heimdall::Diagnostic> VirtualCall(const std::string& source)
    {
        return RunRule(heimdall::SemanticRules::AnalyzeVirtualCallInConstructor, source);
    }

    // TypeModel- and FlowModel-backed rules need their layers built first; the
    // fixture is never moved because the layers reference each other.
    struct Typed
    {
        explicit Typed(std::string text) :
            source(std::move(text)), tree(heimdall::ParseTree::Parse(source)),
            model(heimdall::Binder::Bind(tree)), types(heimdall::Typer::Type(model)),
            flow(heimdall::Flow::Build(types))
        {
        }

        Typed(const Typed&) = delete;

        Typed& operator=(const Typed&) = delete;

        std::string             source;
        heimdall::ParseTree     tree;
        heimdall::SemanticModel model;
        heimdall::TypeModel     types;
        heimdall::FlowModel     flow;
    };

    std::string Fixed(std::string source, const heimdall::Diagnostic& diagnostic)
    {
        source.replace(diagnostic.fix.offset, diagnostic.fix.length, diagnostic.fix.replacement);
        return source;
    }

} // namespace

// ---- api/virtual-destructor ------------------------------------------------

TEST(ApiVirtualDestructor, ReportsAClassWithVirtualFunctionsAndNoDestructor)
{
    const std::string source      = "struct Shape { virtual void draw(); };\n";
    const auto        diagnostics = VirtualDestructor(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "api/virtual-destructor");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::ApiVirtualDestructor);
    EXPECT_EQ(source.substr(diagnostics[0].offset, diagnostics[0].length), "Shape");
    EXPECT_FALSE(diagnostics[0].has_fix);
}

TEST(ApiVirtualDestructor, QuickFixMakesADeclaredDestructorVirtual)
{
    const std::string source      = "class A { public: virtual void f(); ~A(); };\n";
    const auto        diagnostics = VirtualDestructor(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    ASSERT_TRUE(diagnostics[0].has_fix);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(Fixed(source, diagnostics[0]),
              "class A { public: virtual void f(); virtual ~A(); };\n");
}

TEST(ApiVirtualDestructor, ReportsPureVirtualInterfaces)
{
    EXPECT_EQ(VirtualDestructor("struct I { virtual void f() = 0; };\n").size(), 1u);
}

TEST(ApiVirtualDestructor, SilentWhenTheDestructorIsVirtual)
{
    EXPECT_TRUE(VirtualDestructor("struct A { virtual void f(); virtual ~A(); };\n").empty());
    EXPECT_TRUE(
        VirtualDestructor("struct A { virtual void f(); virtual ~A() = default; };\n").empty());
}

TEST(ApiVirtualDestructor, SilentWhenANonPublicDestructorForbidsDeletingThroughTheBase)
{
    EXPECT_TRUE(VirtualDestructor("struct A { virtual void f(); protected: ~A(); };\n").empty());
    EXPECT_TRUE(VirtualDestructor("class A { virtual void f(); ~A(); };\n").empty());
}

TEST(ApiVirtualDestructor, SilentWhenABaseHasAVirtualDestructor)
{
    EXPECT_TRUE(VirtualDestructor("struct Base { virtual ~Base(); };\n"
                                  "struct Derived : Base { virtual void g(); };\n")
                    .empty());
}

TEST(ApiVirtualDestructor, ReportsWhenNoBaseHasAVirtualDestructor)
{
    const auto diagnostics = VirtualDestructor("struct Base { void f(); };\n"
                                               "struct Derived : Base { virtual void g(); };\n");
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].line, 2u);
}

TEST(ApiVirtualDestructor, SilentWhenABaseIsUnknown)
{
    EXPECT_TRUE(VirtualDestructor("struct D : Missing { virtual void g(); };\n").empty());
    EXPECT_TRUE(VirtualDestructor("struct D : Base<int> { virtual void g(); };\n").empty());
}

TEST(ApiVirtualDestructor, SilentForFinalAndNonPolymorphicClasses)
{
    EXPECT_TRUE(VirtualDestructor("struct A final { virtual void f(); };\n").empty());
    EXPECT_TRUE(VirtualDestructor("struct A { void f(); ~A(); };\n").empty());
    EXPECT_TRUE(VirtualDestructor("struct A { void f() override; };\n").empty());
}

TEST(ApiVirtualDestructor, SurvivesBrokenInput)
{
    EXPECT_NO_THROW(VirtualDestructor("struct A { virtual void f("));
    EXPECT_NO_THROW(VirtualDestructor("struct A : A { virtual void f(); };\n"));
}

// ---- api/explicit-constructor ----------------------------------------------

TEST(ApiExplicitConstructor, ReportsAOneArgumentConstructor)
{
    const std::string source      = "struct A { A(int x); };\n";
    const auto        diagnostics = ExplicitConstructor(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "api/explicit-constructor");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::ApiExplicitConstructor);
    EXPECT_EQ(source.substr(diagnostics[0].offset, diagnostics[0].length), "A");
    ASSERT_TRUE(diagnostics[0].has_fix);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(Fixed(source, diagnostics[0]), "struct A { explicit A(int x); };\n");
}

TEST(ApiExplicitConstructor, InsertsBeforeConstexprAndAfterTheTemplateHeader)
{
    const std::string source      = "struct A { constexpr A(int x) {} };\n";
    const auto        diagnostics = ExplicitConstructor(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(Fixed(source, diagnostics[0]), "struct A { explicit constexpr A(int x) {} };\n");

    const std::string templated             = "struct B { template<class T> B(T t); };\n";
    const auto        templated_diagnostics = ExplicitConstructor(templated);
    ASSERT_EQ(templated_diagnostics.size(), 1u);
    EXPECT_EQ(Fixed(templated, templated_diagnostics[0]),
              "struct B { template<class T> explicit B(T t); };\n");
}

TEST(ApiExplicitConstructor, ReportsWhenTheOtherParametersHaveDefaults)
{
    EXPECT_EQ(ExplicitConstructor("struct A { A(int x, int y = 2); };\n").size(), 1u);
    EXPECT_EQ(ExplicitConstructor("struct A { A(int x = 0); };\n").size(), 1u);
}

TEST(ApiExplicitConstructor, SilentWhenAlreadyExplicit)
{
    EXPECT_TRUE(ExplicitConstructor("struct A { explicit A(int x); };\n").empty());
    EXPECT_TRUE(
        ExplicitConstructor("struct A { public: explicit constexpr A(int x); };\n").empty());
}

TEST(ApiExplicitConstructor, SilentWhenItCannotBeCalledWithOneArgument)
{
    EXPECT_TRUE(ExplicitConstructor("struct A { A(); };\n").empty());
    EXPECT_TRUE(ExplicitConstructor("struct A { A(void); };\n").empty());
    EXPECT_TRUE(ExplicitConstructor("struct A { A(int x, int y); };\n").empty());
}

TEST(ApiExplicitConstructor, SilentForCopyMoveListAndVariadicConstructors)
{
    EXPECT_TRUE(ExplicitConstructor("struct A { A(const A& other); A(A&& other); };\n").empty());
    EXPECT_TRUE(ExplicitConstructor("struct A { A(std::initializer_list<int> l); };\n").empty());
    EXPECT_TRUE(ExplicitConstructor("struct A { template<class... T> A(T... t); };\n").empty());
    EXPECT_TRUE(ExplicitConstructor("struct A { A(const A&) = default; };\n").empty());
}

TEST(ApiExplicitConstructor, OnlyConstructorsOfTheClassAreReported)
{
    EXPECT_TRUE(
        ExplicitConstructor("struct A { void f(int x); static A make(int x); };\n").empty());
    EXPECT_TRUE(ExplicitConstructor("struct A { A(int x) = delete; };\n").empty());
}

// ---- api/overload-hiding ---------------------------------------------------

TEST(ApiOverloadHiding, ReportsADerivedOverloadThatHidesAVirtualBaseFunction)
{
    const std::string source      = "struct Base { virtual void f(int x); };\n"
                                    "struct Derived : Base { void f(double d); };\n";
    const auto        diagnostics = OverloadHiding(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "api/overload-hiding");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::ApiOverloadHiding);
    EXPECT_EQ(diagnostics[0].line, 2u);
    EXPECT_NE(diagnostics[0].message.find("Base"), std::string::npos);
    ASSERT_TRUE(diagnostics[0].has_fix);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(Fixed(source, diagnostics[0]),
              "struct Base { virtual void f(int x); };\n"
              "struct Derived : Base { using Base::f; void f(double d); };\n");
}

TEST(ApiOverloadHiding, ReportsOncePerNameAndFollowsIndirectBases)
{
    const auto diagnostics = OverloadHiding(
        "struct A { virtual void f(int x); };\n"
        "struct B : A {};\n"
        "struct C : B { void f(); void f(double d); };\n");
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].line, 3u);
}

TEST(ApiOverloadHiding, SilentWhenTheDerivedClassOverridesEveryBaseOverload)
{
    EXPECT_TRUE(OverloadHiding("struct Base { virtual void f(int x); };\n"
                               "struct Derived : Base { void f(int y) override; };\n")
                    .empty());
}

TEST(ApiOverloadHiding, SilentWithAUsingDeclaration)
{
    EXPECT_TRUE(OverloadHiding("struct Base { virtual void f(int x); };\n"
                               "struct Derived : Base { using Base::f; void f(double d); };\n")
                    .empty());
}

TEST(ApiOverloadHiding, SilentWhenTheBaseFunctionIsNotVirtual)
{
    EXPECT_TRUE(OverloadHiding("struct Base { void f(int x); };\n"
                               "struct Derived : Base { void f(double d); };\n")
                    .empty());
}

TEST(ApiOverloadHiding, SilentWhenBasesDoNotResolveOrNamesDiffer)
{
    EXPECT_TRUE(OverloadHiding("struct D : Missing { void f(double d); };\n").empty());
    EXPECT_TRUE(OverloadHiding("struct Base { virtual void f(int x); };\n"
                               "struct Derived : Base { void g(double d); };\n")
                    .empty());
}

TEST(ApiOverloadHiding, ConstQualifiedOverloadsAreDifferentSignatures)
{
    EXPECT_EQ(OverloadHiding("struct Base { virtual void f(); };\n"
                             "struct Derived : Base { void f() const; };\n")
                  .size(),
              1u);
}

TEST(ApiOverloadHiding, SurvivesCyclesAndBrokenInput)
{
    EXPECT_NO_THROW(OverloadHiding("struct A : B { void f(); }; struct B : A { void f(); };\n"));
    EXPECT_NO_THROW(OverloadHiding("struct A : B { void f("));
}

// ---- api/virtual-call-in-constructor ---------------------------------------

TEST(ApiVirtualCallInConstructor, ReportsAVirtualCallInAConstructor)
{
    const std::string source =
        "struct A {\n"
        "    virtual void init();\n"
        "    A() { init(); }\n"
        "};\n";
    const auto diagnostics = VirtualCall(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "api/virtual-call-in-constructor");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::ApiVirtualCallInConstructor);
    EXPECT_EQ(diagnostics[0].line, 3u);
    EXPECT_EQ(source.substr(diagnostics[0].offset, diagnostics[0].length), "init");
    EXPECT_FALSE(diagnostics[0].has_fix);
    EXPECT_NE(diagnostics[0].message.find("constructor"), std::string::npos);
}

TEST(ApiVirtualCallInConstructor, ReportsCallsInADestructorAndThroughThis)
{
    const auto diagnostics = VirtualCall(
        "struct A {\n"
        "    virtual void stop();\n"
        "    virtual ~A() { this->stop(); }\n"
        "};\n");
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_NE(diagnostics[0].message.find("destructor"), std::string::npos);
}

TEST(ApiVirtualCallInConstructor, ReportsAVirtualFunctionInheritedFromABase)
{
    const auto diagnostics = VirtualCall("struct Base { virtual void f(); };\n"
                                         "struct D : Base { D() { f(); } };\n");
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].line, 2u);
}

TEST(ApiVirtualCallInConstructor, SilentForNonVirtualAndQualifiedCalls)
{
    EXPECT_TRUE(VirtualCall("struct A { void init(); A() { init(); } };\n").empty());
    EXPECT_TRUE(VirtualCall("struct A { virtual void init(); A() { A::init(); } };\n").empty());
    EXPECT_TRUE(VirtualCall("struct A { virtual void init(); A(A* o) { o->init(); } };\n").empty());
    EXPECT_TRUE(VirtualCall("struct A { virtual void init(); A(A& o) { o.init(); } };\n").empty());
}

TEST(ApiVirtualCallInConstructor, SilentForFinalClassesFinalFunctionsAndLambdas)
{
    EXPECT_TRUE(VirtualCall("struct A final { virtual void init(); A() { init(); } };\n").empty());
    EXPECT_TRUE(VirtualCall("struct A { virtual void init() final; A() { init(); } };\n").empty());
    EXPECT_TRUE(
        VirtualCall("struct A { virtual void init(); A() { auto f = [this] { init(); }; } };\n")
            .empty());
}

TEST(ApiVirtualCallInConstructor, SilentForOrdinaryFunctionsAndUnknownCallees)
{
    EXPECT_TRUE(VirtualCall("struct A { virtual void init(); void run() { init(); } };\n").empty());
    EXPECT_TRUE(VirtualCall("struct A { A() { helper(); } };\n").empty());
}

TEST(ApiVirtualCallInConstructor, SurvivesBrokenInput)
{
    EXPECT_NO_THROW(VirtualCall("struct A { virtual void f(); A() { f("));
}

TEST(ApiRulesCatalog, RulesAreRegisteredAndRunWithTheSemanticAnalysis)
{
    for (const auto code :
         { "api/virtual-destructor", "api/explicit-constructor", "api/overload-hiding",
           "api/virtual-call-in-constructor", "api/missing-nodiscard", "api/pass-by-value",
           "api/pass-by-const-reference", "api/const-correctness", "api/unsafe-downcast",
           "api/slicing", "api/implicit-conversion" })
    {
        EXPECT_TRUE(heimdall::IsKnownRuleCode(code)) << code;
    }

    const std::string source = "struct A { A(int x); virtual void f(); };\n";
    const auto        tree   = heimdall::ParseTree::Parse(source);
    const auto        model  = heimdall::Binder::Bind(tree);
    const auto        all    = heimdall::SemanticRules::Analyze(model);
    ASSERT_EQ(all.size(), 2u);
}

// ---- api/missing-nodiscard --------------------------------------------------

TEST(ApiMissingNodiscard, ReportsAResourceLikeResultWithoutTheAttribute)
{
    const Typed typed("struct Widget { int x; };\nWidget* build();\n");
    const auto  diagnostics = heimdall::SemanticRules::AnalyzeMissingNodiscard(typed.types);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "api/missing-nodiscard");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::ApiMissingNodiscard);
    ASSERT_TRUE(diagnostics[0].has_fix);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(Fixed(typed.source, diagnostics[0]),
              "struct Widget { int x; };\n[[nodiscard]] Widget* build();\n");
}

TEST(ApiMissingNodiscard, ReportsClassReturnsWithPlainNames)
{
    const Typed typed("struct W { int x; };\nW load();\n");
    EXPECT_EQ(heimdall::SemanticRules::AnalyzeMissingNodiscard(typed.types).size(), 1u);
}

TEST(ApiMissingNodiscard, SilentForVoidArithmeticQueryNamesAndAnnotatedFunctions)
{
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeMissingNodiscard(
                    Typed("void run();\nint compute();\n").types)
                    .empty());
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeMissingNodiscard(
                    Typed("struct W {};\nW get_widget();\n").types)
                    .empty());
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeMissingNodiscard(
                    Typed("struct W {};\n[[nodiscard]] W load();\n").types)
                    .empty());
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeMissingNodiscard(
                    Typed("struct A { A(int x); };\nint main();\n").types)
                    .empty());
}

// ---- api/pass-by-value ------------------------------------------------------

TEST(ApiPassByValue, ReportsAConstReferenceThatIsCopiedIntoAMember)
{
    const Typed typed("struct Holder {\n"
                      "    std::string name_;\n"
                      "    Holder(const std::string& name) : name_(name) {}\n"
                      "};\n");
    const auto  diagnostics = heimdall::SemanticRules::AnalyzePassByValue(typed.types);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "api/pass-by-value");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::ApiPassByValue);
    ASSERT_TRUE(diagnostics[0].has_fix);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_NE(diagnostics[0].fix.replacement.find("std::string name"), std::string::npos);
}

TEST(ApiPassByValue, SilentWithoutACopyOrWithoutADefinition)
{
    EXPECT_TRUE(
        heimdall::SemanticRules::AnalyzePassByValue(
            Typed("struct F { int x; };\nvoid use(const F& f);\nvoid g(const F& f) { other(f); }\n")
                .types)
            .empty());
    EXPECT_TRUE(
        heimdall::SemanticRules::AnalyzePassByValue(Typed("void f(const int& n) {}\n").types)
            .empty());
}

// ---- api/pass-by-const-reference --------------------------------------------

TEST(ApiPassByConstReference, ReportsAByValueParameterThatIsOnlyRead)
{
    const Typed typed("struct Foo { int x; };\nint read(Foo foo) { int y = foo.x; return y; }\n");
    const auto  diagnostics = heimdall::SemanticRules::AnalyzePassByConstReference(typed.flow);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "api/pass-by-const-reference");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::ApiPassByConstReference);
    ASSERT_TRUE(diagnostics[0].has_fix);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(Fixed(typed.source, diagnostics[0]),
              "struct Foo { int x; };\nint read(const Foo& foo) { int y = foo.x; return y; }\n");
}

TEST(ApiPassByConstReference, SilentWhenModifiedOrCheapOrMoved)
{
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzePassByConstReference(
                    Typed("struct Foo { int x; };\nvoid bump(Foo foo) { foo.x += 1; }\n").flow)
                    .empty());
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzePassByConstReference(
                    Typed("void f(std::string_view text) { use(text); }\n").flow)
                    .empty());
    EXPECT_TRUE(
        heimdall::SemanticRules::AnalyzePassByConstReference(
            Typed("struct Foo { int x; };\nvoid sink(Foo foo) { take(std::move(foo)); }\n").flow)
            .empty());
}

// ---- api/const-correctness ---------------------------------------------------

TEST(ApiConstCorrectness, ReportsAReferenceParameterThatIsNeverModified)
{
    // Routed through a `const Foo&` callee so the flow model sees a pure read
    // (returning a member directly escapes in the flow model: it may move).
    const Typed typed("struct Foo { int x; };\n"
                      "int area(const Foo& f) { return f.x; }\n"
                      "int peek(Foo& foo) { return area(foo); }\n");
    const auto  diagnostics = heimdall::SemanticRules::AnalyzeConstCorrectness(typed.flow);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "api/const-correctness");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::ApiConstCorrectness);
    ASSERT_TRUE(diagnostics[0].has_fix);
    EXPECT_EQ(Fixed(typed.source, diagnostics[0]),
              "struct Foo { int x; };\n"
              "int area(const Foo& f) { return f.x; }\n"
              "int peek(const Foo& foo) { return area(foo); }\n");
}

TEST(ApiConstCorrectness, ReportsAMemberFunctionThatReadsOnly)
{
    const Typed typed("struct A { int x; int get() { return x; } };\n");
    const auto  diagnostics = heimdall::SemanticRules::AnalyzeConstCorrectness(typed.flow);
    ASSERT_EQ(diagnostics.size(), 1u);
    ASSERT_TRUE(diagnostics[0].has_fix);
    EXPECT_EQ(Fixed(typed.source, diagnostics[0]),
              "struct A { int x; int get() const { return x; } };\n");
}

TEST(ApiConstCorrectness, SilentForWritersStaticAndUnknownCalls)
{
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeConstCorrectness(
                    Typed("struct A { int x; void set(int v) { x = v; } };\n").flow)
                    .empty());
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeConstCorrectness(
                    Typed("struct A { int x; static int zero() { return 0; } };\n").flow)
                    .empty());
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeConstCorrectness(
                    Typed("struct A { int x; int f() { return helper(x); } };\n").flow)
                    .empty());
}

// ---- api/unsafe-downcast -----------------------------------------------------

TEST(ApiUnsafeDowncast, ReportsAnUncheckedDowncast)
{
    const Typed typed("struct Base { int x; };\n"
                      "struct Derived : Base { int y; };\n"
                      "void f(Base* base) { Derived* derived = static_cast<Derived*>(base); }\n");
    const auto  diagnostics = heimdall::SemanticRules::AnalyzeUnsafeDowncast(typed.types);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "api/unsafe-downcast");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::ApiUnsafeDowncast);
    EXPECT_FALSE(diagnostics[0].has_fix);
    EXPECT_NE(diagnostics[0].message.find("Derived"), std::string::npos);
}

TEST(ApiUnsafeDowncast, SilentForUpcastsUnrelatedCastsAndDynamicCast)
{
    EXPECT_TRUE(
        heimdall::SemanticRules::AnalyzeUnsafeDowncast(
            Typed("struct B {};\nstruct D : B {};\nvoid f(D* d) { B* b = static_cast<B*>(d); }\n")
                .types)
            .empty());
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeUnsafeDowncast(
                    Typed("struct A {};\nstruct B {};\nvoid f(A* a) { (void)a; }\n").types)
                    .empty());
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeUnsafeDowncast(
                    Typed("struct B { virtual ~B(); };\nstruct D : B {};\nvoid f(B* b) { D* d = "
                          "dynamic_cast<D*>(b); }\n")
                        .types)
                    .empty());
}

// ---- api/slicing ---------------------------------------------------------------

TEST(ApiSlicing, ReportsACopyInitializationThatSlices)
{
    const Typed typed("struct Base { int x; };\n"
                      "struct Derived : Base { int y; };\n"
                      "void f(Derived derived) { Base base = derived; }\n");
    const auto  diagnostics = heimdall::SemanticRules::AnalyzeSlicing(typed.types);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "api/slicing");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::ApiSlicing);
    EXPECT_FALSE(diagnostics[0].has_fix);
}

TEST(ApiSlicing, ReportsAssignmentsAndReturnsByValue)
{
    const Typed assigned("struct Base { int x; };\n"
                         "struct Derived : Base { int y; };\n"
                         "void f(Derived derived) { Base base; base = derived; }\n");
    EXPECT_EQ(heimdall::SemanticRules::AnalyzeSlicing(assigned.types).size(), 1u);

    const Typed returned("struct Base { int x; };\n"
                         "struct Derived : Base { int y; };\n"
                         "Base f(Derived derived) { return derived; }\n");
    EXPECT_EQ(heimdall::SemanticRules::AnalyzeSlicing(returned.types).size(), 1u);
}

TEST(ApiSlicing, SilentForSameTypesAndReferences)
{
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeSlicing(
                    Typed("struct B { int x; };\nvoid f(B first) { B second = first; }\n").types)
                    .empty());
    EXPECT_TRUE(
        heimdall::SemanticRules::AnalyzeSlicing(
            Typed("struct B { int x; };\nstruct D : B {};\nvoid f(D d) { B& ref = d; }\n").types)
            .empty());
}

// ---- api/implicit-conversion ----------------------------------------------------

TEST(ApiImplicitConversion, ReportsANonExplicitConversionOperator)
{
    const std::string source = "struct A { operator int() const; };\n";
    const auto diagnostics   = RunRule(heimdall::SemanticRules::AnalyzeImplicitConversion, source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "api/implicit-conversion");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::ApiImplicitConversion);
    EXPECT_FALSE(diagnostics[0].has_fix);
}

TEST(ApiImplicitConversion, SilentForExplicitOperatorsAndOverloadedOperators)
{
    EXPECT_TRUE(RunRule(heimdall::SemanticRules::AnalyzeImplicitConversion,
                        "struct A { explicit operator int() const; };\n")
                    .empty());
    EXPECT_TRUE(
        RunRule(
            heimdall::SemanticRules::AnalyzeImplicitConversion,
            "struct A { A operator+(const A& other) const; bool operator==(const A& o) const; };\n")
            .empty());
}

// ---- api/virtual-call-in-constructor: initializer lists ------------------------------

TEST(ApiVirtualCallInConstructor, ReportsCallsInTheMemberInitializerList)
{
    const auto diagnostics = VirtualCall(
        "struct A {\n"
        "    virtual int level();\n"
        "    int value_;\n"
        "    A() : value_(level()) {}\n"
        "};\n");
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "api/virtual-call-in-constructor");
}

// ---- api/virtual-destructor: override-only polymorphism ------------------------------

TEST(ApiVirtualDestructor, ReportsOverrideOnlyFunctionsWithAResolvedBase)
{
    // Both the base (virtual without a virtual destructor) and the derived
    // class (polymorphic through `override`) are reported.
    const auto diagnostics = VirtualDestructor("struct Base { virtual void f(); };\n"
                                               "struct Derived : Base { void f() override; };\n");
    ASSERT_EQ(diagnostics.size(), 2u);
    EXPECT_EQ(diagnostics[1].line, 2u);
}
