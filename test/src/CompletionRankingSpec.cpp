#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <Heimdall/Completion.hpp>
#include <Heimdall/CompletionRanking.hpp>
#include <Heimdall/AnalysisFeatures.hpp>
#include <Heimdall/Workspace.hpp>

namespace
{
    std::vector<heimdall::CompletionItem> CompleteAt(std::string source)
    {
        const auto offset = source.find('^');
        source.erase(offset, 1);
        return heimdall::CompletionEngine::Complete(source, offset);
    }

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

TEST(CompletionRankingSpec, TypeContextPrefersTypesOverAlphabeticallyEarlierValues)
{
    const auto items = CompleteAt("int alphaValue; struct zebraType {}; void Run() { new ^; }");

    ASSERT_LT(Position(items, "zebraType"), items.size());
    ASSERT_LT(Position(items, "alphaValue"), items.size());
    EXPECT_LT(Position(items, "zebraType"), Position(items, "alphaValue"));
}

TEST(CompletionRankingSpec, NamespaceContextPrefersNamespacesOverTypes)
{
    const auto items = CompleteAt("struct AlphaType {}; namespace ZebraNamespace {} using namespace ^;");

    ASSERT_LT(Position(items, "ZebraNamespace"), items.size());
    ASSERT_LT(Position(items, "AlphaType"), items.size());
    EXPECT_LT(Position(items, "ZebraNamespace"), Position(items, "AlphaType"));
}

TEST(CompletionRankingSpec, ReturnContextPrefersMatchingValueType)
{
    const auto items = CompleteAt("struct Want {}; int alphaWrong; Want zebraRight; Want Run() { return ^; }");

    ASSERT_LT(Position(items, "zebraRight"), items.size());
    ASSERT_LT(Position(items, "alphaWrong"), items.size());
    EXPECT_TRUE(items[Position(items, "zebraRight")].preferredTypeMatch);
    EXPECT_LT(Position(items, "zebraRight"), Position(items, "alphaWrong"));
}

TEST(CompletionRankingSpec, PointerReturnDoesNotPreferPointeeValues)
{
    const auto items = CompleteAt("int alphaValue; int* zebraPointer; int* Run() { return ^; }");

    ASSERT_LT(Position(items, "zebraPointer"), items.size());
    ASSERT_LT(Position(items, "alphaValue"), items.size());
    EXPECT_TRUE(items[Position(items, "zebraPointer")].preferredTypeMatch);
    EXPECT_FALSE(items[Position(items, "alphaValue")].preferredTypeMatch);
    EXPECT_LT(Position(items, "zebraPointer"), Position(items, "alphaValue"));
}

TEST(CompletionRankingSpec, AssignmentAndInitializationPreferMatchingTypes)
{
    for (const auto tail :
        {
            "Want target; target = ^; }", "Want target = ^; }"
    })
    {
        const auto items = CompleteAt(std::string("struct Want {}; int alphaWrong; Want zebraRight; void Run() { ") + tail);

        ASSERT_LT(Position(items, "zebraRight"), items.size());
        ASSERT_LT(Position(items, "alphaWrong"), items.size());
        EXPECT_TRUE(items[Position(items, "zebraRight")].preferredTypeMatch) << tail;
        EXPECT_LT(Position(items, "zebraRight"), Position(items, "alphaWrong")) << tail;
    }
}

TEST(CompletionRankingSpec, CallArgumentsUseAllAvailableOverloadParameterTypes)
{
    const auto items = CompleteAt("struct Want {}; void Use(int value); void Use(Want value); "
        "double alphaWrong; Want zebraRight; void Run() { Use(^); }");

    ASSERT_LT(Position(items, "zebraRight"), items.size());
    ASSERT_LT(Position(items, "alphaWrong"), items.size());
    EXPECT_TRUE(items[Position(items, "zebraRight")].preferredTypeMatch);
    EXPECT_FALSE(items[Position(items, "alphaWrong")].preferredTypeMatch);
    EXPECT_LT(Position(items, "zebraRight"), Position(items, "alphaWrong"));
}

TEST(CompletionRankingSpec, InstanceMembersOutrankStaticMembersThroughAnObject)
{
    const auto items = CompleteAt("struct Object { static void AlphaStatic(); void ZebraInstance(); }; "
        "void Run() { Object object; object.^ }");

    ASSERT_LT(Position(items, "ZebraInstance"), items.size());
    ASSERT_LT(Position(items, "AlphaStatic"), items.size());
    EXPECT_TRUE(items[Position(items, "AlphaStatic")].nonInstanceCompletion);
    EXPECT_LT(Position(items, "ZebraInstance"), Position(items, "AlphaStatic"));
}

TEST(CompletionRankingSpec, HealthyOverloadsAreNotPoisonedByDeprecatedOrDeletedSiblings)
{
    for (const auto declarations :
        {
            "[[deprecated]] int Fetch(int value); int Fetch(double value);",
            "int Fetch(int value) = delete; int Fetch(double value);",
            "int Fetch(double value); int Fetch(int value) = delete;"
    })
    {
        const auto items = CompleteAt(std::string(declarations) + " void Run() { Fet^; }");

        ASSERT_LT(Position(items, "Fetch"), items.size());
        const auto& item = items[Position(items, "Fetch")];
        EXPECT_FALSE(item.deprecated);
        EXPECT_FALSE(item.deleted);
        EXPECT_NE(item.detail.find("double"), std::string::npos);
    }
}

TEST(CompletionRankingSpec, OverloadGroupsKeepAllAvailableReturnTypeSignals)
{
    const auto items = CompleteAt("struct Want {}; int Alpha(int value); Want Zebra(double value); "
        "double Zebra(int value); Want Run() { return ^; }");

    ASSERT_LT(Position(items, "Zebra"), items.size());
    ASSERT_LT(Position(items, "Alpha"), items.size());
    EXPECT_TRUE(items[Position(items, "Zebra")].preferredTypeMatch);
    EXPECT_LT(Position(items, "Zebra"), Position(items, "Alpha"));
}

TEST(CompletionRankingSpec, DeletedFunctionsAreHiddenFromCompletionButRemainHoverable)
{
    const std::string source = "int Gone() = delete; void Run() { Gon; }";
    const auto items = heimdall::CompletionEngine::Complete(source, source.find("Gon;") + 3);

    EXPECT_EQ(Position(items, "Gone"), items.size());
    EXPECT_TRUE(heimdall::CompletionEngine::Hover(source, {}, source.find("Gone")));
}

TEST(CompletionRankingSpec, ResolvedUsesImproveRankingWithoutBindingOnTheColdPath)
{
    heimdall::Workspace workspace;
    const std::string source = "int alphaRare; int zebraFrequent; void Run() { zebraFrequent++; zebraFrequent++; }";
    const auto document = workspace.Open("completion-uses.cpp",
        std::make_shared<const std::string>(source));
    ASSERT_TRUE(document);
    const auto snapshot = workspace.Snapshot();
    const heimdall::AnalysisContext context(snapshot, *document);
    const auto offset = source.size() - 1;

    const auto cold = heimdall::AnalysisFeatures::Complete(context, offset);

    EXPECT_FALSE(snapshot.TrySemantic(*document));
    EXPECT_EQ(snapshot.Metrics().bind_count, 0);
    ASSERT_LT(Position(cold, "zebraFrequent"), cold.size());
    EXPECT_EQ(cold[Position(cold, "zebraFrequent")].mainFileReferences, 0);
    const auto model = snapshot.Semantic(*document);
    ASSERT_EQ(snapshot.TrySemantic(*document), model);

    const auto warm = heimdall::AnalysisFeatures::Complete(context, offset);

    ASSERT_LT(Position(warm, "zebraFrequent"), warm.size());
    ASSERT_LT(Position(warm, "alphaRare"), warm.size());
    EXPECT_EQ(warm[Position(warm, "zebraFrequent")].mainFileReferences, 2);
    EXPECT_LT(Position(warm, "zebraFrequent"), Position(warm, "alphaRare"));
    EXPECT_EQ(snapshot.Metrics().bind_count, 1);
    ASSERT_TRUE(workspace.Update(*document, std::make_shared<const std::string>("int fresh;"), 1));
    EXPECT_FALSE(workspace.Snapshot().TrySemantic(*document));
    EXPECT_EQ(snapshot.TrySemantic(*document), model);
}

TEST(CompletionRankingSpec, QualityRelevanceAndProximityHaveIndependentExplainableFactors)
{
    heimdall::CompletionItem item;
    item.label = "Fetch";
    item.kind = heimdall::CompletionKind::Function;
    const auto neutral = heimdall::CompletionRanking::Explain(item, "F");
    item.implementationDetail = true;
    const auto internal = heimdall::CompletionRanking::Explain(item, "F");
    EXPECT_LT(internal.quality, neutral.quality);
    EXPECT_EQ(internal.relevance, neutral.relevance);
    item.implementationDetail = false;
    item.mainFileReferences = 100;
    EXPECT_GT(heimdall::CompletionRanking::Evaluate(item, "F"), neutral.total);
    item.mainFileReferences = 0;
    item.scopeDistance = 0;
    const auto close = heimdall::CompletionRanking::Evaluate(item, "F");
    item.scopeDistance = 4;
    EXPECT_LT(heimdall::CompletionRanking::Evaluate(item, "F"), close);
    item.scopeDistance = -1;
    item.fileDistance = 0;
    const auto nearby = heimdall::CompletionRanking::Evaluate(item, "F");
    item.fileDistance = 4;
    EXPECT_LT(heimdall::CompletionRanking::Evaluate(item, "F"), nearby);
    item.fileDistance = -1;
    item.needsQualification = true;
    EXPECT_LT(heimdall::CompletionRanking::Evaluate(item, "F"), neutral.total);
}

TEST(CompletionRankingSpec, TiesAreIndependentOfCandidateCollectionOrder)
{
    heimdall::CompletionItem first;
    first.label = "Same";
    first.kind = heimdall::CompletionKind::Variable;
    first.file = 1;
    heimdall::CompletionItem second = first;
    second.file = 2;
    std::vector a {first, second};
    std::vector b {second, first};

    heimdall::CompletionRanking::Rank(a, "");
    heimdall::CompletionRanking::Rank(b, "");

    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(a[0].file, b[0].file);
    EXPECT_EQ(a[1].file, b[1].file);
}

TEST(CompletionRankingSpec, LocalVariablesKeepTheirIdentityWhenShadowingFunctions)
{
    const auto items = CompleteAt("void value(); void Run() { int value; val^; }");

    ASSERT_LT(Position(items, "value"), items.size());
    const auto& item = items[Position(items, "value")];
    EXPECT_EQ(item.kind, heimdall::CompletionKind::Variable);
    EXPECT_EQ(item.completionScope, heimdall::CompletionScope::Function);
    EXPECT_TRUE(item.has_location);
}

TEST(CompletionRankingSpec, FuzzyScoresRewardNearbyAndContiguousSegments)
{
    EXPECT_GT(heimdall::CompletionRanking::MatchScore("FooBarXX", "FB"),
              heimdall::CompletionRanking::MatchScore("FarAwayB", "FB"));
    EXPECT_GT(heimdall::CompletionRanking::MatchScore("Foo", "foo"),
              heimdall::CompletionRanking::MatchScore("FooBar", "foo"));
    EXPECT_EQ(heimdall::CompletionRanking::MatchScore("Alpha", "Z"), 0);
}

TEST(CompletionRankingSpec, UnknownExpectedTypesDoNotPenalizeOrDiscardCandidates)
{
    const auto items = CompleteAt("int alpha; double zebra; auto Run() { return ^; }");

    ASSERT_LT(Position(items, "alpha"), items.size());
    ASSERT_LT(Position(items, "zebra"), items.size());
    EXPECT_FALSE(items[Position(items, "alpha")].preferredTypeMatch);
    EXPECT_FALSE(items[Position(items, "zebra")].preferredTypeMatch);
}

TEST(CompletionRankingSpec, CategoryAndPreprocessorOrdersAreExplicit)
{
    heimdall::CompletionItem variable;
    variable.label = "Name";
    variable.kind = heimdall::CompletionKind::Variable;
    auto space = variable;
    space.kind = heimdall::CompletionKind::Namespace;
    auto macro = variable;
    macro.kind = heimdall::CompletionKind::Macro;
    auto directive = variable;
    directive.kind = heimdall::CompletionKind::Directive;
    directive.completionRole = heimdall::CompletionRole::Preprocessor;
    macro.completionRole = heimdall::CompletionRole::Preprocessor;

    EXPECT_GT(heimdall::CompletionRanking::Evaluate(variable, ""),
              heimdall::CompletionRanking::Evaluate(space, ""));
    EXPECT_GT(heimdall::CompletionRanking::Evaluate(directive, ""),
              heimdall::CompletionRanking::Evaluate(macro, ""));
    auto deleted = variable;
    deleted.deleted = true;
    EXPECT_EQ(heimdall::CompletionRanking::Evaluate(deleted, ""), 0);
}

TEST(CompletionRankingSpec, FrontendContextRetainsTypeRolesWithoutVisibleCandidates)
{
    for (const auto source :
        {
            "void Run() { new Missing", "using Alias = Missing", "void Run() { new ns::Missing",
            "void Run() { new ::Missing",
            "void Run() { static_cast<Missing"
    })
    {
        const std::string text(source);
        const auto context = heimdall::CompletionEngine::ContextAt(text, text.size());

        EXPECT_EQ(context.role, heimdall::CompletionRole::Type) << text;
    }
}

TEST(CompletionRankingSpec, OverloadRepresentativePrefersFewerRequiredParameters)
{
    for (const auto declarations :
        {
            "int Fetch(int x, int y); int Fetch(int x, int y = 0, int z = 0);",
            "int Fetch(int x, int y = 0, int z = 0); int Fetch(int x, int y);"
    })
    {
        const auto items = CompleteAt(std::string(declarations) + " void Run() { Fet^; }");

        ASSERT_LT(Position(items, "Fetch"), items.size());
        EXPECT_EQ(items[Position(items, "Fetch")].param_count, 3);
    }
}

TEST(CompletionRankingSpec, NearbyNamespacesOutrankOuterNamespaceDeclarations)
{
    const auto items = CompleteAt("int alphaOuter; namespace inner { int zebraInner; void Run() { ^; } }");

    ASSERT_LT(Position(items, "zebraInner"), items.size());
    ASSERT_LT(Position(items, "alphaOuter"), items.size());
    EXPECT_LT(items[Position(items, "zebraInner")].scopeDistance,
              items[Position(items, "alphaOuter")].scopeDistance);
    EXPECT_LT(Position(items, "zebraInner"), Position(items, "alphaOuter"));
}

TEST(CompletionRankingSpec, ImplicitMembersOutrankFileGlobalsButRemainBelowParameters)
{
    const auto items = CompleteAt("int alphaGlobal; struct Object { int middleMember; "
        "void Run(int zebraParameter) { ^; } };");

    ASSERT_LT(Position(items, "alphaGlobal"), items.size());
    ASSERT_LT(Position(items, "middleMember"), items.size());
    ASSERT_LT(Position(items, "zebraParameter"), items.size());
    EXPECT_LT(Position(items, "zebraParameter"), Position(items, "middleMember"));
    EXPECT_LT(Position(items, "middleMember"), Position(items, "alphaGlobal"));
}

TEST(CompletionRankingSpec, OverrideDeclarationsOutrankInheritedCallableSuggestions)
{
    const auto items = CompleteAt("struct Base { virtual void Alpha(); }; struct Derived : Base { Al^ };");
    const auto overrideItem = std::find_if(items.begin(), items.end(),[](const auto& item)
        {
            return item.filter_text == "Alpha" && item.label.ends_with(" override");
        });

    ASSERT_NE(overrideItem, items.end());
    ASSERT_LT(Position(items, "Alpha"), items.size());
    EXPECT_LT(static_cast<std::size_t>(overrideItem - items.begin()), Position(items, "Alpha"));
}

TEST(CompletionRankingSpec, MemberAssignmentUsesTheReceiverRatherThanASameNamedGlobal)
{
    const auto items = CompleteAt("struct Object { double field; }; int field; int alphaWrong; "
        "double zebraRight; void Run() { Object object; object.field = ^; }");

    ASSERT_LT(Position(items, "zebraRight"), items.size());
    ASSERT_LT(Position(items, "alphaWrong"), items.size());
    EXPECT_TRUE(items[Position(items, "zebraRight")].preferredTypeMatch);
    EXPECT_FALSE(items[Position(items, "alphaWrong")].preferredTypeMatch);
    EXPECT_LT(Position(items, "zebraRight"), Position(items, "alphaWrong"));
}
