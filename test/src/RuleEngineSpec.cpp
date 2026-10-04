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
    ASSERT_EQ(diagnostics.size(), 6);
    EXPECT_EQ(diagnostics[0].code, "cpp/modernize-using");
    EXPECT_EQ(diagnostics[0].line, 1);
    EXPECT_TRUE(diagnostics[0].has_fix);
    ASSERT_EQ(diagnostics[5].code, "cpp/no-magic-numbers");
    EXPECT_EQ(diagnostics[5].line, 5);
    EXPECT_FALSE(diagnostics[5].has_fix);
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
        "#include <vector>\n"
        "// TODO: remove this\n"
        "int timeout = 30;\n";
    heimdall::RuleOptions options;
    options.legacy_typedef = false;
    options.empty_catch = false;
    options.duplicate_include = false;
    options.todo_comment = false;
    options.magic_numbers = false;
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

TEST(RuleEngineSpec, FindsTodoMarkersInLineAndBlockComments)
{
    constexpr std::string_view source =
        "// TODO: refactor this\n"
        "int x = 0; /* FIXME urgent */\n"
        "// XXX workaround\n";
    const auto diagnostics = heimdall::RuleEngine().Analyze(source);
    ASSERT_EQ(diagnostics.size(), 3);
    EXPECT_EQ(diagnostics[0].code, "cpp/no-todo");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::TodoComment);
    EXPECT_EQ(diagnostics[0].line, 1);
    EXPECT_FALSE(diagnostics[0].has_fix);
    EXPECT_EQ(diagnostics[1].line, 2);
    EXPECT_EQ(diagnostics[2].line, 3);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics), source);
}

TEST(RuleEngineSpec, IgnoresTodoSubstringsProseAndCode)
{
    constexpr std::string_view source =
        "// todo lowercase stays quiet\n"
        "// todolist is one word\n"
        "// TODOs is one word\n"
        "// a method to fix things\n"
        "const char* s = \"TODO\";\n"
        "int todo_count = 0;\n";
    EXPECT_TRUE(heimdall::RuleEngine().Analyze(source).empty());
}

TEST(RuleEngineSpec, FlagsMagicNumbersOutsideDirectives)
{
    constexpr std::string_view source =
        "#define LIMIT 42\n"
        "int retry = 3;\n"
        "double ratio = 0.5;\n"
        "const char* s = \"42\";\n"
        "// 42 in a comment\n";
    const auto diagnostics = heimdall::RuleEngine().Analyze(source);
    ASSERT_EQ(diagnostics.size(), 2);
    EXPECT_EQ(diagnostics[0].code, "cpp/no-magic-numbers");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::MagicNumber);
    EXPECT_EQ(diagnostics[0].line, 2);
    EXPECT_FALSE(diagnostics[0].has_fix);
    EXPECT_EQ(diagnostics[1].line, 3);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics), source);
}

TEST(RuleEngineSpec, AllowsTrivialZeroAndOneSpellings)
{
    constexpr std::string_view source =
        "int a = 0;\n"
        "int b = 1;\n"
        "unsigned c = 1u;\n"
        "long d = 0L;\n"
        "double e = 0.0;\n"
        "float f = 1.0f;\n"
        "int g = 0x0;\n"
        "int h = 0b1;\n";
    EXPECT_TRUE(heimdall::RuleEngine().Analyze(source).empty());
}

TEST(RuleEngineSpec, DoesNotFlagValuesThatNameAConstant)
{
    constexpr std::string_view source =
        "constexpr std::size_t kTableSize = 512; // power of two, load factor < 0.4\n"
        "const int kMax = 100;\n"
        "constexpr double kHalf{0.5};\n"
        "constexpr int kNeg = -42;\n"
        "constexpr int kParen = (64);\n"
        "enum Kind { A = 3, B };\n"
        "int plain = 7;\n"
        "const int mixed = 8 + 1;\n";
    const auto diagnostics = heimdall::RuleEngine().Analyze(source);
    ASSERT_EQ(diagnostics.size(), 2);
    EXPECT_EQ(diagnostics[0].code, "cpp/no-magic-numbers");
    EXPECT_EQ(diagnostics[0].line, 7);
    EXPECT_EQ(diagnostics[1].line, 8);
}

