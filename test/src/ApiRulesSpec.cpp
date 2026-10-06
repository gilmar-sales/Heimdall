#include <Heimdall/RuleEngine.hpp>
#include <Heimdall/SemanticRules.hpp>

#include <gtest/gtest.h>

#include <string>

namespace
{

    using Analyzer = std::vector<heimdall::Diagnostic>(*)(const heimdall::SemanticModel&);

    std::vector<heimdall::Diagnostic> Run(Analyzer analyzer, const std::string& source)
    {
        const auto tree = heimdall::ParseTree::Parse(source);
        const auto model = heimdall::Binder::Bind(tree);
        return analyzer(model);
    }

    std::vector<heimdall::Diagnostic> VirtualDestructor(const std::string& source)
    {
        return Run(heimdall::SemanticRules::AnalyzeVirtualDestructor, source);
    }

    std::vector<heimdall::Diagnostic> ExplicitConstructor(const std::string& source)
    {
        return Run(heimdall::SemanticRules::AnalyzeExplicitConstructor, source);
    }

    std::vector<heimdall::Diagnostic> OverloadHiding(const std::string& source)
    {
        return Run(heimdall::SemanticRules::AnalyzeOverloadHiding, source);
    }

    std::vector<heimdall::Diagnostic> VirtualCall(const std::string& source)
    {
        return Run(heimdall::SemanticRules::AnalyzeVirtualCallInConstructor, source);
    }

    std::string Fixed(std::string source, const heimdall::Diagnostic& diagnostic)
    {
        source.replace(diagnostic.fix.offset, diagnostic.fix.length, diagnostic.fix.replacement);
        return source;
    }

} // namespace

// ---- api/virtual-destructor ------------------------------------------------

TEST(ApiVirtualDestructor, ReportsAClassWithVirtualFunctionsAndNoDestructor)
{
    const std::string source = "struct Shape { virtual void draw(); };\n";
    const auto diagnostics = VirtualDestructor(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "api/virtual-destructor");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::ApiVirtualDestructor);
    EXPECT_EQ(source.substr(diagnostics[0].offset, diagnostics[0].length), "Shape");
    EXPECT_FALSE(diagnostics[0].has_fix);
}

TEST(ApiVirtualDestructor, QuickFixMakesADeclaredDestructorVirtual)
{
    const std::string source = "class A { public: virtual void f(); ~A(); };\n";
    const auto diagnostics = VirtualDestructor(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    ASSERT_TRUE(diagnostics[0].has_fix);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(Fixed(source, diagnostics[0]), "class A { public: virtual void f(); virtual ~A(); };\n");
}

TEST(ApiVirtualDestructor, ReportsPureVirtualInterfaces)
{
    EXPECT_EQ(VirtualDestructor("struct I { virtual void f() = 0; };\n").size(), 1u);
}

TEST(ApiVirtualDestructor, SilentWhenTheDestructorIsVirtual)
{
    EXPECT_TRUE(VirtualDestructor("struct A { virtual void f(); virtual ~A(); };\n").empty());
    EXPECT_TRUE(VirtualDestructor("struct A { virtual void f(); virtual ~A() = default; };\n").empty());
}

TEST(ApiVirtualDestructor, SilentWhenANonPublicDestructorForbidsDeletingThroughTheBase)
{
    EXPECT_TRUE(VirtualDestructor("struct A { virtual void f(); protected: ~A(); };\n").empty());
    EXPECT_TRUE(VirtualDestructor("class A { virtual void f(); ~A(); };\n").empty());
}

TEST(ApiVirtualDestructor, SilentWhenABaseHasAVirtualDestructor)
{
    EXPECT_TRUE(VirtualDestructor(
        "struct Base { virtual ~Base(); };\n"
        "struct Derived : Base { virtual void g(); };\n").empty());
}

TEST(ApiVirtualDestructor, ReportsWhenNoBaseHasAVirtualDestructor)
{
    const auto diagnostics = VirtualDestructor(
        "struct Base { void f(); };\n"
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
    const std::string source = "struct A { A(int x); };\n";
    const auto diagnostics = ExplicitConstructor(source);
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
    const std::string source = "struct A { constexpr A(int x) {} };\n";
    const auto diagnostics = ExplicitConstructor(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(Fixed(source, diagnostics[0]), "struct A { explicit constexpr A(int x) {} };\n");

    const std::string templated = "struct B { template<class T> B(T t); };\n";
    const auto templated_diagnostics = ExplicitConstructor(templated);
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
    EXPECT_TRUE(ExplicitConstructor("struct A { public: explicit constexpr A(int x); };\n").empty());
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
    EXPECT_TRUE(ExplicitConstructor("struct A { void f(int x); static A make(int x); };\n").empty());
    EXPECT_TRUE(ExplicitConstructor("struct A { A(int x) = delete; };\n").empty());
}

// ---- api/overload-hiding ---------------------------------------------------

TEST(ApiOverloadHiding, ReportsADerivedOverloadThatHidesAVirtualBaseFunction)
{
    const std::string source =
        "struct Base { virtual void f(int x); };\n"
    "struct Derived : Base { void f(double d); };\n";
    const auto diagnostics = OverloadHiding(source);
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
    EXPECT_TRUE(OverloadHiding(
        "struct Base { virtual void f(int x); };\n"
        "struct Derived : Base { void f(int y) override; };\n").empty());
}

TEST(ApiOverloadHiding, SilentWithAUsingDeclaration)
{
    EXPECT_TRUE(OverloadHiding(
        "struct Base { virtual void f(int x); };\n"
        "struct Derived : Base { using Base::f; void f(double d); };\n").empty());
}

TEST(ApiOverloadHiding, SilentWhenTheBaseFunctionIsNotVirtual)
{
    EXPECT_TRUE(OverloadHiding(
        "struct Base { void f(int x); };\n"
        "struct Derived : Base { void f(double d); };\n").empty());
}

TEST(ApiOverloadHiding, SilentWhenBasesDoNotResolveOrNamesDiffer)
{
    EXPECT_TRUE(OverloadHiding("struct D : Missing { void f(double d); };\n").empty());
    EXPECT_TRUE(OverloadHiding(
        "struct Base { virtual void f(int x); };\n"
        "struct Derived : Base { void g(double d); };\n").empty());
}

TEST(ApiOverloadHiding, ConstQualifiedOverloadsAreDifferentSignatures)
{
    EXPECT_EQ(OverloadHiding(
        "struct Base { virtual void f(); };\n"
        "struct Derived : Base { void f() const; };\n").size(), 1u);
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
    const auto diagnostics = VirtualCall(
        "struct Base { virtual void f(); };\n"
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
    EXPECT_TRUE(VirtualCall("struct A { virtual void init(); A() { auto f = [this] { init(); }; } };\n").empty());
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
        {
            "api/virtual-destructor", "api/explicit-constructor", "api/overload-hiding",
            "api/virtual-call-in-constructor"
    })
    {
        EXPECT_TRUE(heimdall::IsKnownRuleCode(code)) << code;
    }

    const std::string source = "struct A { A(int x); virtual void f(); };\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    const auto model = heimdall::Binder::Bind(tree);
    const auto all = heimdall::SemanticRules::Analyze(model);
    ASSERT_EQ(all.size(), 2u);
}
