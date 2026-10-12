#include <Heimdall/MacroScanner.hpp>
#include <Heimdall/MacroTable.hpp>

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace
{

    using heimdall::IncludeRequest;
    using heimdall::MacroScanCallbacks;
    using heimdall::MacroScanner;
    using heimdall::MacroTable;

    // Headers served from memory; `log` records the order they were entered.
    struct Files
    {
        std::map<std::string, std::string, std::less<>> headers;
        std::vector<std::string> log;
        MacroTable* table = nullptr;

        MacroScanCallbacks Callbacks()
        {
            MacroScanCallbacks callbacks;
            callbacks.include =[this](const IncludeRequest& request)
            {
                const auto it = headers.find(request.header);
                if (it == headers.end())
                {
                    return false;
                }

                log.push_back(request.header);
                (void) MacroScanner::Scan(it->second, request.header, *table, Callbacks());
                return true;
            };
            callbacks.hasInclude =[this](const IncludeRequest& request)
            {
                return headers.contains(request.header);
            };
            return callbacks;
        }
    };

    bool Defined(const MacroTable& table, std::string_view name)
    {
        return table.Find(name) != nullptr;
    }

} // namespace

TEST(MacroScannerSpec, AppliesDefinesAndUndefsInOrder)
{
    MacroTable table;

    const auto result = MacroScanner::Scan("#define A 1\n#define B(x) x\n#undef A\n", "t.cpp", table);

    EXPECT_FALSE(Defined(table, "A"));
    ASSERT_TRUE(Defined(table, "B"));
    EXPECT_TRUE(table.Find("B")->functionLike);
    EXPECT_EQ(table.Find("B") -> file, "t.cpp");
    EXPECT_TRUE(result.diagnostics.empty());
}

TEST(MacroScannerSpec, EvaluatesConditionsAgainstTheLiveTable)
{
    MacroTable table;
    constexpr std::string_view text =
        "#define V 3\n#if V >= 3 && defined(V) && !defined(W)\n#define OK 1\n#endif\n"
        "#if V == 4\n#define BAD 1\n#elif V == 3\n#define ELIF 1\n#else\n#define BAD2 1\n#endif\n";

    (void) MacroScanner::Scan(text, "t.cpp", table);

    EXPECT_TRUE(Defined(table, "OK"));
    EXPECT_TRUE(Defined(table, "ELIF"));
    EXPECT_FALSE(Defined(table, "BAD"));
    EXPECT_FALSE(Defined(table, "BAD2"));
}

TEST(MacroScannerSpec, ExpandsFunctionLikeMacrosInConditions)
{
    MacroTable table;
    constexpr std::string_view text =
        "#define VER(a, b) ((a) * 100 + (b))\n#define CUR VER(1, 8)\n#if CUR >= VER(1, 2)\n"
        "#define NEW 1\n#endif\n";

    (void) MacroScanner::Scan(text, "t.cpp", table);

    EXPECT_TRUE(Defined(table, "NEW"));
}

TEST(MacroScannerSpec, SkipsDirectivesInInactiveGroups)
{
    MacroTable table;
    constexpr std::string_view text = "#if 0\n#define HIDDEN 1\n#if 1\n#define NESTED 1\n#endif\n"
        "#else\n#define SHOWN 1\n#endif\n";

    const auto result = MacroScanner::Scan(text, "t.cpp", table);

    EXPECT_FALSE(Defined(table, "HIDDEN"));
    EXPECT_FALSE(Defined(table, "NESTED"));
    EXPECT_TRUE(Defined(table, "SHOWN"));
    EXPECT_GE(result.activeRanges.size(), 2u);
}

TEST(MacroScannerSpec, ReportsInactiveRanges)
{
    MacroTable table;
    constexpr std::string_view text = "int a;\n#if 0\nint b;\n#endif\nint c;\n";

    const auto result = MacroScanner::Scan(text, "t.cpp", table);

    std::string active;
    for (const auto& range : result.activeRanges)
    {
        active += text.substr(range.offset, range.length);
    }

    EXPECT_NE(active.find("int a;"), std::string::npos);
    EXPECT_EQ(active.find("int b;"), std::string::npos);
    EXPECT_NE(active.find("int c;"), std::string::npos);
}

TEST(MacroScannerSpec, MacrosFromAnIncludeAreVisibleToTheFollowingLines)
{
    MacroTable table;
    Files files;
    files.table = &table;
    files.headers["config.h"] = "#define FEATURE 2\n#define EXPORT(x) x\n";
    constexpr std::string_view text = "#include \"config.h\"\n#if FEATURE == 2\n#define ON 1\n#endif\n";

    const auto callbacks = files.Callbacks();
    const auto result = MacroScanner::Scan(text, "main.cpp", table, callbacks);

    EXPECT_TRUE(Defined(table, "ON"));
    EXPECT_TRUE(Defined(table, "EXPORT"));
    EXPECT_EQ(table.Find("EXPORT") -> file, "config.h");
    EXPECT_TRUE(result.diagnostics.empty());
}

