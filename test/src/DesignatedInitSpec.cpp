#include <Heimdall/RuleEngine.hpp>
#include <Heimdall/SemanticRules.hpp>

#include <gtest/gtest.h>

#include <string>

namespace
{

    std::vector<heimdall::Diagnostic> Order(const std::string& source)
    {
        const auto tree = heimdall::ParseTree::Parse(source);
        const auto model = heimdall::Binder::Bind(tree);
        return heimdall::SemanticRules::AnalyzeDesignatedInitOrder(model);
    }

    std::string Fixed(std::string source, const heimdall::Diagnostic& diagnostic)
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

TEST(DesignatedInitZeroAsNull, ReportsZeroAssignedToAPointerMember)
{
    const std::string source = kNode + "Node n{ .next = 0, .value = 0 };\n";
    const auto tree = heimdall::ParseTree::Parse(source);
    const auto model = heimdall::Binder::Bind(tree);
    for (const auto& diagnostics :
        {
            heimdall::SemanticRules::AnalyzeDesignatedZeroAsNull(model),
            heimdall::SemanticRules::AnalyzeZeroAsNull(model)
    })
    {
        ASSERT_EQ(diagnostics.size(), 1u); // `.value = 0` is an int
        EXPECT_EQ(diagnostics[0].code, "cpp/no-zero-as-null");
        EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::NoZeroAsNull);
        EXPECT_EQ(Fixed(source, diagnostics[0]), kNode + "Node n{ .next = nullptr, .value = 0 };\n");
    }
}

TEST(DesignatedInitZeroAsNull, WorksWithNewAndSilentForNullptrOrUnknownClasses)
{
    const auto count =[](const std::string& source)
    {
        const auto tree = heimdall::ParseTree::Parse(source);
        const auto model = heimdall::Binder::Bind(tree);
        return heimdall::SemanticRules::AnalyzeDesignatedZeroAsNull(model).size();
    };
    EXPECT_EQ(count(kNode + "void f() { auto p = new Node{ .next = 0,\n .value = 10 }; }\n"), 1u);
    EXPECT_EQ(count(kNode + "Node n{ .next = nullptr, .value = 1 };\n"), 0u);
    EXPECT_EQ(count("Other n{ .next = 0 };\n"), 0u);
}

namespace
{

    std::vector<heimdall::Diagnostic> IntegerToPointer(const std::string& source)
    {
        const auto tree = heimdall::ParseTree::Parse(source);
        const auto model = heimdall::Binder::Bind(tree);
        return heimdall::SemanticRules::AnalyzeIntegerToPointer(model);
    }

} // namespace

TEST(IntegerToPointer, ReportsAnIntegerInADesignatedInitializerAsAnError)
{
    const std::string source = kNode + "Node n{ .next = 20, .value = 20 };\n";
    const auto diagnostics = IntegerToPointer(source);
    ASSERT_EQ(diagnostics.size(), 1u); // `.value` is an int
    EXPECT_EQ(diagnostics[0].code, "cpp/no-integer-to-pointer");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::NoIntegerToPointer);
    EXPECT_EQ(diagnostics[0].severity, heimdall::Severity::Error);
    EXPECT_FALSE(diagnostics[0].has_fix);
    EXPECT_EQ(source.substr(diagnostics[0].offset, diagnostics[0].length), "20");
    EXPECT_EQ(IntegerToPointer(kNode + "auto p = new Node{ .next = 0x10, .value = 1 };\n").size(), 1u);
}

TEST(IntegerToPointer, ReportsDeclarationsAndAssignments)
{
    EXPECT_EQ(IntegerToPointer("int* p = 20;\n").size(), 1u);
    EXPECT_EQ(IntegerToPointer("void f(int* p = 8) {}\n").size(), 1u);
    EXPECT_EQ(IntegerToPointer("void f() { int* p = nullptr; p = 5; }\n").size(), 1u);
}

TEST(IntegerToPointer, SilentForNullConstantsIntegersAndFloats)
{
    EXPECT_TRUE(IntegerToPointer("int* p = 0;\nint* q = nullptr;\nint n = 20;\n").empty());
    EXPECT_TRUE(IntegerToPointer("void f() { int n; n = 20; }\n").empty());
    EXPECT_TRUE(IntegerToPointer(kNode + "Node n{ .next = 0, .value = 20 };\n").empty());
    EXPECT_TRUE(IntegerToPointer("Unknown n{ .next = 20 };\n").empty());
    EXPECT_TRUE(IntegerToPointer("struct S { int* p; };\nS s{ .p = reinterpret_cast<int*>(20) };\n").empty());
}

TEST(IntegerToPointer, IsRegisteredAsAnErrorRule)
{
    const auto* info = heimdall::FindRuleByCode("cpp/no-integer-to-pointer");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->default_severity, heimdall::Severity::Error);
}
