#include <gtest/gtest.h>

#include <Heimdall/Formatter.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

TEST(FormatterSpec, IndentsByBraceDepthAndIsIdempotent)
{
    constexpr std::string_view source = "int main() {\nint x = 1;\nif (x) {\nreturn x;\n}\n}\n";
    const heimdall::Formatter  formatter;
    const std::string          expected =
        "int main()\n{\n    int x = 1;\n    if (x)\n    {\n        return x;\n    }\n}\n";
    const std::string formatted = formatter.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, IgnoresBracesInCommentsLiteralsAndPreprocessorDirectives)
{
    constexpr std::string_view source =
        "#define BLOCK { ignored }\n"
        "void f() {\n"
        "auto text = \"{ not a block }\"; // } comment\n"
        "/* { comment */\n"
        "}\n";
    const heimdall::Formatter formatter;
    const std::string         expected =
        "#define BLOCK { ignored }\n"
        "void f()\n"
        "{\n"
        "    auto text = \"{ not a block }\"; // } comment\n"
        "    /* { comment */\n"
        "}\n";
    const std::string formatted = formatter.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, SupportsTabsBlankLinesCrLfAndMissingFinalNewline)
{
    const heimdall::Formatter formatter({ .indent_width = 2, .use_tabs = true });
    EXPECT_EQ(formatter.Format("{\r\n\r\nx\r\n}\r\n"), "{\r\n\r\n\tx\r\n}\r\n");
    EXPECT_EQ(formatter.Format("{\nx\n}"), "{\n\tx\n}");
    // CRLF is preserved byte-for-byte and formatting is a fixed point.
    const std::string crlf = formatter.Format("{\r\nx\r\n}\r\n");
    EXPECT_EQ(crlf, "{\r\n\tx\r\n}\r\n");
    EXPECT_EQ(formatter.Format(crlf), crlf);
}

TEST(FormatterSpec, IndentsInnerScopesOfNamespacesAndClasses)
{
    constexpr std::string_view source =
        "namespace outer {\n"
        "namespace inner {\n"
        "int x;\n"
        "}\n"
        "}\n"
        "class Widget {\n"
        "public:\n"
        "int value;\n"
        "void run();\n"
        "private:\n"
        "int hidden;\n"
        "};\n";
    const heimdall::Formatter formatter;
    const std::string         expected =
        "namespace outer\n"
        "{\n"
        "    namespace inner\n"
        "    {\n"
        "        int x;\n"
        "    }\n"
        "}\n"
        "class Widget\n"
        "{\n"
        "public:\n"
        "    int value;\n\n"
        "    void run();\n\n"
        "private:\n"
        "    int hidden;\n"
        "};\n";
    const std::string formatted = formatter.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, DedentsCaseLabelsAndGotoLabels)
{
    constexpr std::string_view source =
        "int f(int x) {\n"
        "switch (x) {\n"
        "case 1:\n"
        "return 1;\n"
        "case 2: {\n"
        "return 2;\n"
        "}\n"
        "default:\n"
        "return 0;\n"
        "}\n"
        "goto done;\n"
        "done:\n"
        "return -1;\n"
        "}\n";
    const heimdall::Formatter formatter({ .blank_line_after_control_block = false });
    const std::string         expected =
        "int f(int x)\n"
        "{\n"
        "    switch (x)\n"
        "    {\n"
        "    case 1:\n"
        "        return 1;\n"
        "    case 2:\n"
        "    {\n"
        "        return 2;\n"
        "    }\n"
        "    default:\n"
        "        return 0;\n"
        "    }\n"
        "    goto done;\n"
        "done:\n"
        "    return -1;\n"
        "}\n";
    const std::string formatted = formatter.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, DedentsOneLevelPerLeadingCloser)
{
    constexpr std::string_view source = "namespace a {\nnamespace b {\nint x;\n}}\n";
    const heimdall::Formatter  formatter({ .brace_style = heimdall::BraceStyle::Attach });
    const std::string expected  = "namespace a {\n    namespace b {\n        int x;\n    }\n}\n";
    const std::string formatted = formatter.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, CollapsesBlankLinesToMaxEmptyLines)
{
    const heimdall::Formatter  formatter;
    constexpr std::string_view source =
        "std::string line;\n"
        "std::size_t length = 0;\n"
        "\n"
        "\n"
        "\n"
        "bool got_length = false;\n";
    const std::string expected =
        "std::string line;\n"
        "std::size_t length = 0;\n"
        "\n"
        "bool got_length = false;\n";
    const std::string formatted = formatter.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);

    // Custom budget keeps two; zero strips all; edges are always trimmed.
    const heimdall::Formatter keep_two({ .max_empty_lines = 2 });
    const std::string         two = keep_two.Format(source);
    EXPECT_EQ(two,
              "std::string line;\n"
              "std::size_t length = 0;\n"
              "\n"
              "\n"
              "bool got_length = false;\n");
    EXPECT_EQ(keep_two.Format(two), two);

    const heimdall::Formatter keep_none({ .max_empty_lines = 0 });
    EXPECT_EQ(keep_none.Format(source),
              "std::string line;\n"
              "std::size_t length = 0;\n"
              "bool got_length = false;\n");
    EXPECT_EQ(formatter.Format("\n\nint x;\n\n\n"), "int x;\n");
}

TEST(FormatterSpec, DoesNotTreatCodeBeforeDirectivesAsDirectives)
{
    // Regression: lines preceding a directive used to be copied verbatim,
    // freezing brace depth for the rest of the file.
    constexpr std::string_view source =
        "int main()\n"
        "{\n"
        "#if defined(_WIN32)\n"
        "_setmode(1);\n"
        "#endif\n"
        "x();\n"
        "}\n";
    const heimdall::Formatter formatter;
    const std::string         expected =
        "int main()\n"
        "{\n"
        "#if defined(_WIN32)\n"
        "    _setmode(1);\n"
        "#endif\n"
        "    x();\n"
        "}\n";
    const std::string formatted = formatter.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, TrimsTrailingWhitespace)
{
    constexpr std::string_view source = "int x;   \n\tint y;\t \n";
    const heimdall::Formatter  formatter;
    const std::string          expected  = "int x;\nint y;\n";
    const std::string          formatted = formatter.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, IndentsContinuationLines)
{
    constexpr std::string_view source =
        "int sum = a +\n"
        "b;\n"
        "foo(a,\n"
        "b);\n"
        "if (x\n"
        "&& y) {\n"
        "f();\n"
        "}\n";
    const heimdall::Formatter formatter({ .brace_style = heimdall::BraceStyle::Attach });
    const std::string         expected =
        "int sum = a +\n"
        "    b;\n"
        "foo(a,\n"
        "    b);\n"
        "if (x\n"
        "    && y) {\n"
        "    f();\n"
        "}\n";
    const std::string formatted = formatter.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, NormalizesSpacing)
{
    constexpr std::string_view source =
        "int x=1+2;\n"
        "foo(a,b,c);\n"
        "if(x){}\n"
        "for(int i=0;i<10;++i){}\n"
        "int*x;\n"
        "int &r=x;\n"
        "a==b;\n"
        "x* (y+1);\n"
        "}else{\n";
    const heimdall::Formatter formatter(
        { .brace_style = heimdall::BraceStyle::Attach, .blank_line_after_control_block = false });
    const std::string expected =
        "int x = 1 + 2;\n"
        "foo(a, b, c);\n"
        "if (x) {}\n"
        "for (int i = 0; i<10; ++i) {}\n"
        "int* x;\n"
        "int& r = x;\n"
        "a == b;\n"
        "x * (y + 1);\n"
        "} else {\n";
    const std::string formatted = formatter.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, KeepsReflectionAndSpliceDelimitersTogether)
{
    const heimdall::Formatter formatter;
    constexpr std::string_view source =
        "using ReflectedType =[: ^^ Sample:];\n"
        "using Alias=typename [: r :]::type;\n"
        "using MemberType=typename [: ^^Sample::type:];\n"
        "namespace reflected=[: ns:];\n"
        "int x=obj.[: member:];\n"
        "int s=template [: klass:]<int>::value;\n"
        "int y=array[0]+2;\n"
        "int z=flag?1:2;\n";
    constexpr std::string_view expected =
        "using ReflectedType = [: ^^Sample :];\n"
        "using Alias = typename [: r :]::type;\n"
        "using MemberType = typename [: ^^Sample::type :];\n"
        "namespace reflected = [: ns :];\n"
        "int x = obj.[: member :];\n"
        "int s = template [: klass :]<int>::value;\n"
        "int y = array[0] + 2;\n"
        "int z = flag ? 1 : 2;\n";

    const auto formatted = formatter.Format(source);

    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, AlignsAdjacentStringLiteralContinuations)
{
    const heimdall::Formatter formatter;
    constexpr std::string_view source = R"cpp(void f()
{
    constexpr std::string_view kSample =
        "#include <vector>\n"
    "namespace demo {\n"
    "template <typename T> struct Box { T value; int get() const { return value; } };\n"
    "enum class Color { Red, Green = 2 };\n"
    "int add(int left, int right) {\n"
    "    if (left > right) { return left - right; }\n"
    "    for (int i = 0; i < right; ++i) { left += i * 2; }\n"
    "    auto lambda = [&](int x) { return x + left; };\n"
    "    return lambda(right) + (left ? right : 0);\n"
    "}\n"
    "}\n";
}

)cpp";
    constexpr std::string_view expected = R"cpp(void f()
{
    constexpr std::string_view kSample =
        "#include <vector>\n"
        "namespace demo {\n"
        "template <typename T> struct Box { T value; int get() const { return value; } };\n"
        "enum class Color { Red, Green = 2 };\n"
        "int add(int left, int right) {\n"
        "    if (left > right) { return left - right; }\n"
        "    for (int i = 0; i < right; ++i) { left += i * 2; }\n"
        "    auto lambda = [&](int x) { return x + left; };\n"
        "    return lambda(right) + (left ? right : 0);\n"
        "}\n"
        "}\n";
}
)cpp";

    const auto formatted = formatter.Format(source);

    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, AlignsDeclarationAndCallContinuations)
{
    const heimdall::Formatter formatter;
    constexpr std::string_view source =
        "void save(std::string_view suffix, std::filesystem::path directory)\n"
        "{\n"
        "    const auto path = std::filesystem::temp_directory_path() /\n"
        "            (\"heimdall_rule_config_\" + std::string(suffix));\n"
        "    std::ofstream out(directory / heimdall::RuleConfigFileName,\n"
        "            std::ios::binary | std::ios::trunc);\n"
        "}\n";
    constexpr std::string_view expected =
        "void save(std::string_view suffix, std::filesystem::path directory)\n"
        "{\n"
        "    const auto path = std::filesystem::temp_directory_path() /\n"
        "                      (\"heimdall_rule_config_\" + std::string(suffix));\n"
        "    std::ofstream out(directory / heimdall::RuleConfigFileName,\n"
        "                      std::ios::binary | std::ios::trunc);\n"
        "}\n";

    const auto formatted = formatter.Format(source);

    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, SpacesBracedListsWithoutChangingBlockBraces)
{
    const heimdall::Formatter formatter;
    constexpr std::string_view source =
        "Widget w{1};\nvoid f(){return Widget{2};}\n";
    const std::string formatted = formatter.Format(source);

    EXPECT_EQ(formatted,
              "Widget w {1};\nvoid f()\n{\n    return Widget {2};\n}\n");
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, LogicalNotSpacingCanBeSelected)
{
    const heimdall::Formatter joined;
    const heimdall::Formatter spaced({ .space_after_logical_not = true });

    EXPECT_EQ(joined.Format("bool x = ! flag;\n"), "bool x = !flag;\n");
    EXPECT_EQ(spaced.Format("bool x = !flag;\n"), "bool x = ! flag;\n");
    EXPECT_EQ(spaced.Format(spaced.Format("bool x = !flag;\n")), "bool x = ! flag;\n");
}