TEST(MacroScannerSpec, ReportsMissingHeaders)
{
    MacroTable table;

    const auto result = MacroScanner::Scan("#include <nothing.h>\n", "t.cpp", table);

    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_NE(result.diagnostics[0].message.find("nothing.h"), std::string::npos);
}

TEST(MacroScannerSpec, EvaluatesHasInclude)
{
    MacroTable table;
    Files files;
    files.table = &table;
    files.headers["a.h"] = "";
    constexpr std::string_view text = "#if __has_include(<a.h>) && !__has_include(\"b.h\")\n"
        "#define YES 1\n#endif\n";

    (void) MacroScanner::Scan(text, "t.cpp", table, files.Callbacks());

    EXPECT_TRUE(Defined(table, "YES"));
}

TEST(MacroScannerSpec, ResolvesMacroIncludeOperands)
{
    MacroTable table;
    Files files;
    files.table = &table;
    files.headers["platform.h"] = "#define PLATFORM 1\n";
    constexpr std::string_view text = "#define HEADER \"platform.h\"\n#include HEADER\n";

    (void) MacroScanner::Scan(text, "t.cpp", table, files.Callbacks());

    EXPECT_TRUE(Defined(table, "PLATFORM"));
}

TEST(MacroScannerSpec, DetectsIncludeGuardsAndPragmaOnce)
{
    MacroTable table;

    const auto guarded =
        MacroScanner::Scan("// header\n#ifndef FOO_H\n#define FOO_H\nint x;\n#endif\n\n", "a.h", table);
    const auto once = MacroScanner::Scan("#pragma once\nint y;\n", "b.h", table);
    const auto trailing =
        MacroScanner::Scan("#ifndef BAR_H\n#define BAR_H\n#endif\nint z;\n", "c.h", table);

    EXPECT_EQ(guarded.guard, "FOO_H");
    EXPECT_TRUE(once.pragmaOnce);
    EXPECT_TRUE(trailing.guard.empty());
}

TEST(MacroScannerSpec, JoinsContinuedDirectives)
{
    MacroTable table;

    (void) MacroScanner::Scan("#if 1 && \\\n  1\n#define JOINED 1\n#endif\n", "t.cpp", table);

    EXPECT_TRUE(Defined(table, "JOINED"));
}

TEST(MacroScannerSpec, EvaluatesAttributeFeatureTests)
{
    MacroTable table;

    EXPECT_TRUE(MacroScanner::Evaluate("__has_cpp_attribute(nodiscard) >= 201603", table));
    EXPECT_FALSE(MacroScanner::Evaluate("__has_cpp_attribute(unknown_attr)", table));
}

TEST(MacroScannerSpec, EvaluatesIntegerOperators)
{
    const MacroTable table;

    EXPECT_TRUE(MacroScanner::Evaluate("(1 << 4) == 16 && 7 % 4 == 3 && 0x10 == 16", table));
    EXPECT_TRUE(MacroScanner::Evaluate("1 ? 2 : 0", table));
    EXPECT_TRUE(MacroScanner::Evaluate("'a' == 97 && 0b101 == 5 && 1'000 == 1000", table));
    EXPECT_FALSE(MacroScanner::Evaluate("UNDEFINED_NAME", table));
}

TEST(MacroScannerSpec, DivisionByZeroIsAnErrorNotACrash)
{
    const MacroTable table;
    std::vector<heimdall::PreprocessorDiagnostic> diagnostics;

    const bool value = MacroScanner::Evaluate("1 / 0", table, {}, &diagnostics);

    EXPECT_FALSE(value);
    EXPECT_EQ(diagnostics.size(), 1u);
}

TEST(MacroScannerSpec, DeeplyNestedExpressionsAreRejected)
{
    const MacroTable table;
    const std::string expression = std::string(500, '(') + "1" + std::string(500, ')');

    EXPECT_FALSE(MacroScanner::Evaluate(expression, table));
}

TEST(MacroScannerSpec, StopsWhenCancelled)
{
    MacroTable table;
    std::stop_source source;
    source.request_stop();

    const auto result = MacroScanner::Scan("#define A 1\n", "t.cpp", table, {}, source.get_token());

    EXPECT_TRUE(result.cancelled);
    EXPECT_FALSE(Defined(table, "A"));
}
