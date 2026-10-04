#include <gtest/gtest.h>

#include <Heimdall/RuleEngine.hpp>

TEST(RuleEngineSpec, FindsNullMacroOutsideCommentsLiteralsAndDirectives)
{
    constexpr std::string_view source =
        "#define TEXT NULL\n"
        "void f() { auto p = NULL; } // NULL\n"
        "const char* s = \"NULL\";\n";
    const auto diagnostics = heimdall::RuleEngine().Analyze(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].code, "cpp/no-null");
    EXPECT_EQ(diagnostics[0].line, 2);
    EXPECT_EQ(diagnostics[0].column, 21);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics),
              "#define TEXT NULL\nvoid f() { auto p = nullptr; } // NULL\nconst char* s = \"NULL\";\n");
}

TEST(RuleEngineSpec, FindsAndFixesTrailingSpacesAndTabsIncludingCrLf)
{
    constexpr std::string_view source = "first  \r\nsecond\t\nclean\n";
    const auto diagnostics = heimdall::RuleEngine().Analyze(source);
    ASSERT_EQ(diagnostics.size(), 2);
    EXPECT_EQ(diagnostics[0].code, "format/no-trailing-whitespace");
    EXPECT_EQ(diagnostics[0].line, 1);
    EXPECT_EQ(diagnostics[1].line, 2);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics), "first\r\nsecond\nclean\n");
}

TEST(RuleEngineSpec, CanDisableRulesIndependently)
{
    const auto diagnostics = heimdall::RuleEngine({ .null_macro = false, .trailing_whitespace = true })
                                 .Analyze("auto p = NULL;  \n");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::TrailingWhitespace);
}

TEST(RuleEngineSpec, AddsMissingFinalNewlineAndPreservesLineEndingStyle)
{
    const auto lf = heimdall::RuleEngine().Analyze("int value;");
    ASSERT_EQ(lf.size(), 1);
    EXPECT_EQ(lf[0].code, "format/require-final-newline");
    EXPECT_TRUE(lf[0].has_fix);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes("int value;", lf), "int value;\n");

    const auto crlf = heimdall::RuleEngine().Analyze("int value;\r\nint other;");
    ASSERT_EQ(crlf.size(), 1);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes("int value;\r\nint other;", crlf),
              "int value;\r\nint other;\r\n");
}

TEST(RuleEngineSpec, DoesNotFlagEmptyFilesOrFilesAlreadyEndingInNewline)
{
    EXPECT_TRUE(heimdall::RuleEngine().Analyze("").empty());
    EXPECT_TRUE(heimdall::RuleEngine().Analyze("int value;\n").empty());
}

TEST(RuleEngineSpec, SupportsPerRuleSeverityAndDisableOverrides)
{
    heimdall::RuleOptions options;
    options.final_newline = false;
    options.overrides.push_back({"cpp/no-null", true, heimdall::Severity::Error});
    auto diagnostics = heimdall::RuleEngine(options).Analyze("auto p = NULL;\n");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].severity, heimdall::Severity::Error);

    options.overrides[0].enabled = false;
    EXPECT_TRUE(heimdall::RuleEngine(options).Analyze("auto p = NULL;\n").empty());
}

TEST(RuleEngineSpec, SupportsInlineAndNextLineRuleSuppressions)
{
    constexpr std::string_view source =
        "auto first = NULL; // heimdall-disable-line cpp/no-null\n"
        "// heimdall-disable-next-line cpp/no-null\n"
        "auto second = NULL;\n"
        "auto third = NULL;\n";
    const auto diagnostics = heimdall::RuleEngine().Analyze(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].line, 4);
}

TEST(RuleEngineSpec, ApplyFixesRejectsEditsOutsideTheirDiagnosticRange)
{
    heimdall::Diagnostic diagnostic{
        heimdall::RuleId::NullMacro, heimdall::Severity::Warning, "cpp/no-null", "replace null macro",
        0, 4, 1, 1, true, {5, 1, "x"}
    };
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes("NULL;", {diagnostic}), "NULL;");
}
