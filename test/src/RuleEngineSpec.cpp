#include <gtest/gtest.h>

#include <Heimdall/RuleEngine.hpp>

#include <algorithm>
#include <string>

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

TEST(RuleEngineSpec, FindsEmptyCatchBlocksIncludingCommentOnlyBodies)
{
    constexpr std::string_view source =
        "void f() {\n"
        "    try { g(); } catch (...) {}\n"
        "    try { g(); } catch (const std::exception& e) { /* ignored */ }\n"
        "}\n";
    const auto diagnostics = heimdall::RuleEngine().Analyze(source);
    ASSERT_EQ(diagnostics.size(), 2);
    EXPECT_EQ(diagnostics[0].code, "cpp/no-empty-catch");
    EXPECT_EQ(diagnostics[0].line, 2);
    EXPECT_TRUE(diagnostics[0].has_fix);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(diagnostics[1].code, "cpp/no-empty-catch");
    EXPECT_EQ(diagnostics[1].line, 3);
}

TEST(RuleEngineSpec, DoesNotFlagNonEmptyOrIncompleteCatchHandlers)
{
    EXPECT_TRUE(heimdall::RuleEngine().Analyze(
        "void f() {\n"
        "    try { g(); } catch (...) { log(); }\n"
        "    try { g(); } catch (...) { throw; }\n"
        "}\n").empty());
    EXPECT_TRUE(heimdall::RuleEngine().Analyze("try { g(); } catch (...)\n").empty());
}

TEST(RuleEngineSpec, IgnoresCatchInCommentsStringsAndDirectives)
{
    constexpr std::string_view source =
        "// catch (...) {}\n"
        "const char* s = \"catch () {}\";\n"
        "#define CATCHALL catch (...) {}\n"
        "void f() { try { g(); } catch (...) { throw; } }\n";
    EXPECT_TRUE(heimdall::RuleEngine().Analyze(source).empty());
}

TEST(RuleEngineSpec, FindsDuplicateIncludesOutsideConditionals)
{
    constexpr std::string_view source =
        "#include <vector>\n"
        "#include \"app.h\"\n"
        "#include <vector>\n"
        "#include \"app.h\"\n"
        "#include <memory>\n";
    const auto diagnostics = heimdall::RuleEngine().Analyze(source);
    ASSERT_EQ(diagnostics.size(), 2);
    EXPECT_EQ(diagnostics[0].code, "cpp/no-duplicate-include");
    EXPECT_EQ(diagnostics[0].line, 3);
    EXPECT_TRUE(diagnostics[0].has_fix);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(diagnostics[1].code, "cpp/no-duplicate-include");
    EXPECT_EQ(diagnostics[1].line, 4);
}

TEST(RuleEngineSpec, DoesNotFlagDistinctConditionalOrMacroIncludes)
{
    constexpr std::string_view source =
        "#include <vector>\n"
        "#include \"vector\"\n"
        "#include <memory>\n"
        "#ifdef USE_FEATURE\n"
        "#include \"feature.h\"\n"
        "#else\n"
        "#include \"feature.h\"\n"
        "#endif\n"
        "#include HEADER_MACRO\n"
        "#include_next \"app.h\"\n";
    EXPECT_TRUE(heimdall::RuleEngine().Analyze(source).empty());
}

TEST(RuleEngineSpec, RewritesSimpleTypedefsWithUsing)
{
    constexpr std::string_view source =
        "typedef int Count;\n"
        "typedef unsigned long size_type;\n"
        "typedef std::vector<int> IntVector;\n"
        "typedef int * IntPtr;\n"
        "typedef int arr[10];\n";
    const auto diagnostics = heimdall::RuleEngine().Analyze(source);
    ASSERT_EQ(diagnostics.size(), 5);
    EXPECT_EQ(diagnostics[0].code, "cpp/modernize-using");
    EXPECT_EQ(diagnostics[0].line, 1);
    EXPECT_TRUE(diagnostics[0].has_fix);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics),
              "using Count = int;\n"
              "using size_type = unsigned long;\n"
              "using IntVector = std::vector<int>;\n"
              "using IntPtr = int *;\n"
              "using arr = int[10];\n");
}

TEST(RuleEngineSpec, RewritesTypedefsWithTemplateArguments)
{
    constexpr std::string_view source = "typedef std::map<int, int> IntMap;\n";
    const auto diagnostics = heimdall::RuleEngine().Analyze(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics),
              "using IntMap = std::map<int, int>;\n");
}

