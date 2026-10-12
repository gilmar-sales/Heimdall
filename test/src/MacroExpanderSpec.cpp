#include <Heimdall/MacroExpander.hpp>
#include <Heimdall/MacroTable.hpp>
#include <Heimdall/Preprocessor.hpp>

#include <gtest/gtest.h>

#include <initializer_list>
#include <stop_token>
#include <string>
#include <string_view>

namespace
{

    using heimdall::MacroExpander;
    using heimdall::MacroTable;

    MacroTable Table(std::initializer_list<std::string_view> definitions)
    {
        MacroTable table;
        for (const std::string_view definition : definitions)
        {
            auto parsed = MacroTable::ParseDefinition(definition);
            EXPECT_TRUE(parsed.has_value()) << definition;
            if (parsed)
            {
                table.Define(std::move(*parsed));
            }
        }

        return table;
    }

    std::string Join(const heimdall::ExpansionResult& result)
    {
        std::string text;
        for (const heimdall::ExpandedToken& token : result.tokens)
        {
            if (!text.empty())
            {
                text += ' ';
            }

            text += token.text;
        }

        return text;
    }

    std::string Expand(const MacroTable& table, std::string_view source)
    {
        return Join(MacroExpander(table).Expand(source));
    }

} // namespace

TEST(MacroExpanderSpec, ReplacesObjectLikeMacros)
{
    const auto table = Table({"N 42", "EMPTY"});

    EXPECT_EQ(Expand(table, "int a = N; EMPTY x"), "int a = 42 ; x");
}

TEST(MacroExpanderSpec, SubstitutesFunctionLikeArguments)
{
    const auto table = Table({"ADD(a, b) ((a) + (b))"});

    EXPECT_EQ(Expand(table, "ADD(1, x * 2)"), "( ( 1 ) + ( x * 2 ) )");
}

TEST(MacroExpanderSpec, LeavesFunctionLikeNameWithoutParenthesesAlone)
{
    const auto table = Table({"F(x) x"});

    EXPECT_EQ(Expand(table, "int F = 1;"), "int F = 1 ;");
}

TEST(MacroExpanderSpec, DoesNotRecurseIntoItself)
{
    const auto table = Table({"foo foo + 1", "A B", "B A"});

    EXPECT_EQ(Expand(table, "foo"), "foo + 1");
    EXPECT_EQ(Expand(table, "A"), "A");
}

TEST(MacroExpanderSpec, StandardExampleNestedFunctionLikeCalls)
{
    const auto table = Table({"x 3", "f(a) f(x * (a))", "z z[0]", "h g(~", "m(a) a(w)", "w 0,1",
            "t(a) a", "p() int", "q(x) x", "r(x, y) x ## y", "str(x) # x"});

    EXPECT_EQ(Expand(table, "f(2)"), "f ( 3 * ( 2 ) )");
    EXPECT_EQ(Expand(table, "z"), "z [ 0 ]");
    EXPECT_EQ(Expand(table, "t(t)(0)"), "t ( 0 )");
}

TEST(MacroExpanderSpec, StandardExampleHashAndPaste)
{
    const auto table = Table({"hash_hash # ## #", "mkstr(a) # a", "in_between(a) mkstr(a)",
            "join(c, d) in_between(c hash_hash d)", "xstr(s) str(s)",
            "str(s) # s", "glue(a, b) a ## b"});

    EXPECT_EQ(Expand(table, "join(x, y)"), "\"x ## y\"");
    EXPECT_EQ(Expand(table, "xstr(1 + 2)"), "\"1 + 2\"");
    EXPECT_EQ(Expand(table, "glue(HIGH, LOW)"), "HIGHLOW");
}

TEST(MacroExpanderSpec, StringifyEscapesLiteralsAndCollapsesSpaces)
{
    const auto table = Table({"str(x) # x"});

    EXPECT_EQ(Expand(table, "str(  a   +\"q\\n\"  )"), "\"a +\\\"q\\\\n\\\"\"");
    EXPECT_EQ(Expand(table, "str()"), "\"\"");
}

TEST(MacroExpanderSpec, PastingWithEmptyArgumentKeepsTheOtherOperand)
{
    const auto table = Table({"cat(a, b) a ## b"});

    EXPECT_EQ(Expand(table, "cat(, x)"), "x");
    EXPECT_EQ(Expand(table, "cat(x, )"), "x");
    EXPECT_EQ(Expand(table, "cat(a b, c d)"), "a bc d");
}

