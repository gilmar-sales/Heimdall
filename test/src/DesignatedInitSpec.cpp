#include <Heimdall/RuleEngine.hpp>
#include <Heimdall/SemanticRules.hpp>

#include <gtest/gtest.h>

#include <string>

namespace
{

    std::vector<heimdall::Diagnostic> Order(const std::string &source)
    {
        const auto tree = heimdall::ParseTree::Parse(source);
        const auto model = heimdall::Binder::Bind(tree);
        return heimdall::SemanticRules::AnalyzeDesignatedInitOrder(model);
    }

    std::string Fixed(std::string source, const heimdall::Diagnostic &diagnostic)
    {
        source.replace(diagnostic.fix.offset, diagnostic.fix.length, diagnostic.fix.replacement);
        return source;
    }

    const std::string kNode = "struct Node { Node* next; int value; };\n";

} // namespace

TEST(DesignatedInitOrder, ReordersAccordingToTheDeclaration)
{
    const std::string source = kNode + "void f() { auto p = new Node{ .value = 10, .next = 0 }; }\n";
    const auto diagnostics = Order(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "cpp/designated-init-order");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::DesignatedInitOrder);
    EXPECT_TRUE(diagnostics[0].has_fix);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(Fixed(source, diagnostics[0]),
        kNode + "void f() { auto p = new Node{ .next = 0, .value = 10 }; }\n");
}

TEST(DesignatedInitOrder, KeepsSeparatorsAndMultilineLayout)
{
    const std::string source = kNode + "void f() {\n    Node n{\n        .value = 10,\n        .next = 0\n    };\n}\n";
    const auto diagnostics = Order(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(Fixed(source, diagnostics[0]),
        kNode + "void f() {\n    Node n{\n        .next = 0,\n        .value = 10\n    };\n}\n");
}

TEST(DesignatedInitOrder, WorksForVariablesWithEqualsAndNestedCommas)
{
    const std::string source = "struct P { std::pair<int, int> pair; int z; int y; };\n"
                               "P p = { .y = 1, .z = 2, .pair = std::pair<int, int>{1, 2} };\n";
    const auto diagnostics = Order(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(Fixed(source, diagnostics[0]),
        "struct P { std::pair<int, int> pair; int z; int y; };\n"
        "P p = { .pair = std::pair<int, int>{1, 2}, .z = 2, .y = 1 };\n");
}

TEST(DesignatedInitOrder, SilentWhenAlreadyOrdered)
{
    EXPECT_TRUE(Order(kNode + "Node n{ .next = 0, .value = 1 };\n").empty());
    EXPECT_TRUE(Order(kNode + "Node n{ .value = 1 };\n").empty());
}

TEST(DesignatedInitOrder, SilentWhenTheClassOrMembersAreUnknown)
{
    EXPECT_TRUE(Order("Unknown n{ .b = 1, .a = 2 };\n").empty());
    EXPECT_TRUE(Order(kNode + "Node n{ .value = 1, .missing = 2 };\n").empty());
    EXPECT_TRUE(Order(kNode + "Node n{ .value = 1, .value = 2 };\n").empty());
    EXPECT_TRUE(Order(kNode + "Node n{ .value = 1, 0 };\n").empty());
    EXPECT_TRUE(Order("struct D : Base { int a; int b; };\nD d{ .b = 1, .a = 2 };\n").empty());
}

TEST(DesignatedInitOrder, SurvivesBrokenInput)
{
    EXPECT_NO_THROW(Order(kNode + "Node n{ .value = 1, .next"));
    EXPECT_NO_THROW(Order(kNode + "Node n{ . "));
}