TEST(FormatterSpec, CStyleCastSpacingDoesNotChangeFunctionCalls)
{
    const heimdall::Formatter formatter;
    const heimdall::Formatter joined({ .space_after_c_style_cast = false });

    EXPECT_EQ(formatter.Format("int x = (int)y;\nfoo(y);\n"),
              "int x = (int) y;\nfoo(y);\n");
    EXPECT_EQ(joined.Format("int x = (int) y;\nfoo(y);\n"),
              "int x = (int)y;\nfoo(y);\n");
    EXPECT_EQ(formatter.Format(formatter.Format("int x = (int)y;\n")),
              "int x = (int) y;\n");
}

TEST(FormatterSpec, AlignsConsecutiveAssignmentsWithoutTouchingComparisons)
{
    const heimdall::Formatter formatter({ .align_consecutive_assignments = true });
    constexpr std::string_view source =
        "int x=1; // first\nint longer=2; // second\n"
        "if (x == longer) {\n    foo(x=3);\n}\n"
        "int y=4;\n\nint longName=5;\n";

    const std::string formatted = formatter.Format(source);

    EXPECT_EQ(formatted,
              "int x      = 1; // first\nint longer = 2; // second\n"
              "if (x == longer)\n{\n    foo(x = 3);\n}\n\n"
              "int y = 4;\n\nint longName = 5;\n");
    EXPECT_EQ(formatter.Format(formatted), formatted);
    EXPECT_EQ(formatter.Format("auto a = \"x=y\";\nauto longer = \"z\";\n"),
              "auto a      = \"x=y\";\nauto longer = \"z\";\n");
}

TEST(FormatterSpec, AlignsOnlyConsecutiveSingleLineMacroDefinitions)
{
    const heimdall::Formatter formatter({ .align_consecutive_macros = true });
    constexpr std::string_view source =
        "#define A 1\n#define LONG_NAME 2\n"
        "#define F(x) (x)\n"
        "#define CONT \\\n"
        "  42\n"
        "#define B 3\n";

    const std::string formatted = formatter.Format(source);

    EXPECT_EQ(formatted,
              "#define A         1\n#define LONG_NAME 2\n"
              "#define F(x)      (x)\n"
              "#define CONT \\\n"
              "  42\n"
              "#define B 3\n");
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, LeavesTemplatesAndShiftOperatorsAsTyped)
{
    // `<`/`>` are ambiguous without name resolution (templates vs.
    // relational/shift operators), so their spacing is preserved as typed
    // (the `,` after `string` is still normalized).
    constexpr std::string_view source =
        "std::map<std::string,int> m;\n"
        "if (a<b) {}\n";
    const heimdall::Formatter formatter({ .brace_style = heimdall::BraceStyle::Attach });
    const std::string         formatted = formatter.Format(source);
    EXPECT_EQ(formatted, "std::map<std::string, int> m;\nif (a<b) {}\n");
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, SplitsOneLineBlocks)
{
    constexpr std::string_view source = "int g(){return 1;}\nvoid f() {}\nx=1; y=2;\n";
    const heimdall::Formatter  formatter({ .brace_style = heimdall::BraceStyle::Attach });
    const std::string expected  = "int g() {\n    return 1;\n}\n\nvoid f() {}\nx = 1;\ny = 2;\n";
    const std::string formatted = formatter.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, SupportsAllmanBraces)
{
    constexpr std::string_view source = "int main() {\nreturn 0;\n}\n";
    const heimdall::Formatter  allman({ .brace_style = heimdall::BraceStyle::Allman });
    const std::string          expected  = "int main()\n{\n    return 0;\n}\n";
    const std::string          formatted = allman.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(allman.Format(formatted), formatted);
}

TEST(FormatterSpec, AttachKeepsBracesOnTheSameLine)
{
    // Explicit default: opening braces attach with one space, `else`/`catch`
    // stay cuddled with the closing brace.
    constexpr std::string_view source =
        "int main() {\n"
        "if (a) {\n"
        "x();\n"
        "} else {\n"
        "y();\n"
        "}\n"
        "}\n";
    const heimdall::Formatter attach({ .brace_style = heimdall::BraceStyle::Attach });
    const std::string         expected =
        "int main() {\n"
        "    if (a) {\n"
        "        x();\n"
        "    } else {\n"
        "        y();\n"
        "    }\n"
        "}\n";
    const std::string formatted = attach.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(attach.Format(formatted), formatted);
}

TEST(FormatterSpec, AllmanDetachesElseAndCatch)
{
    constexpr std::string_view source =
        "if (a) {\n"
        "x();\n"
        "} else {\n"
        "y();\n"
        "}\n"
        "try {\n"
        "f();\n"
        "} catch (...) {\n"
        "g();\n"
        "}\n";
    const heimdall::Formatter allman(
        { .brace_style = heimdall::BraceStyle::Allman, .blank_line_after_control_block = false });
    const std::string expected =
        "if (a)\n"
        "{\n"
        "    x();\n"
        "}\n"
        "else\n"
        "{\n"
        "    y();\n"
        "}\n"
        "try\n"
        "{\n"
        "    f();\n"
        "}\n"
        "catch (...)\n"
        "{\n"
        "    g();\n"
        "}\n";
    const std::string formatted = allman.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(allman.Format(formatted), formatted);
}

TEST(FormatterSpec, AllmanBreaksOneLineBlocks)
{
    const heimdall::Formatter allman({ .brace_style = heimdall::BraceStyle::Allman });
    EXPECT_EQ(allman.Format("int g(){return 1;}\n"), "int g()\n{\n    return 1;\n}\n");
    EXPECT_EQ(allman.Format("do {\nx();\n} while (y);\n"), "do\n{\n    x();\n} while (y);\n");
    const std::string formatted = allman.Format("int g(){return 1;}\n");
    EXPECT_EQ(allman.Format(formatted), formatted);
}

TEST(FormatterSpec, AllmanNewDesignatedInitMultiLineKeepsEntryIndent)
{
    const heimdall::Formatter allman({ .brace_style = heimdall::BraceStyle::Allman });
    const std::string         out = allman.Format(
        "void f() {\n    auto r = new Node{\n        .next = next,\n        .value = 20,\n"
        "        .x = 1\n    };\n}\n");
    EXPECT_EQ(out,
              "void f()\n{\n    auto r = new Node\n    {\n        .next = next,\n"
              "        .value = 20,\n        .x = 1\n    };\n}\n");
    EXPECT_EQ(allman.Format(out), out);
}

TEST(FormatterSpec, AllmanCombinesWithAddedBraces)
{
    const heimdall::Formatter both(
        { .brace_style       = heimdall::BraceStyle::Allman,
          .single_line_style = heimdall::SingleLineStyle::IndentWithBraces });
    const std::string expected  = "if (x)\n{\n    return;\n}\n";
    const std::string formatted = both.Format("if (x) return;\n");
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(both.Format(formatted), formatted);
}