TEST(MacroExpanderSpec, ReportsInvalidPaste)
{
    const auto table = Table({"cat(a, b) a ## b"});

    const auto result = MacroExpander(table).Expand("cat(+, 1)");

    EXPECT_EQ(Join(result), "+ 1");
    ASSERT_EQ(result.diagnostics.size(), 1u);
}

TEST(MacroExpanderSpec, ExpandsVariadicArguments)
{
    const auto table = Table({"LOG(fmt, ...) printf(fmt, __VA_ARGS__)", "NAMED(args...) f(args)"});

    EXPECT_EQ(Expand(table, "LOG(\"%d %d\", 1, 2)"), "printf ( \"%d %d\" , 1 , 2 )");
    EXPECT_EQ(Expand(table, "NAMED(1, 2)"), "f ( 1 , 2 )");
}

TEST(MacroExpanderSpec, VaOptDependsOnVariadicArguments)
{
    const auto table = Table({"F(a, ...) f(a __VA_OPT__(,) __VA_ARGS__)"});

    EXPECT_EQ(Expand(table, "F(1, 2)"), "f ( 1 , 2 )");
    EXPECT_EQ(Expand(table, "F(1)"), "f ( 1 )");
}

TEST(MacroExpanderSpec, KeepsOperandOfDefinedUnexpanded)
{
    const auto table = Table({"FEATURE 1"});

    const auto result = MacroExpander(table).Expand("defined(FEATURE) && FEATURE", true);

    EXPECT_EQ(Join(result), "defined ( FEATURE ) && 1");
}

TEST(MacroExpanderSpec, RejectsWrongArgumentCount)
{
    const auto table = Table({"F(a, b) a b"});

    const auto result = MacroExpander(table).Expand("F(1)");

    EXPECT_EQ(Join(result), "F ( 1 )");
    EXPECT_FALSE(result.diagnostics.empty());
}

TEST(MacroExpanderSpec, ReportsUnterminatedArgumentList)
{
    const auto table = Table({"F(a) a"});

    const auto result = MacroExpander(table).Expand("F(1");

    EXPECT_FALSE(result.diagnostics.empty());
}

TEST(MacroExpanderSpec, RecordsProvenance)
{
    const auto table = Table({"TWICE(x) x x", "K 7"});
    constexpr std::string_view source = "int a = TWICE(K);";

    const auto result = MacroExpander(table).Expand(source);

    ASSERT_GE(result.tokens.size(), 6u);
    const auto& first = result.tokens[3];
    EXPECT_EQ(first.text, "7");
    EXPECT_NE(first.invocation, heimdall::kNoToken);
    EXPECT_EQ(result.tokens[0].invocation, heimdall::kNoToken);
    EXPECT_EQ(result.tokens[0].text, "int");
}

TEST(MacroExpanderSpec, StopsAtTheTokenLimit)
{
    const auto table = Table({"A B B", "B C C", "C D D", "D E E", "E F F", "F x x"});
    heimdall::ExpansionLimits limits;
    limits.maxTokens = 20;

    const auto result = MacroExpander(table, limits).Expand("A");

    EXPECT_TRUE(result.truncated);
    EXPECT_FALSE(result.diagnostics.empty());
}

TEST(MacroExpanderSpec, StopsWhenCancelled)
{
    const auto table = Table({"N 1"});
    std::stop_source source;
    source.request_stop();
    std::string text;
    for (int i = 0; i < 5000; ++i)
    {
        text += "N ";
    }

    const auto result = MacroExpander(table).Expand(text, false, source.get_token());

    EXPECT_TRUE(result.truncated);
}

TEST(MacroTableSpec, ParsesFunctionLikeDefinitions)
{
    const auto parsed = MacroTable::ParseDefinition("F(a, b, ...) a + b");

    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->functionLike);
    EXPECT_TRUE(parsed->variadic);
    EXPECT_EQ(parsed->params.size(), 3u);
    EXPECT_EQ(parsed->body, "a + b");
}

TEST(MacroTableSpec, TreatsParenthesisAfterSpaceAsBody)
{
    const auto parsed = MacroTable::ParseDefinition("OBJ (1)");

    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->functionLike);
    EXPECT_EQ(parsed->body, "(1)");
}