TEST(RuleEngineSpec, NewTodoAndMagicRulesHonorOverridesAndSuppressions)
{
    heimdall::RuleOptions options;
    options.overrides.push_back({"cpp/no-todo", true, heimdall::Severity::Error});
    auto diagnostics = heimdall::RuleEngine(options).Analyze("// TODO: x\n");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].severity, heimdall::Severity::Error);

    options.overrides[0].enabled = false;
    EXPECT_TRUE(heimdall::RuleEngine(options).Analyze("// TODO: x\n").empty());

    EXPECT_TRUE(heimdall::RuleEngine().Analyze(
        "// TODO: x // heimdall-disable-line cpp/no-todo\n").empty());
    EXPECT_TRUE(heimdall::RuleEngine().Analyze(
        "int timeout = 30; // heimdall-disable-line cpp/no-magic-numbers\n").empty());
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
    ASSERT_EQ(catalog.size(), 21);
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
    EXPECT_EQ(heimdall::FindRuleByCode("cpp/sort-includes")->autofix, true);
    EXPECT_EQ(heimdall::FindRuleByCode("cpp/sort-includes")->layer, "diretivas");
    EXPECT_EQ(heimdall::FindRuleByCode("cpp/modernize-override")->autofix, true);
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

TEST(RuleEngineSpec, SortIncludesIsDisabledByDefault)
{
    EXPECT_TRUE(heimdall::RuleEngine().Analyze(
        "#include <vector>\n#include <map>\n").empty());
}

TEST(RuleEngineSpec, SortsIncludesWithinAdjacentBlocks)
{
    constexpr std::string_view source =
        "#include <vector>\n"
        "#include <map>\n"
        "#include <algorithm>\n";
    heimdall::RuleOptions options;
    options.sort_includes = true;
    const auto diagnostics = heimdall::RuleEngine(options).Analyze(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].code, "cpp/sort-includes");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::UnsortedIncludes);
    EXPECT_EQ(diagnostics[0].line, 1);
    EXPECT_TRUE(diagnostics[0].has_fix);
    EXPECT_TRUE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics),
        "#include <algorithm>\n"
        "#include <map>\n"
        "#include <vector>\n");
}

TEST(RuleEngineSpec, SortIncludesKeepsSortedBlocks)
{
    heimdall::RuleOptions options;
    options.sort_includes = true;
    EXPECT_TRUE(heimdall::RuleEngine(options).Analyze(
        "#include <algorithm>\n#include <map>\n").empty());
    EXPECT_TRUE(heimdall::RuleEngine(options).Analyze(
        "#include <algorithm>\n#include <map>\n#include \"app.h\"\n").empty());
}

TEST(RuleEngineSpec, SortIncludesReordersBlocksIndependently)
{
    constexpr std::string_view source =
        "#include <vector>\n"
        "#include <map>\n"
        "\n"
        "#include \"z.h\"\n"
        "#include \"a.h\"\n";
    heimdall::RuleOptions options;
    options.sort_includes = true;
    const auto diagnostics = heimdall::RuleEngine(options).Analyze(source);
    ASSERT_EQ(diagnostics.size(), 2);
    EXPECT_EQ(diagnostics[0].line, 1);
    EXPECT_EQ(diagnostics[1].line, 4);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics),
        "#include <map>\n"
        "#include <vector>\n"
        "\n"
        "#include \"a.h\"\n"
        "#include \"z.h\"\n");
}

TEST(RuleEngineSpec, SortIncludesDoesNotCrossCommentsAndDirectives)
{
    heimdall::RuleOptions options;
    options.sort_includes = true;
    EXPECT_TRUE(heimdall::RuleEngine(options).Analyze(
        "#include <vector>\n// comment\n#include <map>\n").empty());
    EXPECT_TRUE(heimdall::RuleEngine(options).Analyze(
        "#include <vector>\n#pragma once\n#include <map>\n").empty());
    EXPECT_TRUE(heimdall::RuleEngine(options).Analyze(
        "#include <vector>\n#ifdef USE\n#include <map>\n#endif\n#include <algorithm>\n").empty());
}

TEST(RuleEngineSpec, SortIncludesIgnoresMacroAndIncludeNext)
{
    heimdall::RuleOptions options;
    options.sort_includes = true;
    EXPECT_TRUE(heimdall::RuleEngine(options).Analyze(
        "#include <vector>\n#include HEADER\n#include <map>\n").empty());
    EXPECT_TRUE(heimdall::RuleEngine(options).Analyze(
        "#include <vector>\n#include_next <map>\n#include <algorithm>\n").empty());
}

