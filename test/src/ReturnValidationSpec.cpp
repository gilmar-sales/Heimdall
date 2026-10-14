#include <algorithm>
#include <gtest/gtest.h>
#include <Heimdall/AnalysisFeatures.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    [[nodiscard]] std::vector<heimdall::Diagnostic> AnalyzeReturns(const std::string& source)
    {
        heimdall::Workspace workspace;
        const auto file = workspace.Open("returns.cpp", std::make_shared<const std::string>(source));
        heimdall::AnalysisContext context(workspace.Snapshot(), *file);
        auto diagnostics = heimdall::AnalysisFeatures::Diagnostics(
            context, heimdall::RuleEngine {}, true);
        std::erase_if(diagnostics,[](const auto& diagnostic)
            {
                return diagnostic.code != "cpp/missing-return" && diagnostic.code != "cpp/return-type";
        });
        return diagnostics;
    }

    void ExpectReturnError(const std::string& source, std::string_view code)
    {
        const auto diagnostics = AnalyzeReturns(source);

        ASSERT_FALSE(diagnostics.empty());
        const auto matching = std::find_if(diagnostics.begin(), diagnostics.end(),
            [code](const auto& diagnostic)
            {
                return diagnostic.code == code;
        });
        ASSERT_NE(matching, diagnostics.end());
        EXPECT_EQ(matching->severity, heimdall::Severity::Error);
        EXPECT_FALSE(matching->has_fix);
    }
}

TEST(ReturnValidationSpec, ReportsBareReturnAndFallthroughInTheSameNonVoidFunction)
{
    const std::string source =
        "int test_method(int a, int b, std::string desc)\n"
        "{\n"
        "    auto value = a + b;\n"
        "\n"
        "    if (value > 10)\n"
        "        return;\n"
        "}\n";

    const auto diagnostics = AnalyzeReturns(source);

    ASSERT_EQ(diagnostics.size(), 2u);
    const auto bare = std::find_if(diagnostics.begin(), diagnostics.end(),[](const auto& diagnostic)
        {
            return diagnostic.code == "cpp/return-type";
    });
    ASSERT_NE(bare, diagnostics.end());
    EXPECT_EQ(bare->offset, source.find("return;"));
    EXPECT_NE(bare->message.find("int"), std::string::npos);
    EXPECT_EQ(bare->severity, heimdall::Severity::Error);
    EXPECT_TRUE(std::any_of(diagnostics.begin(), diagnostics.end(),[](const auto& diagnostic)
        {
            return diagnostic.code == "cpp/missing-return" &&
            diagnostic.severity == heimdall::Severity::Error;
    }));
}

TEST(ReturnValidationSpec, ReportsNonVoidFunctionWithNoReturn)
{
    ExpectReturnError("int Calculate(int a, int b) { auto value = a + b; }",
                      "cpp/missing-return");
    ExpectReturnError("int Empty() {}", "cpp/missing-return");
    ExpectReturnError("auto Empty() -> int {}", "cpp/missing-return");
}

TEST(ReturnValidationSpec, ReportsBareReturnWithoutInventingFallthrough)
{
    const auto diagnostics = AnalyzeReturns("int Calculate() { return; }");

    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics.front().code, "cpp/return-type");
    EXPECT_EQ(diagnostics.front().severity, heimdall::Severity::Error);
}

TEST(ReturnValidationSpec, AcceptsAllIfBranchesReturningCompatibleValues)
{
    EXPECT_TRUE(AnalyzeReturns("int f(bool c) { if (c) return 1; else return 2; }").empty());
    EXPECT_TRUE(AnalyzeReturns("int f(int n) { if (n > 0) return 1; "
        "else if (n < 0) return -1; else return 0; }").empty());
    EXPECT_TRUE(AnalyzeReturns("int f(bool c) { if (c) return 1; return 2; }").empty());
}

TEST(ReturnValidationSpec, ReportsMissingReturnInOneBranchAndNestedBranches)
{
    ExpectReturnError("int f(bool c) { if (c) return 1; }", "cpp/missing-return");
    ExpectReturnError("int f(bool c) { if (c) return 1; else { int value = 2; } }",
                      "cpp/missing-return");
    ExpectReturnError("int f(bool a, bool b) { if (a) { if (b) return 1; } else return 2; }",
                      "cpp/missing-return");
}

