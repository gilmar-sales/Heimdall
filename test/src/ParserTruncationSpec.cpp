// Regression tests for the review section 7 validation tools: parser behaviour on
// every prefix of a valid document (incomplete typing) and the stb corpus builder
// shared by the bench tools (bench/src/StbCorpus.hpp).
#include "StbCorpus.hpp"

#include <Heimdall/ParseTree.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>

namespace
{

    using heimdall::GrammarKind;

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

    std::size_t CountErrors(const heimdall::ParseTree & tree)
    {
        return static_cast<std::size_t>(std::count_if(tree.Nodes().begin(), tree.Nodes().end(), [](const auto & node)
            {
                return node.kind == GrammarKind::Error || node.kind == GrammarKind::ErrorExpression;
        }));
    }

    TEST(ParserTruncation, EveryPrefixParsesWithBoundedErrorsAndTime)
    {
        EXPECT_EQ(CountErrors(heimdall::ParseTree::Parse(kSample)), 0u) << "the sample must be valid";

        std::size_t worst_errors = 0;
        std::size_t worst_at = 0;
        for (std::size_t length = 0; length <= kSample.size(); ++length)
        {
            const auto start = std::chrono::steady_clock::now();
            const auto tree = heimdall::ParseTree::Parse(kSample.substr(0, length));
            const auto elapsed = std::chrono::steady_clock::now() - start;

            // Hang guard, deliberately loose so a loaded CI machine cannot flake it.
            ASSERT_LT(elapsed, std::chrono::seconds(2)) << "prefix of " << length << " bytes";
            ASSERT_FALSE(tree.Cancelled());
            ASSERT_FALSE(tree.Nodes().empty()) << "prefix of " << length << " bytes";
            ASSERT_EQ(tree.Nodes()[heimdall::ParseTree::RootNode].kind, GrammarKind::TranslationUnit);
            for (const auto & node: tree.Nodes())
            {
                ASSERT_LE(node.subtree_end, tree.Nodes().size());
                ASSERT_LE(static_cast<std::size_t>(node.first_token) + node.token_count, tree.Tokens().size());
            }
            const std::size_t errors = CountErrors(tree);
            if (errors > worst_errors)
            {
                worst_errors = errors;
                worst_at = length;
            }
        }
        EXPECT_LE(worst_errors, 8u) << "worst prefix: " << worst_at;
    }

    TEST(ParserTruncation, UnclosedBraceDoesNotSwallowErrorBudget)
    {
        // A single missing `}` must not cascade into one error per remaining token.
        const std::string source = "int f() {\n" + std::string(200, ' ') + "int a = 1;\nint b = 2;\nint c = 3;\n";
        EXPECT_LE(CountErrors(heimdall::ParseTree::Parse(source)), 8u);
    }

    TEST(StbCorpus, TargetsMapToWholeFileTiers)
    {
        const std::string dir = std::string(HEIMDALL_SOURCE_DIR) + "/bench";
        const auto small = heimdall::bench::BuildDocument(dir, 1000);
        const auto medium = heimdall::bench::BuildDocument(dir, 5000);
        const auto large = heimdall::bench::BuildDocument(dir, 20000);
        ASSERT_TRUE(small && medium && large);
        EXPECT_GE(heimdall::bench::CountLines(*small), 900u);
        EXPECT_LT(heimdall::bench::CountLines(*small), 2000u);
        EXPECT_GE(heimdall::bench::CountLines(*medium), 4500u);
        EXPECT_GE(heimdall::bench::CountLines(*large), 18000u);
        EXPECT_LT(heimdall::bench::CountLines(*small), heimdall::bench::CountLines(*medium));
        EXPECT_LT(heimdall::bench::CountLines(*medium), heimdall::bench::CountLines(*large));
    }

    TEST(StbCorpus, RepeatedCopiesGetUniqueIncludeGuards)
    {
        const std::string dir = std::string(HEIMDALL_SOURCE_DIR) + "/bench";
        const auto large = heimdall::bench::BuildDocument(dir, 20000);
        ASSERT_TRUE(large);
        // The lexer is emitted twice; the second copy must not collide with the first.
        EXPECT_NE(large->find("#ifndef INCLUDE_STB_C_LEXER_H_COPY1"), std::string::npos);
        EXPECT_EQ(large->find("#ifndef INCLUDE_STB_C_LEXER_H_COPY1"), large->rfind("#ifndef INCLUDE_STB_C_LEXER_H_COPY1"));
    }

    TEST(StbCorpus, MissingDirectoryYieldsNullopt)
    {
        EXPECT_FALSE(heimdall::bench::BuildDocument("/definitely/not/here", 1000).has_value());
    }

    TEST(StbCorpus, EmbeddedImplementationParsesWithoutErrorNodes)
    {
        const std::string dir = std::string(HEIMDALL_SOURCE_DIR) + "/bench";
        const auto small = heimdall::bench::BuildDocument(dir, 1000);
        ASSERT_TRUE(small);
        EXPECT_LE(CountErrors(heimdall::ParseTree::Parse(*small)), 8u);
    }

} // namespace
