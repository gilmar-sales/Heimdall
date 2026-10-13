#include <gtest/gtest.h>

#include <Heimdall/Completion.hpp>
#include <Heimdall/TemplateDeduction.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr std::string_view kTupleHeader =
        "namespace library {\n"
        "template<class First, class Second, class Third>\n"
        "struct Tuple { First first; Second second; Third third; };\n"
        "template<class First, class Second, class Third>\n"
        "constexpr auto Make(First first, Second second, Third third) {\n"
        "return Tuple<First, Second, Third> {first, second, third}; }\n"
        "template<unsigned index, class First, class Second, class Third>\n"
        "constexpr decltype(auto) At(const Tuple<First, Second, Third>& tuple) {\n"
        "if constexpr (index == 0) { return (tuple.first); }\n"
        "else if constexpr (index == 1) { return (tuple.second); }\n"
        "else { return (tuple.third); } }\n}\n";
}

TEST(TemplateDeductionSpec, FactoryDeducesHeterogeneousTemplateArguments)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(kTupleHeader, {});
    const std::string source = "constexpr auto tuple = library::Make(1, 'a', 2.0);\n";

    const auto hovered = heimdall::CompletionEngine::Hover(source, {}, source.find("tuple"), &index);

    ASSERT_TRUE(hovered);
    EXPECT_NE(hovered->detail.find("Tuple<int, char, double>"), std::string::npos);
}

TEST(TemplateDeductionSpec, ExplicitFactoryArgumentsSubstituteConstructedReturn)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "template<class T> struct Box { T value; };\n"
        "template<class T> constexpr auto Make() { return Box<T> {}; }\n", {});
    const std::string source = "auto box = Make<int>();\n";

    const auto hovered = heimdall::CompletionEngine::Hover(source, {}, source.find("box"), &index);

    ASSERT_TRUE(hovered);
    EXPECT_NE(hovered->detail.find("Box<int>"), std::string::npos);
}

TEST(TemplateDeductionSpec, MultipleExplicitArgumentsDoNotTruncateInitializerAtComma)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "template<class First, class Second> struct Pair {};\n"
        "template<class First, class Second> auto Make() { return Pair<First, Second> {}; }\n", {});
    const std::string source = "auto pair = Make<int, double>();\n";

    const auto hovered = heimdall::CompletionEngine::Hover(source, {}, source.find("pair"), &index);

    ASSERT_TRUE(hovered);
    EXPECT_NE(hovered->detail.find("Pair<int, double>"), std::string::npos);
}

TEST(TemplateDeductionSpec, LiteralDeductionRespectsHexIntegersAndSuffixes)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(kTupleHeader, {});
    const std::string source = "auto tuple = library::Make(0x1e, 1u, 2.0f);\n";

    const auto hovered = heimdall::CompletionEngine::Hover(source, {}, source.find("tuple"), &index);

    ASSERT_TRUE(hovered);
    EXPECT_NE(hovered->detail.find("Tuple<int, unsigned int, float>"), std::string::npos);
}

TEST(TemplateDeductionSpec, MultipleFactoryOverloadsDoNotGuessAnInstantiation)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "struct First {}; struct Second {};\n"
        "template<class T> auto Make(T first) { return First {}; }\n"
        "template<class T> auto Make(const T& first) { return Second {}; }\n", {});
    const std::string source = "auto object = Make(1);\n";

    const auto hovered = heimdall::CompletionEngine::Hover(source, {}, source.find("object"), &index);

    ASSERT_TRUE(hovered);
    EXPECT_EQ(hovered->detail, "auto");
}

TEST(TemplateDeductionSpec, AmbiguousFactoryCallHoverDoesNotInventAReturnType)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "struct First {}; struct Second {};\n"
        "template<class T> auto Make(T first) { return First {}; }\n"
        "template<class T> auto Make(const T& first) { return Second {}; }\n", {});
    const std::string source = "auto object = Make(1);\n";

    const auto hovered = heimdall::CompletionEngine::Hover(source, {}, source.find("Make("), &index);

    ASSERT_TRUE(hovered);
    EXPECT_NE(hovered->detail.find("auto"), std::string::npos);
}

