#include <Heimdall/RuleEngine.hpp>
#include <Heimdall/SemanticRules.hpp>

#include <gtest/gtest.h>

#include <functional>
#include <string>

namespace
{

    using Analysis = std::vector<heimdall::Diagnostic> (*)(const heimdall::SemanticModel &);

    std::vector<heimdall::Diagnostic> Run(Analysis analysis, const std::string &source)
    {
        const auto tree = heimdall::ParseTree::Parse(source);
        const auto model = heimdall::Binder::Bind(tree);
        return analysis(model);
    }

    std::vector<heimdall::Diagnostic> Nullptr(const std::string &source)
    {
        return Run(heimdall::SemanticRules::AnalyzeNullptr, source);
    }

    std::vector<heimdall::Diagnostic> Zero(const std::string &source)
    {
        return Run(heimdall::SemanticRules::AnalyzeZeroAsNull, source);
    }

    std::vector<heimdall::Diagnostic> Auto(const std::string &source)
    {
        return Run(heimdall::SemanticRules::AnalyzeAuto, source);
    }

    // Applies every fix, right to left.
    std::string ApplyAll(std::string source, std::vector<heimdall::Diagnostic> diagnostics)
    {
        std::sort(diagnostics.begin(), diagnostics.end(),
            [](const heimdall::Diagnostic &a, const heimdall::Diagnostic &b)
            {
                return a.fix.offset > b.fix.offset;
            });
        for (const auto &diagnostic: diagnostics)
        {
            source.replace(diagnostic.fix.offset, diagnostic.fix.length, diagnostic.fix.replacement);
        }

        return source;
    }

    std::string Flagged(const std::string &source, const heimdall::Diagnostic &diagnostic)
    {
        return source.substr(diagnostic.offset, diagnostic.length);
    }

} // namespace

// ---- cpp/modernize-nullptr ---------------------------------------------------

TEST(ModernizeNullptr, ReplacesCStyleCastsOfNullConstants)
{
    const std::string source =
        "void f() {\n"
        "    void* a = (void*)0;\n"
        "    char* b = (char*)NULL;\n"
        "    const int* c = (const int*)0L;\n"
        "    ns::T** d = (ns::T**)0;\n"
        "}\n";
    const auto diagnostics = Nullptr(source);
    ASSERT_EQ(diagnostics.size(), 4u);
    EXPECT_EQ(Flagged(source, diagnostics[0]), "(void*)0");
    EXPECT_EQ(Flagged(source, diagnostics[1]), "(char*)NULL");
    EXPECT_EQ(Flagged(source, diagnostics[2]), "(const int*)0L");
    EXPECT_EQ(Flagged(source, diagnostics[3]), "(ns::T**)0");
    EXPECT_EQ(ApplyAll(source, diagnostics),
        "void f() {\n"
        "    void* a = nullptr;\n"
        "    char* b = nullptr;\n"
        "    const int* c = nullptr;\n"
        "    ns::T** d = nullptr;\n"
        "}\n");
}

TEST(ModernizeNullptr, ReplacesNamedCasts)
{
    const std::string source =
        "void* a = static_cast<void*>(0);\n"
        "char* b = reinterpret_cast<char*>(NULL);\n"
        "T* c = static_cast<std::vector<int>::value_type*>(0);\n";
    const auto diagnostics = Nullptr(source);
    ASSERT_EQ(diagnostics.size(), 3u);
    EXPECT_EQ(ApplyAll(source, diagnostics),
        "void* a = nullptr;\n"
        "char* b = nullptr;\n"
        "T* c = nullptr;\n");
}

TEST(ModernizeNullptr, FixIsOnlyAQuickFix)
{
    const std::string source = "void* a = (void*)0;\n";
    const auto diagnostics = Nullptr(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "cpp/modernize-nullptr");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::ModernizeNullptr);
    EXPECT_EQ(diagnostics[0].line, 1u);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics), source);
}

TEST(ModernizeNullptr, LeavesNonPointerCastsAlone)
{
    EXPECT_TRUE(Nullptr(
        "int a = (int)0;\n"
        "long b = (long)NULL;\n"
        "float c = static_cast<float>(0);\n"
        "void* d = (void*)p;\n"
        "void* e = (void*)1;\n"
        "T* f = static_cast<T*>(other);\n"
        "int g = (a * b) 0;\n").empty());
}

