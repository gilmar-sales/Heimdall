#include <Heimdall/MacroExpander.hpp>
#include <Heimdall/MacroScanner.hpp>
#include <Heimdall/MacroTable.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace
{

    using Clock = std::chrono::steady_clock;

    constexpr std::size_t kIterations = 3000;
    constexpr std::size_t kTokenCap = 5000;

    std::chrono::milliseconds Since(Clock::time_point start)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    }

    class Generator
    {
    public:
        explicit Generator(unsigned seed) : mRandom(seed) {}

        std::size_t Below(std::size_t bound)
        {
            return mRandom() % bound;
        }

        std::string Name()
        {
            return std::string(kNames[Below(kNames.size())]);
        }

        std::string BodyToken(const std::vector<std::string>& params)
        {
            static constexpr std::array<std::string_view, 16> kPieces = {
                "+", "-", "(", ")", ",", "#", "##", "1", "0x10", "\"s\"", "'c'", "{", "}", ";", "::", "..."
            };
            switch (Below(5))
            {
            case 0:
                return std::string(kPieces[Below(kPieces.size())]);
            case 1:
                return params.empty() ? Name() : params[Below(params.size())];
            case 2:
                return "__VA_ARGS__";
            case 3:
                return "__VA_OPT__";
            default:
                return Name();
            }
        }

        std::string Definition()
        {
            std::vector<std::string> params;
            std::string text = Name();
            if (Below(2) == 0)
            {
                text += '(';
                const std::size_t count = Below(4);
                for (std::size_t i = 0; i < count; ++i)
                {
                    params.push_back("p" + std::to_string(i));
                    text +=(i == 0 ? "" : ", ") + params.back();
                }

                if (Below(3) == 0)
                {
                    text += count == 0 ? "..." : ", ...";
                }

                text += ')';
            }

            text += ' ';
            const std::size_t length = Below(9);
            for (std::size_t i = 0; i < length; ++i)
            {
                text += BodyToken(params) + ' ';
            }

            return text;
        }

        std::string Source()
        {
            std::string text;
            const std::size_t length = 1 + Below(24);
            for (std::size_t i = 0; i < length; ++i)
            {
                switch (Below(7))
                {
                case 0:
                    text += Name() + "(" + Name() + ", " + Name() + ") ";
                    break;
                case 1:
                    text += Name() + "(" + Name() + "(" + Name() + ")) ";
                    break;
                case 2:
                    text += Name() + "( ";
                    break;
                case 3:
                    text += ") , ";
                    break;
                case 4:
                    text += "\"str\" 12 ";
                    break;
                default:
                    text += Name() + " ";
                    break;
                }
            }

            return text;
        }

    private:
        static constexpr std::array<std::string_view, 8> kNames = {
            "A", "B", "C", "F", "G", "H", "defined", "__VA_ARGS__"
        };

        std::mt19937 mRandom;
    };

} // namespace

TEST(MacroFuzzSpec, RandomMacroProgramsExpandWithinTheLimitsAndNeverCrash)
{
    Generator generator(20261012);
    heimdall::ExpansionLimits limits;
    limits.maxTokens = kTokenCap;
    const auto start = Clock::now();
    std::size_t truncated = 0;

    for (std::size_t i = 0; i < kIterations; ++i)
    {
        heimdall::MacroTable table;
        const std::size_t count = 1 + generator.Below(6);
        for (std::size_t m = 0; m < count; ++m)
        {
            if (auto parsed = heimdall::MacroTable::ParseDefinition(generator.Definition()))
            {
                table.Define(std::move(*parsed));
            }
        }

        const std::string source = generator.Source();
        const auto result = heimdall::MacroExpander(table, limits).Expand(source);

        EXPECT_LE(result.tokens.size(), kTokenCap + 64) << source;
        truncated += result.truncated ? 1 : 0;
    }

    EXPECT_LT(Since(start).count(), 20000);
    EXPECT_LT(truncated, kIterations);
}