TEST(TemplateDeductionSpec, DecltypeAutoSelectsConstexprBranchAndPreservesReference)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(kTupleHeader, {});
    const std::string prefix = "constexpr auto tuple = library::Make(1, 'a', 2.0);\n";
    const std::vector<std::string> expected
    {
        "const int&", "const char&", "const double&"
    };

    for (std::size_t branch = 0; branch < expected.size(); ++branch)
    {
        const std::string source = prefix + "static_assert(library::At<" + std::to_string(branch) + ">(tuple));\n";
        const auto hovered = heimdall::CompletionEngine::Hover(source, {}, source.find("At<"), &index);

        ASSERT_TRUE(hovered);
        EXPECT_NE(hovered->detail.find(expected[branch]), std::string::npos) << hovered->detail;
    }
}

TEST(TemplateDeductionSpec, UnknownConstexprConditionDoesNotInventReturnType)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(kTupleHeader, {});
    const std::string source = "auto tuple = library::Make(1, 'a', 2.0);\n"
        "static_assert(library::At<unknown>(tuple));\n";

    const auto hovered = heimdall::CompletionEngine::Hover(source, {}, source.find("At<"), &index);

    ASSERT_TRUE(hovered);
    EXPECT_NE(hovered->detail.find("decltype(auto)"), std::string::npos);
}

TEST(TemplateDeductionSpec, DiscardedConstexprBranchDoesNotRequireUnknownInnerCondition)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "struct First {}; struct Second {};\n"
        "template<unsigned index> auto Make() {\n"
        "if constexpr (index == 0) { return First {}; }\n"
        "else { if constexpr (unknown) { return Second {}; } } }\n", {});
    const std::string source = "auto object = Make<0>();\n";

    const auto hovered = heimdall::CompletionEngine::Hover(source, {}, source.find("object"), &index);

    ASSERT_TRUE(hovered);
    EXPECT_NE(hovered->detail.find("First"), std::string::npos);
}

TEST(TemplateDeductionSpec, ConflictingTemplateDeductionsRemainUnknown)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "template<class T> struct Box { T value; };\n"
        "template<class T> auto Make(T first, T second) { return Box<T> {}; }\n", {});
    const std::string source = "auto box = Make(1, 2.0);\n";

    const auto hovered = heimdall::CompletionEngine::Hover(source, {}, source.find("box"), &index);

    ASSERT_TRUE(hovered);
    EXPECT_EQ(hovered->detail, "auto");
}

TEST(TemplateDeductionSpec, DifferentRuntimeReturnTypesRemainUnknown)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "struct First {}; struct Second {};\n"
        "auto Make(int flag) { if (flag) { return First {}; } return Second {}; }\n", {});
    const std::string source = "auto object = Make(1);\n";

    const auto hovered = heimdall::CompletionEngine::Hover(source, {}, source.find("object"), &index);

    ASSERT_TRUE(hovered);
    EXPECT_EQ(hovered->detail, "auto");
}

TEST(TemplateDeductionSpec, UnparenthesizedMemberUsesDeclaredTypeForDecltypeAuto)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "struct Box { int value; };\n"
        "decltype(auto) Get(const Box& box) { return box.value; }\n", {});
    const std::string source = "Box box;\nvoid Run() { Get(box); }\n";

    const auto hovered = heimdall::CompletionEngine::Hover(source, {}, source.find("Get("), &index);

    ASSERT_TRUE(hovered);
    EXPECT_NE(hovered->detail.find("int Get"), std::string::npos);
    EXPECT_EQ(hovered->detail.find("const int&"), std::string::npos);
}

TEST(TemplateDeductionSpec, UnsupportedCallReturnIsNotEvaluatedAsAConstructor)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "struct Box {};\n"
        "template<class F> auto Make(F callable) { return callable(); }\n", {});
    const std::string source = "auto object = Make(1);\n";

    const auto hovered = heimdall::CompletionEngine::Hover(source, {}, source.find("object"), &index);

    ASSERT_TRUE(hovered);
    EXPECT_EQ(hovered->detail, "auto");
}

TEST(TemplateDeductionSpec, ExplicitTypeContainingAutoIsNotTreatedAsPlaceholder)
{
    const auto index = heimdall::CompletionEngine::IndexScopes(
        "struct auto_wrapper {};\n"
        "auto_wrapper Make() { return auto_wrapper {}; }\n", {});
    const std::string source = "auto object = Make();\n";

    const auto hovered = heimdall::CompletionEngine::Hover(source, {}, source.find("object"), &index);

    ASSERT_TRUE(hovered);
    EXPECT_EQ(hovered->detail, "auto_wrapper");
}