TEST(ReturnValidationSpec, HandlesSwitchDefaultAndFallthrough)
{
    EXPECT_TRUE(AnalyzeReturns("int f(int n) { switch (n) { "
        "case 0: return 1; default: return 2; } }").empty());
    EXPECT_TRUE(AnalyzeReturns("int f(int n) { switch (n) { "
        "case 0: [[fallthrough]]; case 1: return 1; default: return 2; } }")
        .empty());
    ExpectReturnError("int f(int n) { switch (n) { case 0: return 1; case 1: return 2; } }",
                      "cpp/missing-return");
    ExpectReturnError("int f(int n) { switch (n) { case 0: return 1; default: break; } }",
                      "cpp/missing-return");
}

TEST(ReturnValidationSpec, AcceptsNonTerminatingLoopsButChecksReachableBreaks)
{
    EXPECT_TRUE(AnalyzeReturns("int f() { for (;;) {} }").empty());
    EXPECT_TRUE(AnalyzeReturns("int f() { while (true) {} }").empty());
    ExpectReturnError("int f(bool c) { for (;;) { if (c) break; } }", "cpp/missing-return");
    ExpectReturnError("int f(bool c) { while (c) { return 1; } }", "cpp/missing-return");
    EXPECT_TRUE(AnalyzeReturns("int f() { do { return 1; } while (false); }").empty());
}

TEST(ReturnValidationSpec, AccountsForKnownConstantConditions)
{
    EXPECT_TRUE(AnalyzeReturns("int f() { if (true) return 1; }").empty());
    ExpectReturnError("int f() { if (false) return 1; }", "cpp/missing-return");
}

TEST(ReturnValidationSpec, AcceptsThrowAndChecksCatchPaths)
{
    EXPECT_TRUE(AnalyzeReturns("int f() { throw 1; }").empty());
    EXPECT_TRUE(AnalyzeReturns("int f(bool c) { if (c) return 1; else throw 2; }").empty());
    EXPECT_TRUE(AnalyzeReturns("int f() { try { return 1; } catch (...) { return 2; } }")
        .empty());
    ExpectReturnError("int f() { try { return 1; } catch (...) {} }", "cpp/missing-return");
}

TEST(ReturnValidationSpec, AcceptsKnownNoreturnCallsWithoutGuessingUnknownCallees)
{
    EXPECT_TRUE(AnalyzeReturns("[[noreturn]] void Stop(); int f() { Stop(); }").empty());
    ExpectReturnError("void External(); int f() { External(); }", "cpp/missing-return");
    ExpectReturnError("[[noreturn]] void Stop(int); void Stop(double); "
        "int f() { Stop(1.5); }", "cpp/missing-return");
}

TEST(ReturnValidationSpec, ExemptsMainVoidConstructorsAndDestructors)
{
    EXPECT_TRUE(AnalyzeReturns("int main() {}").empty());
    EXPECT_TRUE(AnalyzeReturns("void f() {}").empty());
    EXPECT_TRUE(AnalyzeReturns("void f() { return; }").empty());
    EXPECT_TRUE(AnalyzeReturns("class S { public: S() {} ~S() {} };").empty());
    EXPECT_TRUE(AnalyzeReturns("void h() {} void f() { return h(); }").empty());
}

TEST(ReturnValidationSpec, RejectsValueReturnedFromVoidFunction)
{
    ExpectReturnError("void f() { return 1; }", "cpp/return-type");
}

TEST(ReturnValidationSpec, RejectsIncompatibleReturnTypesAndAcceptsImplicitConversions)
{
    ExpectReturnError("std::string f() { return 1; }", "cpp/return-type");
    ExpectReturnError("int f() { return \"text\"; }", "cpp/return-type");
    EXPECT_TRUE(AnalyzeReturns("double f() { return 1; }").empty());
    EXPECT_TRUE(AnalyzeReturns("int f() { return 1.5; }").empty());
    EXPECT_TRUE(AnalyzeReturns("std::string f() { return \"text\"; }").empty());
    EXPECT_TRUE(AnalyzeReturns("double f(int n = [] { auto value = 1; return value; }()) "
        "{ return n; }").empty());
}

