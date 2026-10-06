#include <gtest/gtest.h>

#include <Heimdall/CompileDatabase.hpp>
#include <Heimdall/SemanticAnalyzer.hpp>

TEST(SemanticAnalyzerSpec, UsesLocalTypeNamesToResolveAsteriskAmbiguity)
{
    constexpr std::string_view source = "struct A {};\nusing Alias = A;\n";
    const heimdall::SemanticAnalyzer analyzer;
    const auto types = analyzer.CollectTypeNames(source);
    EXPECT_EQ(analyzer.ClassifyAsteriskStatement("A * b;", types),
        heimdall::AsteriskMeaning::Declaration);
    EXPECT_EQ(analyzer.ClassifyAsteriskStatement("unknown * b;", types),
        heimdall::AsteriskMeaning::Ambiguous);
    EXPECT_EQ(analyzer.ClassifyAsteriskStatement("value * result;", types, {"value"}),
        heimdall::AsteriskMeaning::Multiplication);
    EXPECT_EQ(analyzer.ClassifyAsteriskStatement("int * b;", types),
        heimdall::AsteriskMeaning::Declaration);
}

TEST(SemanticAnalyzerSpec, FindsUnusedSimpleLocalsOnlyInsideFunctionBodies)
{
    constexpr std::string_view source =
        "int global_value;\n"
    "struct Holder { int field; };\n"
    "void f() {\n"
    "  int unused = 1;\n"
    "  int used = 2;\n"
    "  consume(used);\n"
    "}\n";
    const heimdall::SemanticAnalyzer analyzer;
    const auto diagnostics = analyzer.AnalyzeUnusedLocals(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].code, "semantic/no-unused-local");
    EXPECT_EQ(diagnostics[0].message, "local variable 'unused' is never used");
    EXPECT_EQ(diagnostics[0].line, 4);
}

TEST(SemanticAnalyzerSpec, HonorsConditionalBranchesAndCompileCommandDefines)
{
    constexpr std::string_view source =
        "#ifdef FEATURE\n"
    "void enabled() { int active_unused; }\n"
    "#else\n"
    "void disabled() { int inactive_unused; }\n"
    "#endif\n";
    heimdall::CompileCommand command;
    command.defines.emplace("FEATURE", "1");
    const auto diagnostics = heimdall::SemanticAnalyzer().AnalyzeUnusedLocals(source, &command);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].message, "local variable 'active_unused' is never used");
}