TEST(FormatterSpec, SplitsBracesAfterFunctionSuffixes)
{
    // Pin Keep: this test is about brace placement, not single_line_style.
    const heimdall::Formatter allman({ .brace_style       = heimdall::BraceStyle::Allman,
                                       .single_line_style = heimdall::SingleLineStyle::Keep });
    const auto                check = [&](std::string_view source, std::string_view expected) {
        const std::string formatted = allman.Format(source);
        EXPECT_EQ(formatted, expected);
        EXPECT_EQ(allman.Format(formatted), formatted);
    };
    check("void f() const noexcept { return; }\n", "void f() const noexcept\n{\n    return;\n}\n");
    check("void f() const { return; }\n", "void f() const\n{\n    return;\n}\n");
    check("void f() override { x(); }\n", "void f() override\n{\n    x();\n}\n");
    check("void f() & { x(); }\n", "void f() &\n{\n    x();\n}\n");
    check("void f() noexcept(noexcept(g())) { x(); }\n",
          "void f() noexcept(noexcept(g()))\n{\n    x();\n}\n");
    check("void f() throw(X) { x(); }\n", "void f() throw(X)\n{\n    x();\n}\n");
    check("int g() -> int { return 0; }\n", "int g() -> int\n{\n    return 0;\n}\n");
    check("std::size_t C::M(int i) noexcept { return i; }\n",
          "std::size_t C::M(int i) noexcept\n{\n    return i;\n}\n");
    check("bool X::operator==(const X& o) const { return true; }\n",
          "bool X::operator==(const X& o) const\n{\n    return true;\n}\n");
    check("C::C() : x(1) {}\n", "C::C() : x(1) {}\n");
    check("template <typename T> T f(T t) requires C<T> { return t; }\n",
          "template <typename T> T f(T t) requires C<T>\n{\n    return t;\n}\n");
    check("[&](int x) { return x; };\n", "[&](int x)\n{\n    return x;\n};\n");
    check("[&] { return x; };\n", "[&]\n{\n    return x;\n};\n");
    check("void f() try { x(); } catch (...) { y(); }\n",
          "void f() try\n{\n    x();\n}\ncatch (...)\n{\n    y();\n}\n");
}

TEST(FormatterSpec, SplitsBracesOfNamedScopes)
{
    const heimdall::Formatter allman({ .brace_style       = heimdall::BraceStyle::Allman,
                                       .single_line_style = heimdall::SingleLineStyle::Keep });
    const auto                check = [&](std::string_view source, std::string_view expected) {
        const std::string formatted = allman.Format(source);
        EXPECT_EQ(formatted, expected);
        EXPECT_EQ(allman.Format(formatted), formatted);
    };
    check("namespace a { int x; }\n", "namespace a\n{\n    int x;\n}\n");
    check("namespace a::b { int x; }\n", "namespace a::b\n{\n    int x;\n}\n");
    check("namespace { int x; }\n", "namespace\n{\n    int x;\n}\n");
    check("class W { int x; };\n", "class W\n{\n    int x;\n};\n");
    check("struct S { int x; };\n", "struct S\n{\n    int x;\n};\n");
    check("union U { int i; float f; };\n", "union U\n{\n    int i;\n    float f;\n};\n");
    check("enum E { A, B };\n", "enum E\n{\n    A, B\n};\n");
    check("enum class E { A };\n", "enum class E\n{\n    A\n};\n");
    check("union { int i; float f; };\n", "union\n{\n    int i;\n    float f;\n};\n");
    check("typedef struct { int x; } Name;\n", "typedef struct\n{\n    int x;\n} Name;\n");
    check("extern \"C\" { int f(); }\n", "extern \"C\"\n{\n    int f();\n}\n");
    check("extern \"C++\" { void g(); }\n", "extern \"C++\"\n{\n    void g();\n}\n");
    check("extern \"C\" {\n    void a();\n    int b();\n}\n",
          "extern \"C\"\n{\n    void a();\n    int b();\n}\n");
}

TEST(FormatterSpec, KeepsInitializerBracesAttached)
{
    // Initializers and expressions never split, in either brace style.
    const heimdall::Formatter allman({ .brace_style       = heimdall::BraceStyle::Allman,
                                       .single_line_style = heimdall::SingleLineStyle::Keep,
                                       .space_before_cpp11_braced_list = false });
    const auto                check = [&](std::string_view source, std::string_view expected) {
        const std::string formatted = allman.Format(source);
        EXPECT_EQ(formatted, expected);
        EXPECT_EQ(allman.Format(formatted), formatted);
    };
    check("Widget w{1, 2};\n", "Widget w{1, 2};\n");
    check("int a[] = {1, 2, 3};\n", "int a[] = {1, 2, 3};\n");
    check("int f() {\nreturn {1};\n}\n", "int f()\n{\n    return {1};\n}\n");
    check("f(a, {1, 2});\n", "f(a, {1, 2});\n");
    check("auto p = new (buf) T{1};\n", "auto p = new (buf) T{1};\n");
    check("Point p{.x = 1};\n", "Point p{.x = 1};\n");
}

TEST(FormatterSpec, SpacesTrailingReturnArrow)
{
    const heimdall::Formatter formatter;
    const auto                check = [&](std::string_view source, std::string_view expected) {
        const std::string formatted = formatter.Format(source);
        EXPECT_EQ(formatted, expected);
        EXPECT_EQ(formatter.Format(formatted), formatted);
    };
    check("p->x = 1;\n", "p->x = 1;\n");
    check("p ->x=1;\n", "p->x = 1;\n");
    check("foo()->x();\n", "foo()->x();\n");
    check("auto f()->int;\n", "auto f() -> int;\n");
}

TEST(FormatterSpec, CreatedSingleLineBlocksFollowTheBraceStyle)
{
    // The collapsed block itself stays brace-free on one line while the
    // surrounding blocks follow the selected brace style.
    constexpr std::string_view source = "void f() {\nif (x) {\nreturn;\n}\n}\n";
    const heimdall::Formatter  flat_allman(
        { .brace_style       = heimdall::BraceStyle::Allman,
          .single_line_style = heimdall::SingleLineStyle::SingleLine });
    const std::string flat_expected = "void f()\n{\n    if (x) return;\n}\n";
    const std::string flat          = flat_allman.Format(source);
    EXPECT_EQ(flat, flat_expected);
    EXPECT_EQ(flat_allman.Format(flat), flat);

    // Indent mode: the unbraced split body nests under Allman function braces.
    const heimdall::Formatter split_allman(
        { .brace_style       = heimdall::BraceStyle::Allman,
          .single_line_style = heimdall::SingleLineStyle::Indent });
    const std::string split_expected = "void f()\n{\n    if (x)\n        return;\n}\n";
    const std::string split          = split_allman.Format("void f() {\nif (x) return;\n}\n");
    EXPECT_EQ(split, split_expected);
    EXPECT_EQ(split_allman.Format(split), split);

    // Attach (explicit) keeps the created single line cuddled in place.
    const heimdall::Formatter flat_attach(
        { .brace_style       = heimdall::BraceStyle::Attach,
          .single_line_style = heimdall::SingleLineStyle::SingleLine });
    const std::string attach_expected = "void f() {\n    if (x) return;\n}\n";
    const std::string attach          = flat_attach.Format(source);
    EXPECT_EQ(attach, attach_expected);
    EXPECT_EQ(flat_attach.Format(attach), attach);
}

TEST(FormatterSpec, SupportsLeftPointerAlignment)
{
    constexpr std::string_view source = "int*x;\nint &r=x;\n";
    const heimdall::Formatter  left({ .pointer_alignment   = heimdall::PointerAlignment::Left,
                                      .reference_alignment = heimdall::ReferenceAlignment::Left });
    const std::string          expected  = "int* x;\nint& r = x;\n";
    const std::string          formatted = left.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(left.Format(formatted), formatted);
}

TEST(FormatterSpec, AlignsReferencesAfterQualifiedTypes)
{
    constexpr std::string_view source =
        "std::filesystem::path AbsoluteNormalized(const std::filesystem::path & path);\n"
        "foo::Bar *alias;\n"
        "int *Make();\n"
        "foo::Bar *Create();\n"
        "void Consume(const std::vector<Token> & tokens);\n"
        "void Take(std::vector<int> && values);\n"
        "x = a * b();\n"
        "f(a * b());\n";

    const auto check = [](heimdall::FormatOptions options, std::string_view expected) {
        options.blank_line_between_methods = false; // isolate pointer/reference spacing
        const heimdall::Formatter formatter(options);
        const std::string         formatted = formatter.Format(source);
        EXPECT_EQ(formatted, expected);
        EXPECT_EQ(formatter.Format(formatted), formatted);
    };

    // Defaults bind to the type.
    check({}, "std::filesystem::path AbsoluteNormalized(const std::filesystem::path& path);\n"
              "foo::Bar* alias;\n"
              "int* Make();\n"
              "foo::Bar* Create();\n"
              "void Consume(const std::vector<Token>& tokens);\n"
              "void Take(std::vector<int>&& values);\n"
              "x = a * b();\n"
              "f(a * b());\n");
    check({ .pointer_alignment   = heimdall::PointerAlignment::Right,
            .reference_alignment = heimdall::ReferenceAlignment::Right },
          "std::filesystem::path AbsoluteNormalized(const std::filesystem::path &path);\n"
          "foo::Bar *alias;\n"
          "int *Make();\n"
          "foo::Bar *Create();\n"
          "void Consume(const std::vector<Token> &tokens);\n"
          "void Take(std::vector<int> &&values);\n"
          "x = a * b();\n"
          "f(a * b());\n");
}

