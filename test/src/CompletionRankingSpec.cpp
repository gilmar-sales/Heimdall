#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <Heimdall/Completion.hpp>
#include <Heimdall/CompletionRanking.hpp>

namespace
{
    std::size_t Position(
        const std::vector<heimdall::CompletionItem>& items,
        std::string_view label)
    {
        return static_cast<std::size_t>(std::find_if(items.begin(), items.end(),[&](const auto& item)
            {
                return item.label == label;
                }) - items.begin());
    }
}

TEST(CompletionRankingSpec, VisibleLocalOutranksAlphabeticallyEarlierGlobalWithoutPrefix)
{
    const std::string source = "int alphaGlobal; void Run() { int zebraLocal; }";

    const auto items = heimdall::CompletionEngine::Complete(source, source.size() - 1);

    ASSERT_LT(Position(items, "zebraLocal"), items.size());
    ASSERT_LT(Position(items, "alphaGlobal"), items.size());
    EXPECT_LT(Position(items, "zebraLocal"), Position(items, "alphaGlobal"));
}

TEST(CompletionRankingSpec, LocalFuzzyMatchCanOutrankGlobalPrefix)
{
    const std::string source = "int FooGlobal; void Run() { int FarObject; Fo";

    const auto items = heimdall::CompletionEngine::Complete(source, source.size());

    ASSERT_LT(Position(items, "FarObject"), items.size());
    ASSERT_LT(Position(items, "FooGlobal"), items.size());
    EXPECT_LT(Position(items, "FarObject"), Position(items, "FooGlobal"));
}

TEST(CompletionRankingSpec, DirectMembersOutrankAlphabeticallyEarlierInheritedMembers)
{
    const std::string source =
        "struct Base { int alphaInherited; }; struct Derived : Base { int zebraDirect; }; "
        "void Run() { Derived object; object. }";

    const auto items = heimdall::CompletionEngine::Complete(source, source.find("object. ") + 7);

    ASSERT_LT(Position(items, "zebraDirect"), items.size());
    ASSERT_LT(Position(items, "alphaInherited"), items.size());
    EXPECT_LT(Position(items, "zebraDirect"), Position(items, "alphaInherited"));
}

TEST(CompletionRankingSpec, ParametersOutrankGlobalDeclarations)
{
    const std::string source = "int alphaGlobal; void Run(int zebraParameter) { }";

    const auto items = heimdall::CompletionEngine::Complete(source, source.size() - 1);

    ASSERT_LT(Position(items, "zebraParameter"), items.size());
    ASSERT_LT(Position(items, "alphaGlobal"), items.size());
    EXPECT_LT(Position(items, "zebraParameter"), Position(items, "alphaGlobal"));
}

TEST(CompletionRankingSpec, GrowingCandidateTablePreservesEveryLocalAndItsRanking)
{
    constexpr std::size_t kLocalCount = 700;
    std::string source = "int alphaGlobal; void Run() { ";
    for (std::size_t index = 0; index < kLocalCount; ++index)
    {
        source += "int local" + std::to_string(index) + "; ";
    }

    source += '}';

    const auto items = heimdall::CompletionEngine::Complete(source, source.size() - 1);

    for (std::size_t index = 0; index < kLocalCount; ++index)
    {
        const auto position = Position(items, "local" + std::to_string(index));
        ASSERT_LT(position, items.size());
        EXPECT_EQ(items[position].completionScope, heimdall::CompletionScope::Function);
        EXPECT_LT(position, Position(items, "alphaGlobal"));
    }
}

TEST(CompletionRankingSpec, DeprecatedSymbolRanksBelowHealthySymbol)
{
    const std::string source = "[[deprecated]] int appleAncient; int appleZebra; void Run() { apple";

    const auto items = heimdall::CompletionEngine::Complete(source, source.size());

    ASSERT_LT(Position(items, "appleAncient"), items.size());
    ASSERT_LT(Position(items, "appleZebra"), items.size());
    EXPECT_LT(Position(items, "appleZebra"), Position(items, "appleAncient"));
}

TEST(CompletionRankingSpec, MatchScoresRespectPrefixCaseAndSegmentBoundaries)
{
    using heimdall::CompletionRanking;

    EXPECT_GT(CompletionRanking::MatchScore("FooBar", "Foo"),
              CompletionRanking::MatchScore("FooBar", "foo"));
    EXPECT_GT(CompletionRanking::MatchScore("FooBar", "foo"),
              CompletionRanking::MatchScore("FooBar", "FB"));
    EXPECT_GT(CompletionRanking::MatchScore("FooBar", "FB"), 0);
    EXPECT_GT(CompletionRanking::MatchScore("HTTPServer", "HS"), 0);
    EXPECT_EQ(CompletionRanking::MatchScore("explicit", "ec"), 0);
    EXPECT_EQ(CompletionRanking::MatchScore("MACRO", "mac", true), 0);
    EXPECT_EQ(CompletionRanking::MatchScore("MotorCar", "Car", true), 0);
    EXPECT_EQ(CompletionRanking::MatchScore("", ""), 0);
    EXPECT_EQ(CompletionRanking::MatchScore(std::string(257, 'a'), "ab"), 0);
}

TEST(CompletionRankingSpec, ReservedNamesAreDemotedUnlessExplicitlyRequested)
{
    heimdall::CompletionItem reserved;
    reserved.label = "__internal";
    heimdall::CompletionItem regular;
    regular.label = "publicName";

    EXPECT_LT(heimdall::CompletionRanking::Evaluate(reserved, ""),
              heimdall::CompletionRanking::Evaluate(regular, ""));
    EXPECT_GT(heimdall::CompletionRanking::Evaluate(reserved, "__"), 1.0f / 2);
}

TEST(CompletionRankingSpec, IncludeCostIsSoftRatherThanAnAppendOnlyTier)
{
    heimdall::CompletionItem exact;
    exact.label = "Fetch";
    exact.kind = heimdall::CompletionKind::Function;
    heimdall::CompletionItem fuzzy;
    fuzzy.label = "FarThing";
    fuzzy.kind = heimdall::CompletionKind::Function;

    EXPECT_GT(heimdall::CompletionRanking::Evaluate(exact, "Fe", true), 0);
    EXPECT_GT(heimdall::CompletionRanking::Evaluate(exact, "F", true),
              heimdall::CompletionRanking::Evaluate(fuzzy, "FT"));
    EXPECT_LT(heimdall::CompletionRanking::Evaluate(exact, "F", true),
              heimdall::CompletionRanking::Evaluate(exact, "F"));
}

TEST(CompletionRankingSpec, RankingUsesFilterTextAndDeterministicTies)
{
    heimdall::CompletionItem overrideItem;
    overrideItem.label = "void Zebra() override";
    overrideItem.filter_text = "Zebra";
    heimdall::CompletionItem alpha;
    alpha.label = "Alpha";
    heimdall::CompletionItem wrong;
    wrong.label = "Wrong";
    std::vector items {wrong, overrideItem, alpha};

    heimdall::CompletionRanking::Rank(items, "Z");

    ASSERT_EQ(items.size(), 1);
    EXPECT_EQ(items.front().label, overrideItem.label);
    items = {overrideItem, alpha};
    heimdall::CompletionRanking::Rank(items, "");
    EXPECT_EQ(items.front().label, "Alpha");
}