TEST(MacroTableSpec, RejectsMalformedDefinitions)
{
    EXPECT_FALSE(MacroTable::ParseDefinition("").has_value());
    EXPECT_FALSE(MacroTable::ParseDefinition("F(a, a) a").has_value());
    EXPECT_FALSE(MacroTable::ParseDefinition("F(a b) a").has_value());
    EXPECT_FALSE(MacroTable::ParseDefinition("F(a").has_value());
    EXPECT_FALSE(MacroTable::ParseDefinition("F(a) # b").has_value());
    EXPECT_FALSE(MacroTable::ParseDefinition("X ## 1").has_value());
}

TEST(MacroTableSpec, JoinsLineContinuationsAndDropsComments)
{
    const auto parsed = MacroTable::ParseDefinition("M(a) \\\n  a /* c */ + 1 // tail");

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->tokens.size(), 3u);
}

TEST(MacroTableSpec, ParsesCommandLineDefines)
{
    const auto flag = MacroTable::ParseCommandLine("DEBUG");
    const auto value = MacroTable::ParseCommandLine("LEVEL=3");

    ASSERT_TRUE(flag.has_value());
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(flag->body, "1");
    EXPECT_EQ(value->body, "3");
    EXPECT_EQ(value->origin, heimdall::MacroOrigin::CommandLine);
}

TEST(MacroTableSpec, FrozenSnapshotsShareHistoryAndIgnoreLaterChanges)
{
    MacroTable table = Table({"A 1", "B 2"});
    const MacroTable before = table.Freeze();

    table.Undef("A");
    table.Define(*MacroTable::ParseDefinition("C 3"));

    EXPECT_NE(before.Find("A"), nullptr);
    EXPECT_EQ(before.Find("C"), nullptr);
    EXPECT_EQ(table.Find("A"), nullptr);
    EXPECT_NE(table.Find("B"), nullptr);
    EXPECT_EQ(table.Size(), 2u);
    EXPECT_EQ(before.Size(), 2u);
}

TEST(MacroTableSpec, SurvivesManyLayers)
{
    MacroTable table;
    for (int i = 0; i < 100; ++i)
    {
        table.Define(*MacroTable::ParseDefinition("M" + std::to_string(i) + " " + std::to_string(i)));
        (void) table.Freeze();
    }

    EXPECT_EQ(table.Size(), 100u);
    EXPECT_NE(table.Find("M0"), nullptr);
    EXPECT_NE(table.Find("M99"), nullptr);
}

TEST(MacroTableSpec, EqualDefinitionsHashEqually)
{
    const auto a = MacroTable::ParseDefinition("F(x) x + 1");
    const auto b = MacroTable::ParseDefinition("F(x)   x  +   1");
    const auto c = MacroTable::ParseDefinition("F(x) x + 2");

    EXPECT_EQ(a->hash, b->hash);
    EXPECT_NE(a->hash, c->hash);
}

TEST(PreprocessorMacroEventsSpec, RecordsFunctionLikeDefinesAndUndefsInOrder)
{
    const heimdall::Preprocessor preprocessor;

    const auto result = preprocessor.Process("#define F(a) a\n#define N 1\n#undef N\n");

    ASSERT_EQ(result.macro_events.size(), 3u);
    EXPECT_TRUE(result.macro_events[0].define);
    EXPECT_EQ(result.macro_events[0].text, "F(a) a");
    EXPECT_FALSE(result.macro_events[2].define);
    EXPECT_EQ(result.macro_events[2].text, "N");
}

TEST(MacroExpanderSpec, PreservesTheSpellingGapsOfTheSource)
{
    const auto table = Table({"ADD(a, b) a+b"});

    const auto result = MacroExpander(table).Expand("x = ADD(1, 2);");

    ASSERT_EQ(result.tokens.size(), 6u);
    EXPECT_TRUE(result.tokens[1].spaceBefore);
    EXPECT_TRUE(result.tokens[2].spaceBefore);
    EXPECT_FALSE(result.tokens[3].spaceBefore);
    EXPECT_FALSE(result.tokens[4].spaceBefore);
    EXPECT_FALSE(result.tokens[5].spaceBefore);
}
