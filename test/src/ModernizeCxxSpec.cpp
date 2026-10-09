#include <Heimdall/RuleEngine.hpp>
#include <Heimdall/SemanticRules.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace
{

    std::vector<heimdall::Diagnostic> Lint(const std::string& source)
    {
        return heimdall::RuleEngine().Analyze(source);
    }

    std::vector<heimdall::Diagnostic> ByCode(std::vector<heimdall::Diagnostic> diagnostics,
                                             std::string_view                  code)
    {
        std::vector<heimdall::Diagnostic> kept;
        for (auto& diagnostic : diagnostics)
        {
            if (diagnostic.code == code)
            {
                kept.push_back(diagnostic);
            }
        }

        return kept;
    }

    std::vector<heimdall::Diagnostic> ByCode(const std::string& source, std::string_view code)
    {
        return ByCode(Lint(source), code);
    }

    // Owns every layer so the references between them stay valid.
    struct Typed
    {
        explicit Typed(std::string text) :
            source(std::move(text)), tree(heimdall::ParseTree::Parse(source)),
            model(heimdall::Binder::Bind(tree)), types(heimdall::Typer::Type(model))
        {
        }

        Typed(const Typed&) = delete;

        Typed& operator=(const Typed&) = delete;

        std::string             source;
        heimdall::ParseTree     tree;
        heimdall::SemanticModel model;
        heimdall::TypeModel     types;
    };

    std::vector<heimdall::Diagnostic> Bound(
        std::vector<heimdall::Diagnostic> (*analysis)(const heimdall::SemanticModel&),
        const std::string& source)
    {
        const auto tree  = heimdall::ParseTree::Parse(source);
        const auto model = heimdall::Binder::Bind(tree);
        return analysis(model);
    }

    std::string ApplyOne(const std::string& source, const heimdall::Diagnostic& diagnostic)
    {
        std::string result(source);
        result.replace(diagnostic.fix.offset, diagnostic.fix.length, diagnostic.fix.replacement);
        return result;
    }

} // namespace

// ---- cpp/modernize-emplace ---------------------------------------------------

TEST(ModernizeCxxSpec, EmplaceRewritesPushBackOfTemporary)
{
    const auto diagnostics =
        ByCode("void f() { v.push_back(Foo(1, 2)); }\n", "cpp/modernize-emplace");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(diagnostics[0].fix.replacement, "emplace_back(1, 2)");
    EXPECT_EQ(ApplyOne("void f() { v.push_back(Foo(1, 2)); }\n", diagnostics[0]),
              "void f() { v.emplace_back(1, 2); }\n");
}

TEST(ModernizeCxxSpec, EmplaceUnwrapsBracedInit)
{
    const auto diagnostics = ByCode("void f() { v.push_back({1, 2}); }\n", "cpp/modernize-emplace");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].fix.replacement, "emplace_back(1, 2)");
}

TEST(ModernizeCxxSpec, EmplaceIgnoresPlainValues)
{
    EXPECT_TRUE(ByCode("void f() { v.push_back(x); }\n", "cpp/modernize-emplace").empty());
    EXPECT_TRUE(ByCode("void f() { v.push_back(new Foo()); }\n", "cpp/modernize-emplace").empty());
}

// ---- cpp/modernize-make-unique / make-shared ---------------------------------

TEST(ModernizeCxxSpec, MakeUniqueRewritesDeclaration)
{
    const std::string source      = "void f() { std::unique_ptr<Foo> p(new Foo(1)); }\n";
    const auto        diagnostics = ByCode(source, "cpp/modernize-make-unique");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(ApplyOne(source, diagnostics[0]),
              "void f() { auto p = std::make_unique<Foo>(1); }\n");
}

TEST(ModernizeCxxSpec, MakeUniqueRewritesTemporal)
{
    const std::string source      = "void f() { g(std::unique_ptr<Foo>(new Foo())); }\n";
    const auto        diagnostics = ByCode(source, "cpp/modernize-make-unique");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(ApplyOne(source, diagnostics[0]), "void f() { g(std::make_unique<Foo>()); }\n");
}

TEST(ModernizeCxxSpec, MakeSharedRewritesDeclaration)
{
    const std::string source      = "void f() { std::shared_ptr<Foo> p(new Foo(1, 2)); }\n";
    const auto        diagnostics = ByCode(source, "cpp/modernize-make-shared");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(ApplyOne(source, diagnostics[0]),
              "void f() { auto p = std::make_shared<Foo>(1, 2); }\n");
}

TEST(ModernizeCxxSpec, MakeUniqueNeedsSameTypeForFix)
{
    const auto diagnostics = ByCode("void f() { std::unique_ptr<Base> p(new Derived()); }\n",
                                    "cpp/modernize-make-unique");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_FALSE(diagnostics[0].has_fix);
}