TEST(FormatterSpec, AlignsRangeForReferencesAndSpacesSeparator)
{
    const std::string source =
        "for (const auto & node: tree.Nodes()) {}\n"
        "for (auto && value: values) {}\n"
        "for (const auto & [key, value]: entries) {}\n"
        "for (const ns::Node & node: nodes) {}\n"
        "for (auto nodes = Get(); const auto & node: nodes) {}\n"
        "for (auto value: enabled?first:second) {}\n"
        "if (a & b) {}\nwhile (a && b) {}\n";
    const auto check = [&](heimdall::ReferenceAlignment alignment, std::string_view expected) {
        const heimdall::Formatter formatter(
            { .reference_alignment = alignment, .blank_line_after_control_block = false });
        const auto formatted = formatter.Format(source);
        EXPECT_EQ(formatted, expected);
        EXPECT_EQ(formatter.Format(formatted), formatted);
        EXPECT_TRUE(formatter.FormatEdits(formatted).empty());
    };
    check(heimdall::ReferenceAlignment::Left,
          "for (const auto& node : tree.Nodes()) {}\n"
          "for (auto&& value : values) {}\n"
          "for (const auto& [key, value] : entries) {}\n"
          "for (const ns::Node& node : nodes) {}\n"
          "for (auto nodes = Get(); const auto& node : nodes) {}\n"
          "for (auto value : enabled ? first : second) {}\n"
          "if (a & b) {}\nwhile (a && b) {}\n");
    check(heimdall::ReferenceAlignment::Right,
          "for (const auto &node : tree.Nodes()) {}\n"
          "for (auto &&value : values) {}\n"
          "for (const auto &[key, value] : entries) {}\n"
          "for (const ns::Node &node : nodes) {}\n"
          "for (auto nodes = Get(); const auto &node : nodes) {}\n"
          "for (auto value : enabled ? first : second) {}\n"
          "if (a & b) {}\nwhile (a && b) {}\n");
}

TEST(FormatterSpec, KeepsBracedInitializerArithmeticInForHeader)
{
    const heimdall::Formatter formatter({ .space_before_cpp11_braced_list = false });
    const std::string         source = "void Run() {\n"
                                       "for (std::size_t i = std::size_t{name_token} + 1; i < "
                               "tokens.size(); ++i) { use(i); }\n}\n";
    const std::string expected =
        "void Run()\n{\n"
        "    for (std::size_t i = std::size_t{name_token} + 1; i < tokens.size(); ++i)\n"
        "    {\n        use(i);\n    }\n}\n";
    EXPECT_EQ(formatter.Format(source), expected);
    EXPECT_EQ(formatter.Format(expected), expected);
    EXPECT_TRUE(formatter.FormatEdits(expected).empty());
    EXPECT_EQ(formatter.Format("auto i = std::size_t{token} + 1;\n"),
              "auto i = std::size_t{token} + 1;\n");
}

TEST(FormatterSpec, SeparatesNestedBlockClosingBraces)
{
    const std::string source =
        "void Collect() {\nfor (const auto & entry: fs::directory_iterator(directory))\n{\n"
        "if (entry.path().extension() == \".hpp\")\n{\n"
        "result.push_back(entry.path().filename().string());\n}}\n}\n";
    const std::string expected =
        "void Collect()\n{\n    for (const auto& entry : fs::directory_iterator(directory))\n    "
        "{\n"
        "        if (entry.path().extension() == \".hpp\")\n        {\n"
        "            result.push_back(entry.path().filename().string());\n        }\n    }\n}\n";
    const heimdall::Formatter formatter;
    EXPECT_EQ(formatter.Format(source), expected);
    EXPECT_EQ(formatter.Format(expected), expected);
    EXPECT_TRUE(formatter.FormatEdits(expected).empty());
    EXPECT_EQ(formatter.Format("struct S { void Run() { if (ok) {} } };\n"),
              "struct S\n{\n    void Run()\n    {\n        if (ok) {}\n    }\n};\n");
}

TEST(FormatterSpec, AlignsRangeForPointersWithMultilineBracedRanges)
{
    const std::string source = "void Run() {\nfor (const char * sub:\n"
                               "{\n\"core/include/Heimdall\", \"core/src\"\n}) {}\n}\n";
    for (const auto alignment :
         { heimdall::PointerAlignment::Left, heimdall::PointerAlignment::Right })
    {
        const heimdall::Formatter formatter({ .pointer_alignment = alignment });
        const auto                formatted = formatter.Format(source);
        EXPECT_NE(formatted.find(alignment == heimdall::PointerAlignment::Left
                                     ? "for (const char* sub :"
                                     : "for (const char *sub :"),
                  std::string::npos);
        EXPECT_NE(formatted.find("\"core/include/Heimdall\", \"core/src\""), std::string::npos);
        EXPECT_EQ(formatter.Format(formatted), formatted);
        EXPECT_TRUE(formatter.FormatEdits(formatted).empty());
    }
}

TEST(FormatterSpec, AlignsReturnTypesBeforeQualifiedMethodNames)
{
    const std::string source =
        "const SemanticModel & AnalysisContext::Semantic() const { return model; }\n"
        "ns::Model && ns::Context::Take() { return model; }\n"
        "const ns::Model * ns::Context::Get() { return ptr; }\n"
        "x = a & ns::value;\nx = a * ns::Make();\nx = a && ns::Ready();\n";
    const auto check = [&](heimdall::FormatOptions options, std::string_view expected) {
        options.blank_line_between_methods = false;
        const heimdall::Formatter formatter(options);
        const auto                formatted = formatter.Format(source);
        EXPECT_EQ(formatted, expected);
        EXPECT_EQ(formatter.Format(formatted), formatted);
        EXPECT_TRUE(formatter.FormatEdits(formatted).empty());
    };
    check({},
          "const SemanticModel& AnalysisContext::Semantic() const\n{\n    return model;\n}\n"
          "ns::Model&& ns::Context::Take()\n{\n    return model;\n}\n"
          "const ns::Model* ns::Context::Get()\n{\n    return ptr;\n}\n"
          "x = a & ns::value;\nx = a * ns::Make();\nx = a && ns::Ready();\n");
    check({ .pointer_alignment   = heimdall::PointerAlignment::Right,
            .reference_alignment = heimdall::ReferenceAlignment::Right },
          "const SemanticModel &AnalysisContext::Semantic() const\n{\n    return model;\n}\n"
          "ns::Model &&ns::Context::Take()\n{\n    return model;\n}\n"
          "const ns::Model *ns::Context::Get()\n{\n    return ptr;\n}\n"
          "x = a & ns::value;\nx = a * ns::Make();\nx = a && ns::Ready();\n");
}

TEST(FormatterSpec, AlignsPointersAndReferencesIndependently)
{
    constexpr std::string_view source =
        "int*x;\nint &r=x;\nvoid f(int&&v);\nint*const p=nullptr;\n";

    const auto check = [](heimdall::FormatOptions options, std::string_view expected) {
        const heimdall::Formatter formatter(options);
        const std::string         formatted = formatter.Format(source);
        EXPECT_EQ(formatted, expected);
        EXPECT_EQ(formatter.Format(formatted), formatted);
    };

    // Right: both bind to the declarator name.
    check({ .pointer_alignment   = heimdall::PointerAlignment::Right,
            .reference_alignment = heimdall::ReferenceAlignment::Right },
          "int *x;\nint &r = x;\nvoid f(int &&v);\nint *const p = nullptr;\n");
    // Split: pointers to the type, references to the name.
    check({ .pointer_alignment   = heimdall::PointerAlignment::Left,
            .reference_alignment = heimdall::ReferenceAlignment::Right },
          "int* x;\nint &r = x;\nvoid f(int &&v);\nint * const p = nullptr;\n");
    check({ .pointer_alignment   = heimdall::PointerAlignment::Right,
            .reference_alignment = heimdall::ReferenceAlignment::Left },
          "int *x;\nint& r = x;\nvoid f(int&& v);\nint *const p = nullptr;\n");
    check({ .pointer_alignment   = heimdall::PointerAlignment::Left,
            .reference_alignment = heimdall::ReferenceAlignment::Left },
          "int* x;\nint& r = x;\nvoid f(int&& v);\nint * const p = nullptr;\n");
}

TEST(FormatterSpec, BreaksLongLinesAtCommas)
{
    // Isolate width-driven wrapping from the parameter-count rule.
    const heimdall::Formatter  narrow({ .column_limit = 40, .max_parameters_per_line = 0 });
    constexpr std::string_view source    = "void f(int alpha, int beta, int gamma, int delta);\n";
    const std::string          formatted = narrow.Format(source);
    EXPECT_EQ(formatted,
              "void f(int alpha, int beta, int gamma,\n"
              "    int delta);\n");
    EXPECT_EQ(narrow.Format(formatted), formatted);
    // Disabled limit keeps the line whole.
    const heimdall::Formatter unlimited({ .column_limit = 0, .max_parameters_per_line = 0 });
    EXPECT_EQ(unlimited.Format(source), source);
}