TEST(ModernizeNullptr, LeavesCallsAndKeywordConstructsAlone)
{
    EXPECT_TRUE(Nullptr(
        "void f(int*) 0;\n"
        "auto s = sizeof(int*) 0;\n"
        "if (x) 0;\n"
        "g(T*) 0;\n").empty());
}

TEST(ModernizeNullptr, KeepsTheOffsetofIdiomWorking)
{
    // `nullptr->member` does not compile: the cast result is used as an object.
    EXPECT_TRUE(Nullptr(
        "long a = (long)&((Foo*)0)->member;\n"
        "long b = (long)&((Foo*)NULL)->member;\n"
        "auto c = ((Foo*)0)[1];\n"
        "int d = static_cast<Foo*>(0)->x;\n").empty());
}

TEST(ModernizeNullptr, IgnoresInactiveCodeAndDirectives)
{
    EXPECT_TRUE(Nullptr(
        "#define NIL (void*)0\n"
        "#if 0\n"
        "void* a = (void*)0;\n"
        "#endif\n").empty());
}

TEST(ModernizeNullptr, ToleratesBrokenInput)
{
    for (const char *source: {"void* a = (void*", "void* a = (void*)", "static_cast<void*>(", "(", "static_cast<", ""})
    {
        EXPECT_NO_FATAL_FAILURE(Nullptr(source)) << source;
    }
}

// ---- cpp/no-zero-as-null -----------------------------------------------------

TEST(NoZeroAsNull, ReportsInitializationOfPointers)
{
    const std::string source =
        "int* g = 0;\n"
        "void f(int* p = 0) {\n"
        "    char *a = 0, *b = 0L;\n"
        "    const char* c = 0u;\n"
        "}\n"
        "struct S { int* member = 0; };\n";
    const auto diagnostics = Zero(source);
    ASSERT_EQ(diagnostics.size(), 6u);
    for (const auto &diagnostic: diagnostics)
    {
        EXPECT_EQ(diagnostic.code, "cpp/no-zero-as-null");
        EXPECT_EQ(diagnostic.rule, heimdall::RuleId::NoZeroAsNull);
        EXPECT_TRUE(diagnostic.fix_is_safe);
    }

    EXPECT_EQ(ApplyAll(source, diagnostics),
        "int* g = nullptr;\n"
        "void f(int* p = nullptr) {\n"
        "    char *a = nullptr, *b = nullptr;\n"
        "    const char* c = nullptr;\n"
        "}\n"
        "struct S { int* member = nullptr; };\n");
}

TEST(NoZeroAsNull, ReportsAssignmentsAndComparisons)
{
    const std::string source =
        "void f(int* p, int* q) {\n"
        "    p = 0;\n"
        "    if (p == 0 && q != 0) {}\n"
        "    if (0 == p || 0 != q) {}\n"
        "    bool b = p == 0L;\n"
        "}\n";
    const auto diagnostics = Zero(source);
    ASSERT_EQ(diagnostics.size(), 6u);
    EXPECT_EQ(ApplyAll(source, diagnostics),
        "void f(int* p, int* q) {\n"
        "    p = nullptr;\n"
        "    if (p == nullptr && q != nullptr) {}\n"
        "    if (nullptr == p || nullptr != q) {}\n"
        "    bool b = p == nullptr;\n"
        "}\n");
}

TEST(NoZeroAsNull, ReportsReturnsFromPointerFunctions)
{
    const std::string source =
        "int* a() { return 0; }\n"
        "auto b() -> char* { return 0; }\n"
        "struct S { const char* c() { return 0; } };\n"
        "int* S2::d() { return 0L; }\n";
    const auto diagnostics = Zero(source);
    ASSERT_EQ(diagnostics.size(), 4u);
    EXPECT_NE(diagnostics[0].message.find("returned"), std::string::npos);
}

TEST(NoZeroAsNull, LeavesIntegersAlone)
{
    EXPECT_TRUE(Zero(
        "int n = 0;\n"
        "int f(int a) { a = 0; if (a == 0) {} return 0; }\n"
        "long l(long a) { return 0L; }\n").empty());
}