TEST(ModernizeCxxSpec, ClaimedNewIsNotReportedByNoNewDelete)
{
    const auto diagnostics = Lint("void f() { std::unique_ptr<Foo> p(new Foo()); }\n");
    EXPECT_TRUE(ByCode(std::move(diagnostics), "cpp/no-new-delete").empty());
}

// ---- cpp/modernize-smart-ptr --------------------------------------------------

TEST(ModernizeCxxSpec, SmartPtrRewritesRawOwningPointer)
{
    const std::string source      = "void f() { Foo* p = new Foo(1); }\n";
    const auto        diagnostics = ByCode(source, "cpp/modernize-smart-ptr");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(ApplyOne(source, diagnostics[0]),
              "void f() { auto p = std::make_unique<Foo>(1); }\n");
    // The claimed `new` belongs to this rule, not to cpp/no-new-delete.
    EXPECT_TRUE(ByCode(source, "cpp/no-new-delete").empty());
}

TEST(ModernizeCxxSpec, SmartPtrFlagsResetWithoutFix)
{
    const auto diagnostics =
        ByCode("void f() { p.reset(new Foo()); }\n", "cpp/modernize-smart-ptr");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_FALSE(diagnostics[0].has_fix);
}

TEST(ModernizeCxxSpec, SmartPtrIgnoresDereferenceAssignment)
{
    EXPECT_TRUE(ByCode("void f() { *p = new Foo(); }\n", "cpp/modernize-smart-ptr").empty());
}

// ---- cpp/no-new-delete --------------------------------------------------------

TEST(ModernizeCxxSpec, NoNewDeleteFlagsBareNewAndDelete)
{
    const auto diagnostics =
        ByCode("void f() { g(new Foo()); delete p; delete[] q; }\n", "cpp/no-new-delete");
    ASSERT_EQ(diagnostics.size(), 3);
    for (const auto& diagnostic : diagnostics)
    {
        EXPECT_FALSE(diagnostic.has_fix);
    }
}

TEST(ModernizeCxxSpec, NoNewDeleteSkipsPlacementAndOperators)
{
    EXPECT_TRUE(ByCode("void f() { g(new (buffer) Foo()); }\n", "cpp/no-new-delete").empty());
    EXPECT_TRUE(ByCode("void* operator new(std::size_t n); void operator delete(void* p); \n",
                       "cpp/no-new-delete")
                    .empty());
}

// ---- cpp/modernize-structured-bindings ----------------------------------------

TEST(ModernizeCxxSpec, StructuredBindingsRewriteTie)
{
    const std::string source      = "void f() { std::tie(a, b) = pair; }\n";
    const auto        diagnostics = ByCode(source, "cpp/modernize-structured-bindings");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(ApplyOne(source, diagnostics[0]), "void f() { auto [a, b] = pair; }\n");
}

TEST(ModernizeCxxSpec, StructuredBindingsSuggestOnFirstAndSecond)
{
    const auto diagnostics = ByCode("int f() { return point.first + point.second; }\n",
                                    "cpp/modernize-structured-bindings");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_FALSE(diagnostics[0].has_fix);
    EXPECT_TRUE(
        ByCode("int f() { return point.first; }\n", "cpp/modernize-structured-bindings").empty());
}

// ---- cpp/modernize-algorithms --------------------------------------------------

TEST(ModernizeCxxSpec, AlgorithmsSuggestAccumulate)
{
    const auto diagnostics = ByCode("int f() { for (const auto& x : values) total += x; }\n",
                                    "cpp/modernize-algorithms");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_FALSE(diagnostics[0].has_fix);
}

TEST(ModernizeCxxSpec, AlgorithmsSuggestCountIf)
{
    const auto diagnostics =
        ByCode("int f() { for (int i = 0; i < n; ++i) if (valid(i)) ++count; }\n",
               "cpp/modernize-algorithms");
    ASSERT_EQ(diagnostics.size(), 1);
}

TEST(ModernizeCxxSpec, AlgorithmsSuggestCopyIf)
{
    const auto diagnostics =
        ByCode("void f() { for (auto x : in) if (keep(x)) out.push_back(x); }\n",
               "cpp/modernize-algorithms");
    ASSERT_EQ(diagnostics.size(), 1);
}

TEST(ModernizeCxxSpec, AlgorithmsIgnoresMutationsAndEscapes)
{
    EXPECT_TRUE(ByCode("void f() { for (auto& x : values) x += 1; }\n", "cpp/modernize-algorithms")
                    .empty());
    EXPECT_TRUE(
        ByCode("int f() { for (auto x : values) { if (x) break; } }\n", "cpp/modernize-algorithms")
            .empty());
    EXPECT_TRUE(ByCode("void f() { int i = 0; }\n", "cpp/modernize-algorithms").empty());
}

// ---- cpp/modernize-span --------------------------------------------------------