TEST(RuleEngineSpec, DoesNotRewriteComplexTypedefs)
{
    constexpr std::string_view source =
        "typedef void (*Callback)(int);\n"
        "typedef int a, b;\n"
        "typedef struct S { int x; } S;\n"
        "typedef int Fn();\n"
        "typedef int;\n"
        "typedef int X [[deprecated]];\n"
        "#define LEGACY typedef int T;\n";
    EXPECT_TRUE(heimdall::RuleEngine().Analyze(source).empty());
}

TEST(RuleEngineSpec, NewRulesCanBeDisabledIndependently)
{
    constexpr std::string_view source =
        "typedef int T;\n"
        "void f() { try { g(); } catch (...) {} }\n"
        "#include <vector>\n"
        "#include <vector>\n";
    heimdall::RuleOptions options;
    options.legacy_typedef = false;
    options.empty_catch = false;
    options.duplicate_include = false;
    EXPECT_TRUE(heimdall::RuleEngine(options).Analyze(source).empty());

    heimdall::RuleOptions only_typedef = options;
    only_typedef.legacy_typedef = true;
    const auto diagnostics = heimdall::RuleEngine(only_typedef).Analyze(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].code, "cpp/modernize-using");
}

TEST(RuleEngineSpec, NewRulesHonorSeverityAndDisableOverrides)
{
    heimdall::RuleOptions options;
    options.overrides.push_back({"cpp/no-empty-catch", true, heimdall::Severity::Error});
    auto diagnostics = heimdall::RuleEngine(options).Analyze(
        "void f() { try { g(); } catch (...) {} }\n");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].severity, heimdall::Severity::Error);

    options.overrides[0].enabled = false;
    EXPECT_TRUE(heimdall::RuleEngine(options).Analyze(
        "void f() { try { g(); } catch (...) {} }\n").empty());
}

TEST(RuleEngineSpec, SuppressionsCoverNewRules)
{
    constexpr std::string_view source =
        "typedef int T; // heimdall-disable-line cpp/modernize-using\n"
        "void f() { try { g(); } catch (...) {} } // heimdall-disable-line cpp/no-empty-catch\n"
        "#include <vector>\n"
        "#include <vector> // heimdall-disable-line cpp/no-duplicate-include\n";
    EXPECT_TRUE(heimdall::RuleEngine().Analyze(source).empty());
}

namespace
{
    // Applies every fix, safe or not, the way an editor would when the user
    // accepts each quick fix one by one.
    std::string ApplyQuickFixes(std::string_view source)
    {
        std::string text(source);
        for (int guard = 0; guard < 16; ++guard)
        {
            auto diagnostics = heimdall::RuleEngine().Analyze(text);
            auto it = std::find_if(diagnostics.begin(), diagnostics.end(),
                [](const heimdall::Diagnostic &d) { return d.has_fix; });
            if (it == diagnostics.end())
            {
                break;
            }

            text.replace(it->fix.offset, it->fix.length, it->fix.replacement);
        }

        return text;
    }
}

TEST(RuleEngineSpec, EmptyCatchQuickFixRethrowsButIsNotAppliedInBatch)
{
    constexpr std::string_view source = "void f() { try { g(); } catch (...) {} }\n";
    const auto diagnostics = heimdall::RuleEngine().Analyze(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_FALSE(diagnostics[0].fix_title.empty());
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics), source);
    EXPECT_EQ(ApplyQuickFixes(source), "void f() { try { g(); } catch (...) { throw; } }\n");
}

TEST(RuleEngineSpec, EmptyCatchQuickFixKeepsComments)
{
    EXPECT_EQ(ApplyQuickFixes("try { g(); } catch (...) { /* ignored */ }\n"),
        "try { g(); } catch (...) { /* ignored */ throw; }\n");
    EXPECT_EQ(ApplyQuickFixes("try { g(); } catch (...) {\n}\n"),
        "try { g(); } catch (...) { throw; }\n");
}

