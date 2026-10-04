#include <gtest/gtest.h>

#include <Heimdall/Preprocessor.hpp>

#include <string>

TEST(PreprocessorSpec, KeepsDirectivesOpaqueAndSelectsConditionalBranches)
{
    constexpr std::string_view source =
        "#include <vector>\n"
        "#define ENABLED 1\n"
        "#if defined(ENABLED) && ENABLED\n"
        "int selected = ENABLED;\n"
        "#else\n"
        "int rejected;\n"
        "#endif\n";

    const auto result = heimdall::Preprocessor().Process(source, true);
    EXPECT_EQ(result.active_source, "int selected = 1;\n");
    ASSERT_EQ(result.directives.size(), 5);
    EXPECT_EQ(result.directives.front().kind, heimdall::DirectiveKind::Include);
    ASSERT_TRUE(result.diagnostics.empty()) << (result.diagnostics.empty() ? "" : result.diagnostics.front().message);
}

TEST(PreprocessorSpec, SupportsNestedConditionalsAndUndef)
{
    constexpr std::string_view source =
        "#define FLAG 1\n"
        "#if FLAG\n"
        "outer\n"
        "#if 0\n"
        "inner_no\n"
        "#else\n"
        "inner_yes\n"
        "#endif\n"
        "#undef FLAG\n"
        "#endif\n"
        "#ifdef FLAG\n"
        "undefined_no\n"
        "#else\n"
        "undefined_yes\n"
        "#endif\n";

    const auto result = heimdall::Preprocessor().Process(source, true);
    EXPECT_EQ(result.active_source, "outer\ninner_yes\nundefined_yes\n");
    ASSERT_TRUE(result.diagnostics.empty()) << (result.diagnostics.empty() ? "" : result.diagnostics.front().message);
}

TEST(PreprocessorSpec, ReportsUnmatchedAndUnterminatedConditionals)
{
    const auto result = heimdall::Preprocessor().Process("#else\n#if 1\nactive\n", true);
    EXPECT_EQ(result.diagnostics.size(), 2);
    EXPECT_EQ(result.diagnostics[0].message, "#else without matching #if");
    EXPECT_EQ(result.diagnostics[1].message, "unterminated conditional directive");
    EXPECT_EQ(result.active_source, "active\n");
}

TEST(PreprocessorSpec, DoesNotExpandMacrosInsideQuotedLiterals)
{
    const auto result = heimdall::Preprocessor().Process("#define NAME replacement\nconst char* s = \"NAME\"; // NAME\n", true);
    EXPECT_EQ(result.active_source, "const char* s = \"NAME\"; // NAME\n");
}

TEST(PreprocessorSpec, EvaluatesIntegerComparisonsInIfExpressions)
{
    constexpr std::string_view source =
        "#define VERSION 2\n"
        "#if VERSION >= 2 && VERSION != 3\n"
        "selected\n"
        "#else\n"
        "rejected\n"
        "#endif\n";
    const auto result = heimdall::Preprocessor().Process(source, true);
    EXPECT_EQ(result.active_source, "selected\n");
    EXPECT_TRUE(result.diagnostics.empty());
}

TEST(PreprocessorSpec, SkipsActiveSourceExpansionByDefault)
{
    constexpr std::string_view source = "#define ENABLED 1\nint selected = ENABLED;\n";
    const auto result = heimdall::Preprocessor().Process(source);
    EXPECT_TRUE(result.active_source.empty());
    ASSERT_EQ(result.active_ranges.size(), 1);
    EXPECT_EQ(source.substr(result.active_ranges.front().offset, result.active_ranges.front().length),
              "int selected = ENABLED;\n");
}

TEST(PreprocessorSpec, PredefinedMacrosAreReadWithoutCopying)
{
    heimdall::Preprocessor::MacroMap predefined { { "ENABLED", "1" } };
    constexpr std::string_view source = "#if ENABLED\nselected\n#else\nrejected\n#endif\n";
    const auto result = heimdall::Preprocessor(predefined).Process(source, true);
    EXPECT_EQ(result.active_source, "selected\n");
    // The view must not have taken ownership: the caller's map is untouched.
    EXPECT_EQ(predefined.size(), 1);
}

TEST(PreprocessorSpec, BackslashContinuationExtendsTheDirectiveSpan)
{
    for (const std::string_view eol : {"\n", "\r\n"})
    {
        std::string source = "#define VALUE \\" + std::string(eol) + "    40 + \\" + std::string(eol) + "    2" +
            std::string(eol) + "int x = VALUE;" + std::string(eol) + "#define TAIL \\";
        const auto result = heimdall::Preprocessor().Process(source, true);

        ASSERT_EQ(result.directives.size(), 2u);
        const auto first_end = source.find("int x");
        EXPECT_EQ(result.directives[0].offset, 0u);
        EXPECT_EQ(result.directives[0].length, first_end);
        // Only the real code line is active; the continuation lines are not.
        ASSERT_EQ(result.active_ranges.size(), 1u);
        EXPECT_EQ(result.active_ranges[0].offset, first_end);
        EXPECT_EQ(result.active_source, "int x = 40 +      2;" + std::string(eol));
        EXPECT_TRUE(result.diagnostics.empty());
    }
}

TEST(PreprocessorSpec, ContinuedConditionalDirectiveIsEvaluatedAsOneLine)
{
    constexpr std::string_view source =
        "#define A 1\n"
        "#if defined(A) && \\\n"
        "    A\n"
        "int on;\n"
        "#else\n"
        "int off;\n"
        "#endif\n";
    const auto result = heimdall::Preprocessor().Process(source, true);
    EXPECT_EQ(result.active_source, "int on;\n");
    EXPECT_TRUE(result.diagnostics.empty());
}

TEST(PreprocessorSpec, IgnoresDirectivesInsideBlockComments)
{
    constexpr std::string_view source =
        "#ifndef GUARD\n"
        "/* history:\n"
        "     #ifdef unused functions\n"
        "*/\n"
        "int kept; // #if 0\n"
        "const char * s = \"/* not a comment\";\n"
        "#endif\n";

    const auto result = heimdall::Preprocessor().Process(source);
    EXPECT_TRUE(result.diagnostics.empty()) << (result.diagnostics.empty() ? "" : result.diagnostics.front().message);
    EXPECT_EQ(result.directives.size(), 2);
}

TEST(PreprocessorSpec, ReportsLocalDefines)
{
    const auto result = heimdall::Preprocessor().Process("#define API extern\n#define GONE 1\n#undef GONE\n");
    ASSERT_TRUE(result.local_macros.contains("API"));
    EXPECT_EQ(result.local_macros.at("API"), "extern");
    EXPECT_FALSE(result.local_macros.contains("GONE"));
}