TEST(NoZeroAsNull, OnlyTrustsPointersItCanSee)
{
    // Typedef'd pointers, auto, references to pointers, arrays and unknown names
    // are not provably pointers: the rule stays silent.
    EXPECT_TRUE(Zero(
        "typedef int* Handle;\n"
        "void f(Handle h, auto a, int*& r, int* arr[2], Unknown u) {\n"
        "    h = 0;\n"
        "    a = 0;\n"
        "    r = 0;\n"
        "    u = 0;\n"
        "    missing = 0;\n"
        "}\n").empty());
}

TEST(NoZeroAsNull, DoesNotConfusePointersWithOtherDeclarators)
{
    // `b` shares the declaration of the pointer `a` but is an int.
    const std::string source = "void f() { int *a = 0, b = 0; b = 0; if (b == 0) {} }\n";
    const auto diagnostics = Zero(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].column, 21u);
}

TEST(NoZeroAsNull, RespectsShadowing)
{
    const std::string source =
        "int* p;\n"
        "void f() {\n"
        "    int p = 1;\n"
        "    p = 0;\n"
        "}\n"
        "void g() { p = 0; }\n";
    const auto diagnostics = Zero(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].line, 6u);
}

TEST(NoZeroAsNull, MemberAccessThroughObjectsIsUnknown)
{
    EXPECT_TRUE(Zero(
        "struct S { int* p; };\n"
        "void f(S s, S* t) { s.p = 0; t->p = 0; if (s.p == 0) {} }\n").empty());
}

TEST(NoZeroAsNull, LambdaReturnsAreUnknown)
{
    EXPECT_TRUE(Zero("int* f() { auto l = [] { return 0; }; return l(); }\n").empty());
}

TEST(NoZeroAsNull, ArgumentsNeedOverloadResolutionAndAreLeftAlone)
{
    EXPECT_TRUE(Zero("void g(int*); void f() { g(0); }\n").empty());
}

TEST(NoZeroAsNull, IgnoresInactiveCode)
{
    EXPECT_TRUE(Zero(
        "#if 0\n"
        "int* p = 0;\n"
        "#endif\n").empty());
}

TEST(NoZeroAsNull, ToleratesBrokenInput)
{
    for (const char *source: {"int* p = ", "int* p = 0", "void f(int* p) { p ==", "int* f() { return", "int* f() { return 0"})
    {
        EXPECT_NO_FATAL_FAILURE(Zero(source)) << source;
    }
}

// ---- cpp/modernize-auto ------------------------------------------------------

TEST(ModernizeAuto, ReplacesTheTypeOfNewExpressions)
{
    const std::string source =
        "void f() {\n"
        "    Foo* a = new Foo(1);\n"
        "    ns::Foo* b = new ns::Foo{2};\n"
        "    Foo* c = new Foo;\n"
        "    int* d = new int[10];\n"
        "    Foo** e = new Foo*[3];\n"
        "}\n";
    const auto diagnostics = Auto(source);
    ASSERT_EQ(diagnostics.size(), 5u);
    for (const auto &diagnostic: diagnostics)
    {
        EXPECT_EQ(diagnostic.code, "cpp/modernize-auto");
        EXPECT_EQ(diagnostic.rule, heimdall::RuleId::ModernizeAuto);
        EXPECT_TRUE(diagnostic.fix_is_safe);
    }

    EXPECT_EQ(Flagged(source, diagnostics[0]), "Foo*");
    EXPECT_EQ(ApplyAll(source, diagnostics),
        "void f() {\n"
        "    auto* a = new Foo(1);\n"
        "    auto* b = new ns::Foo{2};\n"
        "    auto* c = new Foo;\n"
        "    auto* d = new int[10];\n"
        "    auto** e = new Foo*[3];\n"
        "}\n");
}

