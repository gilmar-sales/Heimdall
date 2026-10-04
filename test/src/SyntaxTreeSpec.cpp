#include <gtest/gtest.h>

#include <Heimdall/SyntaxTree.hpp>

#include <string>

TEST(SyntaxTreeSpec, PreservesSourceAndBuildsNestedDelimiterGroups)
{
    constexpr std::string_view source = "int f(int x) { return values[x + (1)]; }\n";
    const auto tree = heimdall::SyntaxTree::Parse(source);

    std::string reconstructed;
    for (const auto& token : tree.Tokens()) reconstructed.append(tree.Text(token));
    EXPECT_EQ(reconstructed, source);
    ASSERT_EQ(tree.Nodes().front().kind, heimdall::SyntaxKind::TranslationUnit);
    EXPECT_EQ(tree.Nodes().front().token_count, tree.Tokens().size());
    EXPECT_TRUE(tree.Diagnostics().empty());

    std::size_t parens = 0;
    std::size_t brackets = 0;
    std::size_t braces = 0;
    for (const auto& node : tree.Nodes())
    {
        parens += node.kind == heimdall::SyntaxKind::ParenthesizedGroup;
        brackets += node.kind == heimdall::SyntaxKind::BracketedGroup;
        braces += node.kind == heimdall::SyntaxKind::BracedGroup;
    }
    EXPECT_EQ(parens, 2);
    EXPECT_EQ(brackets, 1);
    EXPECT_EQ(braces, 1);
}

TEST(SyntaxTreeSpec, ReportsMismatchedAndUnclosedDelimitersWithoutAborting)
{
    constexpr std::string_view source = "([)] {";
    const auto tree = heimdall::SyntaxTree::Parse(source);
    EXPECT_EQ(tree.Tokens().size(), 6);
    ASSERT_EQ(tree.Diagnostics().size(), 3);
    EXPECT_EQ(tree.Diagnostics()[0].message, "unmatched closing delimiter");
    EXPECT_EQ(tree.Diagnostics()[1].message, "unclosed delimiter");
    EXPECT_EQ(tree.Diagnostics()[2].message, "unclosed delimiter");
    EXPECT_EQ(tree.Nodes().front().token_count, tree.Tokens().size());
}

TEST(SyntaxTreeSpec, EnforcesNestingLimitAndStillProducesTree)
{
    const auto tree = heimdall::SyntaxTree::Parse("((((x))))", 2);
    EXPECT_FALSE(tree.Diagnostics().empty());
    EXPECT_EQ(tree.Nodes().front().token_count, tree.Tokens().size());
}

TEST(SyntaxTreeSpec, CarriesExplicitLanguageDialectWithoutChangingSourceSpans)
{
    constexpr std::string_view source = "int f() { return 0; }";
    for (const auto standard : { heimdall::CppStandard::Cpp20, heimdall::CppStandard::Cpp23,
                                 heimdall::CppStandard::Cpp26 })
    {
        const auto tree = heimdall::SyntaxTree::Parse(source, standard);
        EXPECT_EQ(tree.Standard(), standard);
        EXPECT_EQ(tree.Source(), source);
        EXPECT_TRUE(tree.Diagnostics().empty());
    }

    // Existing callers remain source-compatible and select the documented
    // default dialect until a compilation database provides one.
    EXPECT_EQ(heimdall::SyntaxTree::Parse(source).Standard(), heimdall::CppStandard::Cpp20);
}