TEST(MacroFuzzSpec, ExponentialChainsStopAtTheTokenLimitQuickly)
{
    heimdall::MacroTable table;
    table.Define(*heimdall::MacroTable::ParseDefinition("L0 x"));
    for (int level = 1; level <= 60; ++level)
    {
        table.Define(*heimdall::MacroTable::ParseDefinition(
            "L" + std::to_string(level) + " L" + std::to_string(level - 1) + " L" +
            std::to_string(level - 1)));
    }

    heimdall::ExpansionLimits limits;
    limits.maxTokens = 50'000;
    const auto start = Clock::now();

    const auto result = heimdall::MacroExpander(table, limits).Expand("L60");

    EXPECT_TRUE(result.truncated);
    EXPECT_LE(result.tokens.size(), 50'064u);
    EXPECT_LT(Since(start).count(), 2000);
}

TEST(MacroFuzzSpec, MutuallyRecursiveMacrosTerminate)
{
    heimdall::MacroTable table;
    table.Define(*heimdall::MacroTable::ParseDefinition("A B + A"));
    table.Define(*heimdall::MacroTable::ParseDefinition("B A + B"));
    table.Define(*heimdall::MacroTable::ParseDefinition("F(x) G(x) F(x)"));
    table.Define(*heimdall::MacroTable::ParseDefinition("G(x) F(x) G(x)"));

    const auto objects = heimdall::MacroExpander(table).Expand("A B");
    const auto functions = heimdall::MacroExpander(table).Expand("F(1)");

    EXPECT_FALSE(objects.truncated);
    EXPECT_FALSE(functions.truncated);
}

TEST(MacroFuzzSpec, DeeplyNestedArgumentsStayWithinTheDepthLimit)
{
    heimdall::MacroTable table;
    table.Define(*heimdall::MacroTable::ParseDefinition("ID(x) x"));
    std::string source;
    constexpr int kDepth = 5000;
    for (int i = 0; i < kDepth; ++i)
    {
        source += "ID(";
    }

    source += "0";
    source += std::string(kDepth, ')');

    const auto result = heimdall::MacroExpander(table).Expand(source);

    EXPECT_TRUE(result.truncated);
    EXPECT_FALSE(result.diagnostics.empty());
}

TEST(MacroFuzzSpec, UnbalancedAndHostileArgumentListsAreReportedNotFollowed)
{
    heimdall::MacroTable table;
    table.Define(*heimdall::MacroTable::ParseDefinition("F(a, b) a b"));

    for (const std::string_view source :
        {
            "F(", "F((", "F(1", "F(1,", "F(1,2", "F(1,2,3)", "F()", "F(,)", ")F(1,2)", "F F F(1,2"
    })
    {
        const auto result = heimdall::MacroExpander(table).Expand(source);
        EXPECT_LE(result.tokens.size(), 16u) << source;
    }
}

TEST(MacroFuzzSpec, ParseDefinitionAcceptsArbitraryTextWithoutCrashing)
{
    Generator generator(7);
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < kIterations; ++i)
    {
        std::string text;
        const std::size_t length = generator.Below(40);
        for (std::size_t c = 0; c < length; ++c)
        {
            static constexpr std::string_view kAlphabet = "ab_ 1()#,.\\\n\"'/*+";
            text += kAlphabet[generator.Below(kAlphabet.size())];
        }

        accepted += heimdall::MacroTable::ParseDefinition(text).has_value() ? 1 : 0;
    }

    EXPECT_GT(accepted, 0u);
}

TEST(MacroFuzzSpec, DirectiveSoupIsScannedToTheEndWithoutCrashing)
{
    Generator generator(99);
    static constexpr std::array<std::string_view, 16> kLines = {
        "#if 1\n", "#if 0\n", "#if A && (B || C)\n", "#if 1 / 0\n", "#if (((\n", "#elif B\n", "#else\n",
        "#endif\n", "#endif\n", "#define A 1\n", "#define B(x) x\n", "#undef A\n", "#ifdef C\n",
        "#include \"missing.h\"\n", "#if __has_include(<x>) && defined(B)\n", "int x;\n"
    };
    const auto start = Clock::now();

    for (std::size_t i = 0; i < kIterations; ++i)
    {
        std::string text;
        const std::size_t length = generator.Below(30);
        for (std::size_t line = 0; line < length; ++line)
        {
            text += kLines[generator.Below(kLines.size())];
        }

        heimdall::MacroTable table;
        const auto result = heimdall::MacroScanner::Scan(text, "soup.cpp", table);

        EXPECT_FALSE(result.cancelled);
    }

    EXPECT_LT(Since(start).count(), 20000);
}

TEST(MacroFuzzSpec, ConditionExpressionsOfAnySizeAreBounded)
{
    heimdall::MacroTable table;
    std::string deep(2000, '(');
    deep += "1";
    deep += std::string(2000, ')');
    std::string chained = "1";
    for (int i = 0; i < 20000; ++i)
    {
        chained += " + 1";
    }

    const auto start = Clock::now();

    EXPECT_FALSE(heimdall::MacroScanner::Evaluate(deep, table));
    EXPECT_TRUE(heimdall::MacroScanner::Evaluate(chained, table));
    EXPECT_LT(Since(start).count(), 2000);
}