TEST(ModernizeCxxSpec, SpanMergesPointerAndSize)
{
    // Parameters only bind on definitions.
    Typed      fixture("void process(int* data, int size) {}\n");
    const auto diagnostics = heimdall::SemanticRules::AnalyzeSpan(fixture.types);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].code, "cpp/modernize-span");
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(ApplyOne(fixture.source, diagnostics[0]), "void process(std::span<int> data) {}\n");
}

TEST(ModernizeCxxSpec, SpanStaysSilentWithoutShape)
{
    Typed plain("void process(int* data, int other) {}\n");
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeSpan(plain.types).empty());
    Typed no_pointer("void process(int data, int size) {}\n");
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeSpan(no_pointer.types).empty());
    Typed virtual_member("struct Base { virtual void process(int* data, int size) {} };\n");
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeSpan(virtual_member.types).empty());
    Typed      void_pointer("void process(void* data, int size) {}\n");
    const auto diagnostics = heimdall::SemanticRules::AnalyzeSpan(void_pointer.types);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_FALSE(diagnostics[0].has_fix);
}

// ---- cpp/modernize-string-view --------------------------------------------------

TEST(ModernizeCxxSpec, StringViewRewritesConstValueParameter)
{
    const std::string source      = "void log(const std::string message) {}\n";
    const auto        diagnostics = Bound(heimdall::SemanticRules::AnalyzeStringView, source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(ApplyOne(source, diagnostics[0]), "void log(std::string_view message) {}\n");
}

TEST(ModernizeCxxSpec, StringViewKeepsSinksAndReferences)
{
    EXPECT_TRUE(
        Bound(heimdall::SemanticRules::AnalyzeStringView, "void set(std::string value) {}\n")
            .empty());
    EXPECT_TRUE(Bound(heimdall::SemanticRules::AnalyzeStringView,
                      "void log(const std::string& message) {}\n")
                    .empty());
}

// ---- cpp/modernize-attributes ----------------------------------------------------

TEST(ModernizeCxxSpec, AttributesSuggestNodiscardForQueries)
{
    Typed      fixture("int get_count();\n");
    const auto diagnostics = heimdall::SemanticRules::AnalyzeAttributes(fixture.types);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].code, "cpp/modernize-attributes");
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(ApplyOne(fixture.source, diagnostics[0]), "[[nodiscard]] int get_count();\n");
}

TEST(ModernizeCxxSpec, AttributesStaySilentOtherwise)
{
    Typed void_fn("void reset();\n");
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeAttributes(void_fn.types).empty());
    Typed not_query("int compute(int value) { return value * 2; }\n");
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeAttributes(not_query.types).empty());
    Typed already("[[nodiscard]] int get_count();\n");
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeAttributes(already.types).empty());
    Typed main("int main() { return 0; }\n");
    EXPECT_TRUE(heimdall::SemanticRules::AnalyzeAttributes(main.types).empty());
}

// ---- cpp/modernize-consteval-constexpr --------------------------------------------

TEST(ModernizeCxxSpec, ConstevalConstexprMakesConstantConstexpr)
{
    const std::string source = "const int kMax = 100;\n";
    const auto diagnostics   = Bound(heimdall::SemanticRules::AnalyzeConstevalConstexpr, source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_TRUE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics), "constexpr int kMax = 100;\n");
}

TEST(ModernizeCxxSpec, ConstevalConstexprStaysSilentOtherwise)
{
    EXPECT_TRUE(
        Bound(heimdall::SemanticRules::AnalyzeConstevalConstexpr, "int value = 5;\n").empty());
    EXPECT_TRUE(
        Bound(heimdall::SemanticRules::AnalyzeConstevalConstexpr, "extern const int kMax = 5;\n")
            .empty());
    EXPECT_TRUE(
        Bound(heimdall::SemanticRules::AnalyzeConstevalConstexpr, "const int kMax = Compute();\n")
            .empty());
    EXPECT_TRUE(Bound(heimdall::SemanticRules::AnalyzeConstevalConstexpr,
                      "void f() { const int local = 5; }\n")
                    .empty());
}

// ---- wiring ----------------------------------------------------------------------

TEST(ModernizeCxxSpec, NewCodesAreKnownAndConfigurable)
{
    for (const char* code :
         { "cpp/modernize-emplace", "cpp/modernize-make-unique", "cpp/modernize-make-shared",
           "cpp/modernize-smart-ptr", "cpp/no-new-delete", "cpp/modernize-span",
           "cpp/modernize-string-view", "cpp/modernize-algorithms",
           "cpp/modernize-structured-bindings", "cpp/modernize-attributes",
           "cpp/modernize-consteval-constexpr" })
    {
        EXPECT_TRUE(heimdall::IsKnownRuleCode(code)) << code;
    }

    heimdall::RuleOptions options;
    options.overrides.push_back({ "cpp/modernize-emplace", false, heimdall::Severity::Warning });
    EXPECT_TRUE(ByCode(heimdall::RuleEngine(options).Analyze("void f() { v.push_back(Foo(1)); }\n"),
                       "cpp/modernize-emplace")
                    .empty());
}