TEST(FormatterSpec, NormalizesAndAlignsTrailingComments)
{
    constexpr std::string_view source = "int x;   // comment\nint y; //c2\n";
    const heimdall::Formatter  formatter;
    const std::string          formatted = formatter.Format(source);
    EXPECT_EQ(formatted, "int x; // comment\nint y; // c2\n");
    EXPECT_EQ(formatter.Format(formatted), formatted);

    constexpr std::string_view run = "int longer_name; // first\nint x; // second\n";
    EXPECT_EQ(formatter.Format(run), "int longer_name; // first\nint x;           // second\n");
}

TEST(FormatterSpec, SortsIncludesWhenEnabled)
{
    constexpr std::string_view source =
        "#include <vector>\n"
        "#include <string>\n"
        "#include \"b.h\"\n"
        "#include \"a.h\"\n";
    const heimdall::Formatter plain;
    EXPECT_EQ(plain.Format(source), source); // off by default: verbatim
    const heimdall::Formatter sorter({ .sort_includes = true });
    const std::string         expected =
        "#include \"a.h\"\n"
        "#include \"b.h\"\n"
        "#include <string>\n"
        "#include <vector>\n";
    const std::string sorted = sorter.Format(source);
    EXPECT_EQ(sorted, expected);
    EXPECT_EQ(sorter.Format(sorted), sorted);
}

TEST(FormatterSpec, ReportsNoEditsWhenAlreadyFormatted)
{
    const heimdall::Formatter  formatter({ .brace_style = heimdall::BraceStyle::Attach });
    constexpr std::string_view clean = "int main() {\n    return 0;\n}\n";
    EXPECT_TRUE(formatter.FormatEdits(clean).empty());
    constexpr std::string_view messy = "int main() {\nreturn 0;\n}\n";
    const auto                 edits = formatter.FormatEdits(messy);
    ASSERT_FALSE(edits.empty());
    // Edits are disjoint, ascending, and reproduce Format().
    std::string applied(messy);
    for (auto it = edits.rbegin(); it != edits.rend(); ++it)
    {
        applied.replace(it->start_offset, it->end_offset - it->start_offset, it->replacement);
    }

    EXPECT_EQ(applied, formatter.Format(messy));
    for (std::size_t k = 1; k < edits.size(); ++k)
    {
        EXPECT_LE(edits[k - 1].end_offset, edits[k].start_offset);
    }
}

TEST(FormatterSpec, FormatsOnlyTheRequestedRange)
{
    const heimdall::Formatter formatter;
    // Blank lines split the diff into one hunk per statement.
    constexpr std::string_view source = "int a=1;\n\nint b=2;\n\nint c=3;\n";
    // Only line 2 is in range: the other lines stay byte-identical.
    const std::string ranged = formatter.FormatRange(source, 2, 2);
    EXPECT_EQ(ranged, "int a=1;\n\nint b = 2;\n\nint c=3;\n");
    EXPECT_EQ(formatter.FormatRange(source, 0, 4), formatter.Format(source));
    EXPECT_EQ(formatter.FormatRange(formatter.Format(source), 0, 4), formatter.Format(source));
}

TEST(FormatterSpec, LeavesMultilineCommentsAndRawStringsVerbatim)
{
    // Block comment and raw string interiors are copied byte-for-byte: no
    // indentation, spacing or brace tracking may touch them (indenting a raw
    // string would change its value).
    constexpr std::string_view source =
        "void f() {\n"
        "/* { open\n"
        "} close */\n"
        "auto s = R\"(\n"
        "{ not code }\n"
        ")\";\n"
        "int y=2;\n"
        "}\n";
    const heimdall::Formatter formatter({ .brace_style = heimdall::BraceStyle::Attach });
    const std::string         expected =
        "void f() {\n"
        "/* { open\n"
        "} close */\n"
        "auto s = R\"(\n"
        "{ not code }\n"
        ")\";\n"
        "    int y = 2;\n"
        "}\n";
    const std::string formatted = formatter.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, IsIdempotentOnEveryBenchmarkCorpusFile)
{
    const auto corpus = std::filesystem::path(HEIMDALL_SOURCE_DIR) / "bench" / "corpus";
    ASSERT_TRUE(std::filesystem::exists(corpus));
    const heimdall::Formatter formatter;
    for (const auto& entry : std::filesystem::directory_iterator(corpus))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        std::ifstream     file(entry.path(), std::ios::binary);
        const std::string source(std::istreambuf_iterator<char>(file), {});
        const std::string once = formatter.Format(source);
        EXPECT_EQ(formatter.Format(once), once) << entry.path().string();
    }
}

TEST(FormatterSpec, AddsBracesToSingleLineBlocks)
{
    // Attach pinned: this test isolates single_line_style (Allman
    // combinations have their own tests below).
    const heimdall::Formatter braced(
        { .brace_style       = heimdall::BraceStyle::Attach,
          .single_line_style = heimdall::SingleLineStyle::IndentWithBraces });
    EXPECT_EQ(braced.Format("if (x) return;\n"), "if (x) {\n    return;\n}\n");
    EXPECT_EQ(braced.Format("if (a) if (b) x();\n"),
              "if (a) {\n    if (b) {\n        x();\n    }\n}\n");
    EXPECT_EQ(braced.Format("for (int i = 0; i < n; ++i) sum += i;\n"),
              "for (int i = 0; i < n; ++i) {\n    sum += i;\n}\n");
    EXPECT_EQ(braced.Format("while (x) x = f();\n"), "while (x) {\n    x = f();\n}\n");
    EXPECT_EQ(braced.Format("if (a) x(); else y();\n"),
              "if (a) {\n    x();\n} else {\n    y();\n}\n");
    // Already braced, empty bodies and non-control blocks are untouched
    // (`;` still attaches to `)` per the spacing rules).
    EXPECT_EQ(braced.Format("if (a) {\n    x();\n}\n"), "if (a) {\n    x();\n}\n");
    EXPECT_EQ(braced.Format("if (a) ;\n"), "if (a);\n");
    EXPECT_EQ(braced.Format("void f() { x(); }\n"), "void f() {\n    x();\n}\n");
    const std::string nested = braced.Format("if (a) if (b) x();\n");
    EXPECT_EQ(braced.Format(nested), nested);
}

TEST(FormatterSpec, SplitsSingleLineBlocksWithIndent)
{
    // Attach pinned: this test isolates single_line_style.
    const heimdall::Formatter split({ .brace_style       = heimdall::BraceStyle::Attach,
                                      .single_line_style = heimdall::SingleLineStyle::Indent });
    EXPECT_EQ(split.Format("if (x) return;\n"), "if (x)\n    return;\n");
    // Braced singles lose the braces instead of gaining indentation.
    EXPECT_EQ(split.Format("if (x) {\n    return;\n}\n"), "if (x)\n    return;\n");
    EXPECT_EQ(split.Format("if (a) if (b) x();\n"), "if (a)\n    if (b)\n        x();\n");
    EXPECT_EQ(split.Format("while (x) x = f();\n"), "while (x)\n    x = f();\n");
    // `else if` chains stay together; the inner header splits on its own.
    EXPECT_EQ(split.Format("if (a) x(); else if (b) y();\n"),
              "if (a)\n    x();\nelse if (b)\n    y();\n");
    const std::string nested = split.Format("if (a) if (b) x();\n");
    EXPECT_EQ(split.Format(nested), nested);
}

TEST(FormatterSpec, CollapsesSingleLineBlocks)
{
    // Attach pinned: this test isolates single_line_style.
    const heimdall::Formatter flat({ .brace_style       = heimdall::BraceStyle::Attach,
                                     .single_line_style = heimdall::SingleLineStyle::SingleLine });
    EXPECT_EQ(flat.Format("if (x) {\n    return;\n}\n"), "if (x) return;\n");
    EXPECT_EQ(flat.Format("if (x)\n    return;\n"), "if (x) return;\n");
    EXPECT_EQ(flat.Format("while (x) {\n    x = f();\n}\n"), "while (x) x = f();\n");
    // Dangling else, declarations, empty blocks and multi-statement bodies
    // keep their braces: removing them would change meaning or legality.
    EXPECT_EQ(flat.Format("if (a) {\n    if (b) x();\n} else y();\n"),
              "if (a) {\n    if (b) x();\n} else y();\n");
    EXPECT_EQ(flat.Format("if (a) {\n    int x = 1;\n}\n"), "if (a) {\n    int x = 1;\n}\n");
    EXPECT_EQ(flat.Format("if (a) {}\n"), "if (a) {}\n");
    EXPECT_EQ(flat.Format("if (a) {\n    x();\n    y();\n}\n"),
              "if (a) {\n    x();\n    y();\n}\n");
    const std::string collapsed = flat.Format("if (x) {\n    return;\n}\n");
    EXPECT_EQ(flat.Format(collapsed), collapsed);
}

TEST(FormatterSpec, BracesElseIfChainsWithoutNesting)
{
    const heimdall::Formatter formatter(
        { .brace_style       = heimdall::BraceStyle::Attach,
          .single_line_style = heimdall::SingleLineStyle::IndentWithBraces });
    const std::string formatted =
        formatter.Format("void f() {\nif (a) x();\nelse if (b) y();\nelse return;\n}\n");
    EXPECT_EQ(formatted,
              "void f() {\n    if (a) {\n        x();\n    } else if (b) {\n        y();\n"
              "    } else {\n        return;\n    }\n}\n");
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, KeepsStackedPointersTogether)
{
    const heimdall::Formatter formatter({ .brace_style = heimdall::BraceStyle::Attach });
    EXPECT_EQ(formatter.Format("int main(int argc, char **argv, const char *const *p);\n"),
              "int main(int argc, char**argv, const char* const* p);\n");
    EXPECT_EQ(formatter.Format("void f() { x = a * *p; }\n"), "void f() {\n    x = a * *p;\n}\n");
}