TEST(RuleEngineSpec, SortIncludesRespectsConfiguredGroupOrder)
{
    constexpr std::string_view source =
        "#include <vector>\n"
        "#include \"app.h\"\n";
    heimdall::RuleOptions options;
    options.sort_includes = true;
    options.include_order = {heimdall::IncludeGroup::Quote, heimdall::IncludeGroup::Angle};
    const auto diagnostics = heimdall::RuleEngine(options).Analyze(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics),
        "#include \"app.h\"\n"
        "#include <vector>\n");
}

TEST(RuleEngineSpec, SortIncludesSeparatesConfiguredGroups)
{
    constexpr std::string_view source =
        "#include <vector>\n"
        "#include \"app.h\"\n"
        "#include <map>\n";
    heimdall::RuleOptions options;
    options.sort_includes = true;
    const auto diagnostics = heimdall::RuleEngine(options).Analyze(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics),
        "#include <map>\n"
        "#include <vector>\n"
        "#include \"app.h\"\n");
}

TEST(RuleEngineSpec, SortIncludesIsCaseInsensitiveByDefault)
{
    constexpr std::string_view source =
        "#include <vector>\n"
        "#include <Array>\n";
    heimdall::RuleOptions options;
    options.sort_includes = true;
    const auto diagnostics = heimdall::RuleEngine(options).Analyze(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics),
        "#include <Array>\n"
        "#include <vector>\n");
}

TEST(RuleEngineSpec, SortIncludesCaseSensitivityIsConfigurable)
{
    constexpr std::string_view source =
        "#include <apple>\n"
        "#include <Zebra>\n";
    heimdall::RuleOptions options;
    options.sort_includes = true;
    EXPECT_TRUE(heimdall::RuleEngine(options).Analyze(source).empty());

    options.include_case_insensitive = false;
    const auto diagnostics = heimdall::RuleEngine(options).Analyze(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics),
        "#include <Zebra>\n"
        "#include <apple>\n");
}

TEST(RuleEngineSpec, SortIncludesPreservesLineEndingsAndIndentation)
{
    heimdall::RuleOptions options;
    options.sort_includes = true;
    constexpr std::string_view crlf = "#include <b>\r\n#include <a>\r\n";
    const auto with_crlf = heimdall::RuleEngine(options).Analyze(crlf);
    ASSERT_EQ(with_crlf.size(), 1);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(crlf, with_crlf),
        "#include <a>\r\n#include <b>\r\n");

    constexpr std::string_view indented = "  #include <b>\n  #include <a>\n";
    const auto with_indent = heimdall::RuleEngine(options).Analyze(indented);
    ASSERT_EQ(with_indent.size(), 1);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(indented, with_indent),
        "  #include <a>\n  #include <b>\n");
}

TEST(RuleEngineSpec, SortIncludesHonorsSuppressionsAndOverrides)
{
    constexpr std::string_view source =
        "#include <vector> // heimdall-disable-line cpp/sort-includes\n"
        "#include <map>\n";
    heimdall::RuleOptions options;
    options.sort_includes = true;
    EXPECT_TRUE(heimdall::RuleEngine(options).Analyze(source).empty());

    options.overrides.push_back({"cpp/sort-includes", true, heimdall::Severity::Error});
    const auto diagnostics = heimdall::RuleEngine(options).Analyze(
        "#include <vector>\n#include <map>\n");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].severity, heimdall::Severity::Error);

    options.overrides[0].enabled = false;
    EXPECT_TRUE(heimdall::RuleEngine(options).Analyze(
        "#include <vector>\n#include <map>\n").empty());
}

TEST(RuleEngineSpec, SortIncludesEnabledByOverride)
{
    // Opt-in rule: an enabled override turns it on, like
    // --rule cpp/sort-includes=warning or the 'rules' section.
    heimdall::RuleOptions options;
    options.overrides.push_back({"cpp/sort-includes", true, heimdall::Severity::Warning});
    const auto diagnostics = heimdall::RuleEngine(options).Analyze(
        "#include <vector>\n#include <map>\n");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].code, "cpp/sort-includes");
    EXPECT_EQ(diagnostics[0].severity, heimdall::Severity::Warning);

    options.overrides[0].enabled = false;
    EXPECT_TRUE(heimdall::RuleEngine(options).Analyze(
        "#include <vector>\n#include <map>\n").empty());
}
