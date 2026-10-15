#include <algorithm>
#include <gtest/gtest.h>
#include <Heimdall/AnalysisFeatures.hpp>
#include <memory>
#include <string>
#include <vector>

namespace
{
    [[nodiscard]] std::vector<heimdall::Diagnostic> AnalyzeUnreachable(const std::string& source)
    {
        heimdall::Workspace workspace;
        const auto file = workspace.Open("unreachable.cpp",
            std::make_shared<const std::string>(source));
        heimdall::AnalysisContext context(workspace.Snapshot(), *file);
        heimdall::RuleOptions options;
        options.overrides.push_back({"cpp/unreachable-code", true, heimdall::Severity::Warning});
        options.overrides.push_back({"cpp/missing-return", true, heimdall::Severity::Error});
        options.overrides.push_back({"cpp/return-type", true, heimdall::Severity::Error});
        auto diagnostics = heimdall::AnalysisFeatures::Diagnostics(
            context, heimdall::RuleEngine(std::move(options)), true);
        std::erase_if(diagnostics,[](const auto& diagnostic)
            {
                return diagnostic.code != "cpp/unreachable-code";
            });
        return diagnostics;
    }

    void ExpectUnreachableAt(const std::string& source, const std::string& statement)
    {
        const auto diagnostics = AnalyzeUnreachable(source);

        ASSERT_FALSE(diagnostics.empty());
        EXPECT_EQ(diagnostics.front().offset, source.find(statement));
        EXPECT_GT(diagnostics.front().length, 0u);
        EXPECT_EQ(diagnostics.front().severity, heimdall::Severity::Warning);
        EXPECT_FALSE(diagnostics.front().has_fix);
    }
}

TEST(UnreachableCodeSpec, ReportsDeclarationAndIncrementAfterVoidReturn)
{
    const std::string source =
        "void unreacheable_code() {\n"
        "    return;\n"
        "\n"
        "    auto unreacheable_variable = 10;\n"
        "    unreacheable_variable++;\n"
        "}\n";

    ExpectUnreachableAt(source, "auto unreacheable_variable");
}

TEST(UnreachableCodeSpec, ReportsStatementsAfterValueReturnAndNestedReturn)
{
    ExpectUnreachableAt("int f() { return 1; int dead = 2; return dead; }", "int dead");
    ExpectUnreachableAt("void f() { { return; } int dead = 1; }", "int dead");
    ExpectUnreachableAt("void f() { return; { int dead = 1; } }", "int dead");
}

TEST(UnreachableCodeSpec, ReportsCodeAfterBothIfBranchesReturn)
{
    ExpectUnreachableAt("void f(bool condition) { if (condition) { return; } "
        "else { return; } int dead = 1; }", "int dead");
}

TEST(UnreachableCodeSpec, KeepsCodeReachableWhenOnlyOneBranchReturns)
{
    EXPECT_TRUE(AnalyzeUnreachable("void f(bool condition) { if (condition) { return; } "
        "int live = 1; live++; }").empty());
    EXPECT_TRUE(AnalyzeUnreachable("void f(bool condition) { if (condition) { return; } "
        "else { int live = 1; live++; } }").empty());
}

TEST(UnreachableCodeSpec, ReportsStatementsAfterThrowButKeepsCatchReachable)
{
    ExpectUnreachableAt("void f() { throw 1; int dead = 2; }", "int dead");
    EXPECT_TRUE(AnalyzeUnreachable("void f() { try { throw 1; } "
        "catch (...) { int live = 2; live++; } "
        "int afterCatch = 3; afterCatch++; }").empty());
}

TEST(UnreachableCodeSpec, ReportsStatementsAfterBreakAndContinueInsideLoop)
{
    ExpectUnreachableAt("void f() { for (;;) { break; int dead = 1; } }", "int dead");
    ExpectUnreachableAt("void f(bool condition) { while (condition) "
        "{ continue; int dead = 1; } }", "int dead");
}

TEST(UnreachableCodeSpec, KeepsCodeAfterLoopWithBreakReachable)
{
    EXPECT_TRUE(AnalyzeUnreachable("void f() { for (;;) { break; } "
        "int live = 1; live++; }").empty());
    EXPECT_TRUE(AnalyzeUnreachable("void f(bool condition) { while (condition) { continue; } "
        "int live = 1; live++; }").empty());
}

TEST(UnreachableCodeSpec, ReportsCodeAfterInfiniteLoopsWithoutBreak)
{
    ExpectUnreachableAt("void f() { for (;;) {} int dead = 1; }", "int dead");
    ExpectUnreachableAt("void f() { while (true) {} int dead = 1; }", "int dead");
}

TEST(UnreachableCodeSpec, KeepsSwitchCasesAndFallthroughReachable)
{
    EXPECT_TRUE(AnalyzeUnreachable("void f(int value) { switch (value) { "
        "case 0: return; case 1: value++; break; default: value--; } "
        "value++; }").empty());
    EXPECT_TRUE(AnalyzeUnreachable("void f(int value) { switch (value) { "
        "case 0: value++; [[fallthrough]]; case 1: value++; break; } "
        "value++; }").empty());
}

TEST(UnreachableCodeSpec, KeepsGotoTargetsReachableAndAvoidsIncompleteFlowClaims)
{
    EXPECT_TRUE(AnalyzeUnreachable("void f(bool condition) { if (condition) goto target; "
        "return; target: int live = 1; live++; }").empty());
}

TEST(UnreachableCodeSpec, AnalyzesLambdaReturnIndependentlyOfEnclosingFunction)
{
    EXPECT_TRUE(AnalyzeUnreachable("void f() { auto callback = [] { return; }; "
        "callback(); int live = 1; live++; }").empty());
    ExpectUnreachableAt("void f() { auto callback = [] { return; int dead = 1; }; "
        "callback(); }", "int dead");
}

TEST(UnreachableCodeSpec, IgnoresInactiveCodeAndUnknownCalleeTermination)
{
    EXPECT_TRUE(AnalyzeUnreachable("void f() {\n#if 0\nreturn;\n#endif\n"
        "int live = 1; live++; }\n").empty());
    EXPECT_TRUE(AnalyzeUnreachable("void external(); void f() { external(); "
        "int live = 1; live++; }").empty());
    EXPECT_TRUE(AnalyzeUnreachable("void f() { return; /* no statement follows */ }").empty());
    EXPECT_TRUE(AnalyzeUnreachable("void f() {").empty());
}