TEST(ModernizeAuto, ReplacesTheTypeOfCasts)
{
    const std::string source =
        "void f(Base* b, double d) {\n"
        "    Derived* a = static_cast<Derived*>(b);\n"
        "    Derived* c = dynamic_cast<Derived*>(b);\n"
        "    int n = static_cast<int>(d);\n"
        "    std::string s = static_cast<std::string>(d);\n"
        "    char* r = reinterpret_cast<char*>(b);\n"
        "}\n";
    const auto diagnostics = Auto(source);
    ASSERT_EQ(diagnostics.size(), 5u);
    EXPECT_EQ(ApplyAll(source, diagnostics),
        "void f(Base* b, double d) {\n"
        "    auto* a = static_cast<Derived*>(b);\n"
        "    auto* c = dynamic_cast<Derived*>(b);\n"
        "    auto n = static_cast<int>(d);\n"
        "    auto s = static_cast<std::string>(d);\n"
        "    auto* r = reinterpret_cast<char*>(b);\n"
        "}\n");
}

TEST(ModernizeAuto, ReplacesSmartPointerFactories)
{
    const std::string source =
        "void f() {\n"
        "    std::unique_ptr<Foo> a = std::make_unique<Foo>(1);\n"
        "    std::shared_ptr<Foo> b = std::make_shared<Foo>();\n"
        "    std::unique_ptr<std::vector<int>> c = std::make_unique<std::vector<int>>();\n"
        "}\n";
    const auto diagnostics = Auto(source);
    ASSERT_EQ(diagnostics.size(), 3u);
    EXPECT_EQ(ApplyAll(source, diagnostics),
        "void f() {\n"
        "    auto a = std::make_unique<Foo>(1);\n"
        "    auto b = std::make_shared<Foo>();\n"
        "    auto c = std::make_unique<std::vector<int>>();\n"
        "}\n");
}

TEST(ModernizeAuto, KeepsStorageSpecifiers)
{
    const std::string source =
        "void f() {\n"
        "    static Foo* a = new Foo;\n"
        "    constexpr int n = static_cast<int>(3.0);\n"
        "}\n";
    const auto diagnostics = Auto(source);
    ASSERT_EQ(diagnostics.size(), 2u);
    EXPECT_EQ(ApplyAll(source, diagnostics),
        "void f() {\n"
        "    static auto* a = new Foo;\n"
        "    constexpr auto n = static_cast<int>(3.0);\n"
        "}\n");
}

TEST(ModernizeAuto, SilentWhenTheTypesDiffer)
{
    EXPECT_TRUE(Auto(
        "void f(Derived* d, int i) {\n"
        "    Base* a = new Derived;\n"
        "    Base* b = static_cast<Derived*>(d);\n"
        "    long c = static_cast<int>(i);\n"
        "    std::unique_ptr<Base> e = std::make_unique<Derived>();\n"
        "    std::shared_ptr<Foo> g = std::make_unique<Foo>();\n"
        "    Foo h = Foo(1);\n"
        "    int j = i;\n"
        "}\n").empty());
}

TEST(ModernizeAuto, SilentForQualifiersReferencesAndArrays)
{
    EXPECT_TRUE(Auto(
        "void f(Foo* p) {\n"
        "    const Foo* a = new Foo;\n"
        "    Foo* const b = new Foo;\n"
        "    Foo& c = static_cast<Foo&>(*p);\n"
        "    const int d = static_cast<const int>(1);\n"
        "    int e[2] = static_cast<int[2]>(x);\n"
        "}\n").empty());
}

TEST(ModernizeAuto, SilentForMultipleDeclaratorsAndPlacementNew)
{
    EXPECT_TRUE(Auto(
        "void f(void* mem) {\n"
        "    Foo* a = new Foo, *b = new Foo;\n"
        "    Foo* c = new (mem) Foo;\n"
        "    Foo* d = new Foo(1) + 1;\n"
        "}\n").empty());
}

TEST(ModernizeAuto, SilentForClassMembers)
{
    EXPECT_TRUE(Auto(
        "struct S {\n"
        "    Foo* a = new Foo;\n"
        "    static constexpr int n = static_cast<int>(3);\n"
        "};\n").empty());
}

TEST(ModernizeAuto, SilentForDeclarationsThatCannotUseAuto)
{
    EXPECT_TRUE(Auto(
        "extern Foo* a = new Foo;\n"
        "typedef Foo* T;\n"
        "auto b = new Foo;\n"
        "Foo c(new Foo);\n"
        "Foo* d;\n").empty());
}