TEST(ReturnValidationSpec, ValidatesPointerReturnsAndQualification)
{
    EXPECT_TRUE(AnalyzeReturns("int* f() { return nullptr; }").empty());
    EXPECT_TRUE(AnalyzeReturns("int* f() { return 0; }").empty());
    EXPECT_TRUE(AnalyzeReturns("const int* f(int* p) { return p; }").empty());
    ExpectReturnError("int* f() { return 1; }", "cpp/return-type");
    ExpectReturnError("int* f(const int* p) { return p; }", "cpp/return-type");
}

TEST(ReturnValidationSpec, ValidatesNonConstReferenceBinding)
{
    EXPECT_TRUE(AnalyzeReturns("int& f(int& value) { return value; }").empty());
    ExpectReturnError("int& f() { return 1; }", "cpp/return-type");
    ExpectReturnError("int& f(const int& value) { return value; }", "cpp/return-type");
}

TEST(ReturnValidationSpec, ChecksDeducedReturnConsistencyButAllowsDeducedVoid)
{
    EXPECT_TRUE(AnalyzeReturns("auto f() {}").empty());
    EXPECT_TRUE(AnalyzeReturns("auto f() { return; }").empty());
    EXPECT_TRUE(AnalyzeReturns("auto f(bool c) { if (c) return 1; return 2; }").empty());
    ExpectReturnError("auto f(bool c) { if (c) return 1; return 2.5; }", "cpp/return-type");
    ExpectReturnError("auto f(bool c) { if (c) return 1; return; }", "cpp/return-type");
    ExpectReturnError("auto f(bool c) { if (c) return 1; }", "cpp/missing-return");
}

TEST(ReturnValidationSpec, AcceptsStatementMacroListFollowedByReturn)
{
    // Regression for `tok_detail::BuildTable` in Tok.hpp: a `;`-less
    // statement-macro invocation followed by `#undef` and `return` keeps its
    // return statement, so no `cpp/missing-return` is reported for it.
    // `Broken` proves the rule still runs in the same file.
    const std::string source =
        "#define LIST(X) X(a, \"a\") X(b, \"b\")\n"
        "#define APPLY(name, text) consume(text);\n"
        "void consume(const char*);\n"
        "int Build()\n"
        "{\n"
        "    LIST(APPLY)\n"
        "#undef APPLY\n"
        "    return 1;\n"
        "}\n"
        "int Broken(bool flag)\n"
        "{\n"
        "    if (flag)\n"
        "        return 1;\n"
        "}\n";

    const auto diagnostics = AnalyzeReturns(source);

    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics.front().code, "cpp/missing-return");
    EXPECT_GE(diagnostics.front().offset, source.find("int Broken"));
}

TEST(ReturnValidationSpec, DoesNotCountLambdaReturnsAsEnclosingFunctionReturns)
{
    ExpectReturnError("int f() { auto callback = [] { return 1; }; callback(); }",
                      "cpp/missing-return");
    EXPECT_TRUE(AnalyzeReturns("int f() { auto callback = [] { return 1; }; "
        "return callback(); }").empty());
    ExpectReturnError("void f() { auto callback = [](bool c) -> int "
        "{ if (c) return 1; }; callback(true); }", "cpp/missing-return");
}

TEST(ReturnValidationSpec, ChecksInvalidReturnTypesEvenInUnreachableCode)
{
    ExpectReturnError("int f() { return 1; return; }", "cpp/return-type");
}

TEST(ReturnValidationSpec, IgnoresDeclarationsInactiveCodeAndUnknownConversions)
{
    EXPECT_TRUE(AnalyzeReturns("int f(); extern int g();").empty());
    EXPECT_TRUE(AnalyzeReturns("int f() {\n#if 0\nreturn;\n#endif\nreturn 1; }\n").empty());
    EXPECT_TRUE(AnalyzeReturns("struct S { S(int); }; S f() { return 1; }").empty());
    EXPECT_TRUE(AnalyzeReturns("template<class T> T f(T value) { return value; }").empty());
    EXPECT_TRUE(AnalyzeReturns("int f() { return Unknown(); }").empty());
    EXPECT_TRUE(AnalyzeReturns("int f() {").empty());
}