TEST(FormatterSpec, BlankLineAfterControlBlock)
{
    const heimdall::Formatter  formatter({ .brace_style       = heimdall::BraceStyle::Attach,
                                           .single_line_style = heimdall::SingleLineStyle::Keep,
                                           .blank_line_after_control_block = true });
    constexpr std::string_view source =
        "void f() {\n"
        "if (a) {\n"
        "x();\n"
        "} else {\n"
        "y();\n"
        "}\n"
        "z();\n"
        "while (b) {\n"
        "w();\n"
        "}\n"
        "do {\n"
        "v();\n"
        "} while (c);\n"
        "// note\n"
        "g();\n"
        "if (d) {\n"
        "h();\n"
        "}\n"
        "}\n";
    const std::string expected =
        "void f() {\n"
        "    if (a) {\n"
        "        x();\n"
        "    } else {\n"
        "        y();\n"
        "    }\n"
        "\n"
        "    z();\n"
        "    while (b) {\n"
        "        w();\n"
        "    }\n"
        "\n"
        "    do {\n"
        "        v();\n"
        "    } while (c);\n"
        "\n"
        "    // note\n"
        "    g();\n"
        "    if (d) {\n"
        "        h();\n"
        "    }\n"
        "}\n";
    const std::string formatted = formatter.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
    // Disabled: nothing is inserted.
    EXPECT_EQ(heimdall::Formatter({ .brace_style                    = heimdall::BraceStyle::Attach,
                                    .blank_line_after_control_block = false })
                  .Format("void f() {\nif (a) {\nx();\n}\ny();\n}\n"),
              "void f() {\n    if (a) {\n        x();\n    }\n    y();\n}\n");
}

TEST(FormatterSpec, KeepsEnumeratorsAndInitializerListsAtOneLevel)
{
    const heimdall::Formatter formatter({ .blank_line_after_control_block = false });
    const std::string enum_source = "enum class Command\n{\nLint,\nCheck,\nFormat,\nParse\n};\n";
    const std::string enum_expected =
        "enum class Command\n{\n    Lint,\n    Check,\n    Format,\n    Parse\n};\n";
    EXPECT_EQ(formatter.Format(enum_source), enum_expected);
    EXPECT_EQ(formatter.Format(enum_expected), enum_expected);
    const std::string list = "int a[] = {\n    1,\n    2\n};\n";
    EXPECT_EQ(formatter.Format(list), list);
    // Argument and declarator commas still continue the statement.
    EXPECT_EQ(formatter.Format("void f(int a,\nint b);\n"), "void f(int a,\n    int b);\n");
}

TEST(FormatterSpec, KeepsMemberAndVariableBraceInitializersAttached)
{
    const heimdall::Formatter formatter({ .blank_line_after_control_block = false,
                                          .space_before_cpp11_braced_list = false });
    const std::string source = "struct Options {\nCommand command{};\nint x{1};\nstd::vector<int> "
                               "v{};\nstd::string s {};\n};\n";
    const std::string expected =
        "struct Options\n{\n    Command command{};\n    int x{1};\n    std::vector<int> v{};\n"
        "    std::string s{};\n};\n";
    EXPECT_EQ(formatter.Format(source), expected);
    EXPECT_EQ(formatter.Format(expected), expected);
    // Headers with a base clause or underlying type still open a block.
    EXPECT_EQ(formatter.Format("enum E : unsigned int { A };\n"),
              "enum E : unsigned int\n{\n    A\n};\n");
}

TEST(FormatterSpec, SpacesTernaryOperatorsOnBothSides)
{
    const heimdall::Formatter formatter(
        { .brace_style = heimdall::BraceStyle::Attach, .blank_line_after_control_block = false });
    const auto check = [&](std::string_view source, std::string_view expected) {
        const std::string formatted = formatter.Format(source);
        EXPECT_EQ(formatted, expected);
        EXPECT_EQ(formatter.Format(formatted), formatted);
    };
    check("x = a?b:c;\n", "x = a ? b : c;\n");
    check("x = (a)?b:c;\n", "x = (a) ? b : c;\n");
    check("x = a ?b :c;\n", "x = a ? b : c;\n");
    check("x = a?f(1):g(2);\n", "x = a ? f(1) : g(2);\n");
    check("x = a?b::c:d;\n", "x = a ? b::c : d;\n");
    check("x = a?y?1:2:3;\n", "x = a ? y ? 1 : 2 : 3;\n");
    check("x = f(a?\"x\":\"y\", d);\n", "x = f(a ? \"x\" : \"y\", d);\n");
    check("return a?b:c;\n", "return a ? b : c;\n");
    // Non-ternary colons keep their own rules.
    check("switch (x) {\ncase 1: break;\n}\n", "switch (x) {\ncase 1:\n    break;\n}\n");
    check("struct S { int b:3; };\n", "struct S {\n    int b: 3;\n};\n");
}

TEST(FormatterSpec, NeverBreaksEmptyBracePairs)
{
    const heimdall::Formatter allman(
        { .brace_style = heimdall::BraceStyle::Allman, .blank_line_after_control_block = false });
    const auto check = [&](std::string_view source, std::string_view expected) {
        const std::string formatted = allman.Format(source);
        EXPECT_EQ(formatted, expected);
        EXPECT_EQ(allman.Format(formatted), formatted);
    };
    // Constructor with an init list whose empty body was already on its own line.
    check("class F {\npublic:\nexplicit F(O o = {}) : m(o)\n{}\n};\n",
          "class F\n{\npublic:\n    explicit F(O o = {}) : m(o) {}\n};\n");
    check("F() {}\nvoid g(){}\nvoid h()\n{\n}\n", "F() {}\n\nvoid g() {}\n\nvoid h() {}\n");
    check("namespace n {}\nstruct S {};\n", "namespace n {}\nstruct S {};\n");
    check("void h() { if (x) {} else {} }\n", "void h()\n{\n    if (x) {}\n    else {}\n}\n");
    // Non-empty bodies still break in Allman.
    check("void f() { x(); }\n", "void f()\n{\n    x();\n}\n");
    // A comment between the braces keeps the pair apart.
    check("void f() {\n// todo\n}\n", "void f()\n{\n    // todo\n}\n");
}

TEST(FormatterSpec, BlankLineAfterTypeDefinitions)
{
    const heimdall::Formatter formatter(
        { .brace_style = heimdall::BraceStyle::Attach, .blank_line_after_type_definition = true });
    constexpr std::string_view source =
        "struct A {\nint x;\n};\n"
        "class B {\npublic:\nstruct In {\nint y;\n};\nint z;\n};\n"
        "enum class E {\nP,\nQ\n};\n"
        "union U {\nint i;\n};\n"
        "struct F;\n"
        "int f() {\nreturn 1;\n}\n";
    const std::string expected =
        "struct A {\n    int x;\n};\n\n"
        "class B {\npublic:\n    struct In {\n        int y;\n    };\n\n    int z;\n};\n\n"
        "enum class E {\n    P,\n    Q\n};\n\n"
        "union U {\n    int i;\n};\n\n"
        "struct F;\n"
        "int f() {\n    return 1;\n}\n";
    const std::string formatted = formatter.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
    // Disabled: definitions stay tight.
    EXPECT_EQ(heimdall::Formatter({ .brace_style = heimdall::BraceStyle::Attach,
                                    .blank_line_after_type_definition = false })
                  .Format("struct A {\nint x;\n};\nint y;\n"),
              "struct A {\n    int x;\n};\nint y;\n");
    // The end of the enclosing scope never gets a trailing blank line.
    EXPECT_EQ(formatter.Format("namespace n {\nstruct A {\nint x;\n};\n}\n"),
              "namespace n {\n    struct A {\n        int x;\n    };\n}\n");
}

TEST(FormatterSpec, SeparatesConsecutiveMethodsAndKeepsDocumentationAttached)
{
    const heimdall::Formatter formatter;
    const std::string         source =
        "class Model {\npublic:\n"
        "const ParseTree& Tree() const noexcept\n{\nreturn *m_tree;\n}\n"
        "/// The names.\nconst InternPool& Names() const noexcept\n{\nreturn m_names;\n}\n};\n";
    const std::string expected =
        "class Model\n{\npublic:\n"
        "    const ParseTree& Tree() const noexcept\n    {\n        return *m_tree;\n    }\n\n"
        "    /// The names.\n    const InternPool& Names() const noexcept\n    {\n        return "
        "m_names;\n    }\n};\n";
    EXPECT_EQ(formatter.Format(source), expected);
    EXPECT_EQ(formatter.Format(expected), expected);
    EXPECT_EQ(formatter.Format(heimdall::ParseTree::Parse(source, {})), expected);
    EXPECT_TRUE(formatter.FormatEdits(expected).empty());
    const std::string fragment = "const ParseTree& Tree() const noexcept { return *m_tree; }\n"
                                 "const InternPool& Names() const noexcept { return m_names; }\n";
    const std::string fragment_expected =
        "const ParseTree& Tree() const noexcept\n{\n    return *m_tree;\n}\n\n"
        "const InternPool& Names() const noexcept\n{\n    return m_names;\n}\n";
    EXPECT_EQ(formatter.Format(fragment), fragment_expected);
    EXPECT_EQ(formatter.Format(fragment_expected), fragment_expected);
}