TEST(RuleEngineSpec, DuplicateIncludeQuickFixRemovesTheWholeLine)
{
    constexpr std::string_view source =
        "#include <vector>\n"
        "#include <memory>\n"
        "#  include <vector>\n"
        "int x;\n";
    const auto diagnostics = heimdall::RuleEngine().Analyze(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics), source);
    EXPECT_EQ(ApplyQuickFixes(source), "#include <vector>\n#include <memory>\nint x;\n");
    EXPECT_EQ(ApplyQuickFixes("#include <a>\r\n#include <a>\r\nint x;\r\n"), "#include <a>\r\nint x;\r\n");
    EXPECT_EQ(ApplyQuickFixes("#include <a>\n#include <a>"), "#include <a>\n");
}

TEST(RuleEngineSpec, DuplicateIncludeQuickFixDropsTrailingComment)
{
    EXPECT_EQ(ApplyQuickFixes("#include <a>\n#include <a> // again\n"), "#include <a>\n");
}

TEST(RuleEngineSpec, SafeFixesStayMarkedSafe)
{
    const auto diagnostics = heimdall::RuleEngine().Analyze("typedef int T;\nvoid* p = NULL;\n");
    ASSERT_EQ(diagnostics.size(), 2);
    for (const auto & diagnostic: diagnostics)
    {
        EXPECT_TRUE(diagnostic.fix_is_safe);
    }
}

TEST(RuleEngineSpec, CatalogDescribesEveryRule)
{
    const auto & catalog = heimdall::RuleCatalog();
    ASSERT_EQ(catalog.size(), 7);
    for (const auto & info: catalog)
    {
        EXPECT_FALSE(info.code.empty());
        EXPECT_FALSE(info.category.empty());
        EXPECT_FALSE(info.layer.empty());
        EXPECT_FALSE(info.summary.empty());
        ASSERT_NE(heimdall::FindRuleByCode(info.code), nullptr);
        EXPECT_EQ(heimdall::FindRuleByCode(info.code)->id, info.id);
        ASSERT_NE(heimdall::FindRule(info.id), nullptr);
        EXPECT_EQ(heimdall::FindRule(info.id)->code, info.code);
        EXPECT_TRUE(heimdall::IsKnownRuleCode(info.code));
    }

    EXPECT_FALSE(heimdall::IsKnownRuleCode("cpp/not-real"));
    EXPECT_EQ(heimdall::FindRuleByCode("cpp/no-null")->autofix, true);
    EXPECT_EQ(heimdall::FindRuleByCode("cpp/no-empty-catch")->autofix, false);
    EXPECT_EQ(heimdall::FindRuleByCode("cpp/no-duplicate-include")->autofix, false);
    EXPECT_EQ(heimdall::FindRuleByCode("cpp/modernize-using")->autofix, true);
    EXPECT_EQ(heimdall::FindRuleByCode("cpp/modernize-using")->layer, "sintática");
}

TEST(RuleEngineSpec, RemoveDirectiveLineTakesIndentationAndLineTerminator)
{
    constexpr std::string_view source = "int a;\n  #include <x>  \r\nint b;\n";
    const auto edit = heimdall::RemoveDirectiveLine(source, source.find('#'), std::string_view("#include <x>").size());
    std::string text(source);
    text.replace(edit.offset, edit.length, edit.replacement);
    EXPECT_EQ(text, "int a;\nint b;\n");

    const auto last = heimdall::RemoveDirectiveLine("#include <x>", 0, 12);
    EXPECT_EQ(last.offset, 0);
    EXPECT_EQ(last.length, 12);
}

TEST(RuleEngineSpec, ApplyPolicyFiltersExternalDiagnosticsAndSortsThem)
{
    heimdall::RuleOptions options;
    options.overrides.push_back({"cpp/no-unused-include", false, heimdall::Severity::Warning});
    const auto tree = heimdall::ParseTree::Parse("int a;\n", {});
    heimdall::Diagnostic late{heimdall::RuleId::UnusedInclude, heimdall::Severity::Warning, "cpp/no-unused-include",
        "m", 5, 1, 1, 6, false, {}};
    heimdall::Diagnostic early{heimdall::RuleId::NullMacro, heimdall::Severity::Warning, "cpp/no-null", "m", 1, 1,
        1, 2, false, {}};
    const auto kept = heimdall::RuleEngine().ApplyPolicy({late, early}, tree);
    ASSERT_EQ(kept.size(), 2);
    EXPECT_EQ(kept[0].code, "cpp/no-null");
    EXPECT_EQ(heimdall::RuleEngine(options).ApplyPolicy({late, early}, tree).size(), 1);
}