TEST(ModernizeAuto, LeavesNullCastsToModernizeNullptr)
{
    // After cpp/modernize-nullptr there is nothing to deduce from.
    EXPECT_TRUE(Auto("void* w = static_cast<void*>(0);\n").empty());
    EXPECT_EQ(Nullptr("void* w = static_cast<void*>(0);\n").size(), 1u);
}

TEST(ModernizeAuto, ReportsLocationOfTheType)
{
    const std::string source = "void f() {\n    Foo* a = new Foo;\n}\n";
    const auto diagnostics = Auto(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].line, 2u);
    EXPECT_EQ(diagnostics[0].column, 5u);
    EXPECT_EQ(diagnostics[0].fix_title, "Use auto*");
}

TEST(ModernizeAuto, IgnoresInactiveCode)
{
    EXPECT_TRUE(Auto(
        "#if 0\n"
        "Foo* a = new Foo;\n"
        "#endif\n").empty());
}

TEST(ModernizeAuto, ToleratesBrokenInput)
{
    for (const char *source: {"Foo* a = new", "Foo* a = new Foo(", "int n = static_cast<", "int n = static_cast<int>(",
             "std::unique_ptr<Foo> p = std::make_unique<Foo>(", "Foo* a ="})
    {
        EXPECT_NO_FATAL_FAILURE(Auto(source)) << source;
    }
}

// ---- all rules together -----------------------------------------------------

TEST(SemanticRulesAll, ReturnsEveryRuleSortedByOffset)
{
    const std::string source =
        "struct Base { virtual void f(); };\n"
        "struct D : Base { void f(); };\n"
        "void g(int* p) {\n"
        "    p = 0;\n"
        "    void* q = (void*)0;\n"
        "    Foo* r = new Foo;\n"
        "}\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    const auto model = heimdall::Binder::Bind(tree);
    const auto diagnostics = heimdall::SemanticRules::Analyze(model);
    ASSERT_EQ(diagnostics.size(), 4u);
    EXPECT_EQ(diagnostics[0].code, "cpp/modernize-override");
    EXPECT_EQ(diagnostics[1].code, "cpp/no-zero-as-null");
    EXPECT_EQ(diagnostics[2].code, "cpp/modernize-nullptr");
    EXPECT_EQ(diagnostics[3].code, "cpp/modernize-auto");
    for (std::size_t i = 1; i < diagnostics.size(); ++i)
    {
        EXPECT_LE(diagnostics[i - 1].offset, diagnostics[i].offset);
    }
}

TEST(SemanticRulesAll, NewRulesAreInTheCatalog)
{
    EXPECT_TRUE(heimdall::IsKnownRuleCode("cpp/modernize-nullptr"));
    EXPECT_TRUE(heimdall::IsKnownRuleCode("cpp/no-zero-as-null"));
    EXPECT_TRUE(heimdall::IsKnownRuleCode("cpp/modernize-auto"));
    EXPECT_EQ(heimdall::FindRuleByCode("cpp/modernize-nullptr")->id, heimdall::RuleId::ModernizeNullptr);
    EXPECT_EQ(heimdall::FindRuleByCode("cpp/no-zero-as-null")->id, heimdall::RuleId::NoZeroAsNull);
    EXPECT_EQ(heimdall::FindRuleByCode("cpp/modernize-auto")->id, heimdall::RuleId::ModernizeAuto);
}

TEST(SemanticRulesAll, PolicyCanDisableAndSuppressEachRule)
{
    const std::string source =
        "void g(int* p) {\n"
        "    p = 0; // heimdall-disable-line cpp/no-zero-as-null\n"
        "    void* q = (void*)0;\n"
        "    Foo* r = new Foo;\n"
        "}\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    const auto model = heimdall::Binder::Bind(tree);
    const auto raw = heimdall::SemanticRules::Analyze(model);
    ASSERT_EQ(raw.size(), 3u);

    heimdall::RuleOptions options;
    options.overrides.push_back({"cpp/modernize-auto", false, heimdall::Severity::Warning});
    const auto filtered = heimdall::RuleEngine(options).ApplyPolicy(raw, tree);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].code, "cpp/modernize-nullptr");
}