TEST(FormatterSpec, MethodSeparationCanBeDisabledAndDoesNotSeparateFields)
{
    const std::string source = "struct S {\nvoid first() {}\nvoid second() {}\nint value;\n};\n";
    const heimdall::Formatter formatter;
    EXPECT_EQ(formatter.Format(source),
              "struct S\n{\n    void first() {}\n\n    void second() {}\n\n    int value;\n};\n");
    EXPECT_EQ(heimdall::Formatter({ .blank_line_between_methods = false }).Format(source),
              "struct S\n{\n    void first() {}\n    void second() {}\n    int value;\n};\n");
}

TEST(FormatterSpec, BreaksMoreThanThreeParametersOnePerLine)
{
    const heimdall::Formatter formatter({ .column_limit = 0 });
    const std::string         source =
        "class ParseTree {\nstatic ParseTree Parse(std::string_view source, const ParserOptions& "
        "options,\n"
        "std::stop_token stop, const std::vector<Token>* lexed = nullptr,\n"
        "const ParseReuse* reuse = nullptr);\n};\n";
    const std::string expected =
        "class ParseTree\n{\n    static ParseTree Parse(\n"
        "        std::string_view source,\n        const ParserOptions& options,\n"
        "        std::stop_token stop,\n        const std::vector<Token>* lexed = nullptr,\n"
        "        const ParseReuse* reuse = nullptr);\n};\n";
    EXPECT_EQ(formatter.Format(source), expected);
    EXPECT_EQ(formatter.Format(expected), expected);
    EXPECT_EQ(formatter.Format(heimdall::ParseTree::Parse(source, {})), expected);
}

TEST(FormatterSpec, WrapsConstructorDeclarationsAndSeparatesPublicMethods)
{
    const heimdall::Formatter formatter({ .column_limit = 0 });
    const std::string         source =
        "class DiagnosticSink {\npublic:\n"
        "DiagnosticSink(const AnalysisContext& context, const RuleDescriptor& rule,\n"
        "std::vector<Diagnostic>& diagnostics, RuleMetrics& metrics, const LineTable& lines);\n"
        "// Validates bounds.\nbool Emit(Diagnostic diagnostic);\n"
        "bool Emit(SourceRange range, std::string message);\nprivate:\n"
        "const AnalysisContext& m_context;\nconst RuleDescriptor& m_rule;\n};\n";
    const std::string expected =
        "class DiagnosticSink\n{\npublic:\n    DiagnosticSink(\n"
        "        const AnalysisContext& context,\n        const RuleDescriptor& rule,\n"
        "        std::vector<Diagnostic>& diagnostics,\n        RuleMetrics& metrics,\n"
        "        const LineTable& lines);\n\n"
        "    // Validates bounds.\n    bool Emit(Diagnostic diagnostic);\n\n"
        "    bool Emit(SourceRange range, std::string message);\n\nprivate:\n"
        "    const AnalysisContext& m_context;\n    const RuleDescriptor& m_rule;\n};\n";
    EXPECT_EQ(formatter.Format(source), expected);
    EXPECT_EQ(formatter.Format(expected), expected);
    EXPECT_EQ(formatter.Format(heimdall::ParseTree::Parse(source, {})), expected);
    EXPECT_TRUE(formatter.FormatEdits(expected).empty());
}

TEST(FormatterSpec, SeparatesFieldsMethodsAndAccessSectionsWithoutSplittingFields)
{
    const heimdall::Formatter formatter({ .column_limit = 0 });
    const std::string         source =
        "struct RuleMetrics {\nstd::string code;\nbool failed = false;\n"
        "double AverageNs() const noexcept { return failed ? 0 : 1; }\n"
        "private:\nint a;\nint b;\n/// Sets the count.\nvoid Set(int value);\n"
        "int count;\nprotected:\nvoid Reset();\n};\n";
    const std::string expected =
        "struct RuleMetrics\n{\n    std::string code;\n    bool failed = false;\n\n"
        "    double AverageNs() const noexcept\n    {\n        return failed ? 0 : 1;\n    }\n\n"
        "private:\n    int a;\n    int b;\n\n    /// Sets the count.\n    void Set(int value);\n\n"
        "    int count;\n\nprotected:\n    void Reset();\n};\n";
    EXPECT_EQ(formatter.Format(source), expected);
    EXPECT_EQ(formatter.Format(expected), expected);
}

TEST(FormatterSpec, SeparatesMethodDeclarationsFromEachOtherAndDefinitions)
{
    const heimdall::Formatter formatter({ .column_limit = 0 });
    const std::string         source =
        "class Scheduler {\npublic:\n"
        "std::expected<void, std::string> Register(ScheduledRule rule);\n"
        "ScheduledResult Analyze(const AnalysisContext& context, const RuleEngine& engine, bool "
        "profiling = false, std::stop_token stop = {}) const;\n"
        "std::size_t Size() const noexcept { return m_rules.size(); }\n};\n";
    const std::string expected =
        "class Scheduler\n{\npublic:\n"
        "    std::expected<void, std::string> Register(ScheduledRule rule);\n\n"
        "    ScheduledResult Analyze(\n        const AnalysisContext& context,\n        const "
        "RuleEngine& engine,\n"
        "        bool profiling = false,\n        std::stop_token stop = {}) const;\n\n"
        "    std::size_t Size() const noexcept\n    {\n        return m_rules.size();\n    }\n};\n";
    EXPECT_EQ(formatter.Format(source), expected);
    EXPECT_EQ(formatter.Format(expected), expected);
}

TEST(FormatterSpec, WrapsConstructorDefinitionsAndInitializerLists)
{
    const heimdall::Formatter formatter({ .column_limit = 0 });
    for (const std::string prefix : { "", "explicit ", "constexpr ", "explicit(false) " })
    {
        SCOPED_TRACE(prefix);
        const std::string source = "struct S {\n" + prefix +
                                   "S(int a, int b, int c, int d) : value(a) {}\nint value;\n};\n";
        const std::string expected = "struct S\n{\n    " + prefix +
                                     "S(\n        int a,\n        int b,\n        int c,\n        "
                                     "int d) : value(a) {}\n\n    int value;\n};\n";
        EXPECT_EQ(formatter.Format(source), expected);
        EXPECT_EQ(formatter.Format(expected), expected);
    }

    EXPECT_EQ(formatter.Format("struct S { S(int a, int b, int c); };\n"),
              "struct S\n{\n    S(int a, int b, int c);\n};\n");
}

TEST(FormatterSpec, CallableFieldsAndCallInitializersRemainGrouped)
{
    const heimdall::Formatter formatter({ .column_limit = 0 });
    const std::string         source = "struct S {\nint (*callback)(int);\nint count = factory();\n"
                                       "void Run();\nint value;\n};\n";
    const std::string         expected =
        "struct S\n{\n    int(*callback)(int);\n    int count = factory();\n\n"
        "    void Run();\n\n    int value;\n};\n";
    EXPECT_EQ(formatter.Format(source), expected);
    EXPECT_EQ(formatter.Format(expected), expected);
}

TEST(FormatterSpec, DeclarationSpacingCanBeDisabledAndPreservesCrLf)
{
    const std::string         source = "class S {\r\npublic:\r\nS(int a, int b, int c, int d);\r\n"
                                       "void Run();\r\nprivate:\r\nint a;\r\nint b;\r\n};\r\n";
    const heimdall::Formatter formatter({ .column_limit = 0 });
    const auto                formatted = formatter.Format(source);
    EXPECT_NE(formatted.find("int d);\r\n\r\n"), std::string::npos);
    EXPECT_NE(formatted.find("Run();\r\n\r\nprivate:"), std::string::npos);
    EXPECT_EQ(formatter.Format(formatted), formatted);
    EXPECT_EQ(heimdall::Formatter({ .column_limit               = 0,
                                    .blank_line_between_methods = false,
                                    .max_parameters_per_line    = 0 })
                  .Format(source),
              "class S\r\n{\r\npublic:\r\n    S(int a, int b, int c, int d);\r\n"
              "    void Run();\r\nprivate:\r\n    int a;\r\n    int b;\r\n};\r\n");
}

TEST(FormatterSpec, ParameterThresholdDoesNotCountNestedCommasOrChangeCalls)
{
    const heimdall::Formatter formatter({ .column_limit = 0 });
    const std::string         source =
        "void small(std::pair<int, int> pair, int x = call(1, 2), int y = 0);\n"
        "void run() { call(1, 2, 3, 4, 5); }\n";
    EXPECT_EQ(formatter.Format(source),
              "void small(std::pair<int, int> pair, int x = call(1, 2), int y = 0);\n\n"
              "void run()\n{\n    call(1, 2, 3, 4, 5);\n}\n");
    const std::string many =
        "void many(std::pair<int, int> p, int x = call(1, 2), int y = 0, int z = 1);\n";
    const std::string expected = "void many(\n    std::pair<int, int> p,\n    int x = call(1, "
                                 "2),\n    int y = 0,\n    int z = 1);\n";
    EXPECT_EQ(formatter.Format(many), expected);
    EXPECT_EQ(formatter.Format(expected), expected);
}

TEST(FormatterSpec, ParameterWrappingCanBeDisabledOrConfigured)
{
    const std::string source = "void f(int a, int b, int c, int d);\n";
    EXPECT_EQ(
        heimdall::Formatter({ .column_limit = 0, .max_parameters_per_line = 0 }).Format(source),
        source);
    EXPECT_EQ(
        heimdall::Formatter({ .column_limit = 0, .max_parameters_per_line = 4 }).Format(source),
        source);
    EXPECT_EQ(heimdall::Formatter({ .column_limit = 0, .max_parameters_per_line = 2 })
                  .Format("void f(int a, int b, int c);\n"),
              "void f(\n    int a,\n    int b,\n    int c);\n");
}

TEST(FormatterSpec, DeclarationLayoutPreservesCrLfAndDirectives)
{
    const heimdall::Formatter formatter({ .column_limit = 0 });
    const std::string         source =
        "#define FN(a,b,c,d) function(a,b,c,d)\r\nvoid f(int a,int b,int c,int d);\r\n";
    const std::string expected = "#define FN(a,b,c,d) function(a,b,c,d)\r\nvoid f(\r\n    int "
                                 "a,\r\n    int b,\r\n    int c,\r\n    int d);\r\n";
    EXPECT_EQ(formatter.Format(source), expected);
    EXPECT_EQ(formatter.Format(expected), expected);
}

TEST(FormatterSpec, WrapsDefinitionsVariadicsAndKeepsParameterComments)
{
    const heimdall::Formatter formatter({ .column_limit = 0 });
    const std::string source = "void f(int a, /* second */ int b, int c, ...) { use(a); }\n";
    const std::string expected =
        "void f(\n    int a,\n    /* second */ int b,\n    int c,\n    ...)\n{\n    use(a);\n}\n";
    EXPECT_EQ(formatter.Format(source), expected);
    EXPECT_EQ(formatter.Format(expected), expected);
}

TEST(FormatterSpec, MethodSeparationHandlesConstructorsTemplatesAndTrailingComments)
{
    const heimdall::Formatter formatter({ .column_limit = 0 });
    const std::string         source    = "struct S {\nS() {} // constructor\n~S() {}\n"
                                          "template<class T> void f(T value) {}\nvoid g() {}\n};\n";
    const auto                formatted = formatter.Format(source);
    EXPECT_NE(formatted.find("S() {} // constructor\n\n"), std::string::npos);
    EXPECT_NE(formatted.find("~S() {}\n\n"), std::string::npos);
    EXPECT_NE(formatted.find("void f(T value) {}\n\n"), std::string::npos);
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, SeparatesAssignmentFromBitwiseComplement)
{
    const heimdall::Formatter formatter;
    const std::string         source   = "inline constexpr std::uint32_t kNone =~0u;\n";
    const std::string         expected = "inline constexpr std::uint32_t kNone = ~0u;\n";
    EXPECT_EQ(formatter.Format(source), expected);
    EXPECT_EQ(formatter.Format(expected), expected);
    EXPECT_EQ(formatter.Format("void f() { x=~mask; auto y = ~value; }\n"),
              "void f()\n{\n    x = ~mask;\n    auto y = ~value;\n}\n");
    EXPECT_EQ(formatter.Format("struct S { ~S() {} };\n"), "struct S\n{\n    ~S() {}\n};\n");
}

TEST(FormatterSpec, SpacesInheritanceAndUnderlyingTypeColon)
{
    const heimdall::Formatter formatter(
        { .brace_style = heimdall::BraceStyle::Attach, .blank_line_after_type_definition = false });
    const auto check =
        [&](const heimdall::Formatter& f, std::string_view source, std::string_view expected) {
            const std::string formatted = f.Format(source);
            EXPECT_EQ(formatted, expected);
            EXPECT_EQ(f.Format(formatted), formatted);
        };
    check(formatter, "enum class CompletionKind: std::uint8_t { A };\n",
          "enum class CompletionKind : std::uint8_t {\n    A\n};\n");
    check(formatter, "enum E:int { A };\n", "enum E : int {\n    A\n};\n");
    check(formatter, "class B: public A, private C {};\n", "class B : public A, private C {};\n");
    check(formatter, "struct S:Base {};\n", "struct S : Base {};\n");
    // Colons that are not base clauses keep their own rules.
    check(formatter, "struct S {\nint b:3;\npublic:\nint c;\n};\n",
          "struct S {\n    int b: 3;\n\npublic:\n    int c;\n};\n");
    check(formatter, "x = a?b:c;\n", "x = a ? b : c;\n");
    // Disabled: attached colon as before.
    const heimdall::Formatter tight(
        { .brace_style = heimdall::BraceStyle::Attach, .space_before_inheritance_colon = false });
    check(tight, "enum class K : std::uint8_t {};\n", "enum class K: std::uint8_t {};\n");
}

TEST(FormatterSpec, MemberAccessOperatorsTakeNoSpaces)
{
    // `->` after a name or `]` used to be mistaken for a trailing return type
    // whenever a name and `;` followed it, so `a -> b;` kept its spaces.
    constexpr std::string_view source =
        "void f() {\n"
        "a -> b;\n"
        "a . b;\n"
        "a->b -> c();\n"
        "x = p -> q . r [ 0 ] -> s;\n"
        "y = a .* b;\n"
        "z = a ->* b;\n"
        "g(a , b -> c);\n"
        "}\n";
    const std::string expected =
        "void f()\n{\n"
        "    a->b;\n"
        "    a.b;\n"
        "    a->b->c();\n"
        "    x = p->q.r[0]->s;\n"
        "    y = a.*b;\n"
        "    z = a->*b;\n"
        "    g(a, b->c);\n"
        "}\n";
    const heimdall::Formatter formatter;
    const std::string         formatted = formatter.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, TrailingReturnArrowKeepsItsSpacesAfterDeclaratorSuffixes)
{
    const heimdall::Formatter formatter;
    for (const std::string_view declaration : {
             "auto f() -> int;",
             "auto f() const -> int;",
             "auto f() noexcept -> int;",
             "auto f() const noexcept -> int;",
             "auto f() & -> int;",
             "auto f() const && -> int;",
             "auto f(int a) -> Foo;",
             "auto f() -> Foo *;",
             "struct S { auto m() const override -> int; };",
         })
    {
        EXPECT_NE(formatter.Format(std::string(declaration) + "\n").find(" -> "), std::string::npos)
            << declaration;
    }

    const std::string lambda =
        formatter.Format("auto l = [](int v) mutable -> int { return v; };\n");
    EXPECT_NE(lambda.find(") mutable -> int"), std::string::npos) << lambda;
    const std::string bare = formatter.Format("auto l = [] -> int { return 1; };\n");
    EXPECT_NE(bare.find("] -> int"), std::string::npos) << bare;
}

TEST(FormatterSpec, LeavesBackslashContinuedMacroDefinitionsVerbatim)
{
    // Continuation lines are part of the directive: re-indenting or wrapping them
    // would drop the trailing backslash and cut the macro short.
    constexpr std::string_view source =
        "#define LIST(X) \\\n"
        "    X(Spaceship, \"<=>\") X(ShlEq, \"<<=\") X(Shl, \"<<\") X(Le, \"<=\") X(ShrEq, "
        "\">>=\") X(Shr, \">>\") X(Ge, \">=\") \\\n"
        "X(PlusPlus, \"++\") \\\n"
        "      X(PlusEq, \"+=\")\n"
        "int  a ;\n";
    const auto formatted = heimdall::Formatter().Format(source);
    EXPECT_EQ(formatted.substr(0, formatted.find("int")), source.substr(0, source.find("int")));
    EXPECT_EQ(heimdall::Formatter().Format(formatted), formatted);
}

TEST(FormatterSpec, IfConstexprKeepsTheSpaceBeforeTheCondition)
{
    const auto formatted = heimdall::Formatter().Format(
        "int f()\n{\n    if constexpr(sizeof(void*) == 8) { return 1; } else { return 2; }\n}\n");
    EXPECT_EQ(formatted,
              "int f()\n"
              "{\n"
              "    if constexpr (sizeof(void*) == 8)\n"
              "    {\n"
              "        return 1;\n"
              "    }\n"
              "    else\n"
              "    {\n"
              "        return 2;\n"
              "    }\n"
              "}\n");
    EXPECT_EQ(heimdall::Formatter().Format(formatted), formatted);
}

TEST(FormatterSpec, IfConstevalGetsABlockAndASpaceAfterTheBang)
{
    const auto formatted = heimdall::Formatter().Format(
        "void f()\n{\n    if consteval { a(); }\n    if !consteval { b(); }\n}\n");
    EXPECT_NE(formatted.find("if consteval\n"), std::string::npos) << formatted;
    EXPECT_NE(formatted.find("if !consteval\n"), std::string::npos) << formatted;
    EXPECT_EQ(heimdall::Formatter().Format(formatted), formatted);
}

TEST(FormatterSpec, UnnamedPointerTypesFollowThePointerAlignment)
{
    constexpr std::string_view source =
        "auto n = sizeof(void*);\nauto k = static_cast<void*>(p);\nvoid f(char*, int&);\n";
    EXPECT_EQ(heimdall::Formatter().Format(source), source);
    const auto right =
        heimdall::Formatter({ .pointer_alignment   = heimdall::PointerAlignment::Right,
                              .reference_alignment = heimdall::ReferenceAlignment::Right })
            .Format(source);
    EXPECT_NE(right.find("sizeof(void *)"), std::string::npos) << right;
    EXPECT_NE(right.find("static_cast<void *>(p)"), std::string::npos) << right;
}
