#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

#include <Heimdall/ParseTree.hpp>

TEST(HeaderFixtureSpec, ShapesHeaderParsesWithoutCompileDefinitions)
{
    std::ifstream input(
        std::string(
        HEIMDALL_SOURCE_DIR) + "/test/fixtures/include/mylib/shapes.hpp");
    ASSERT_TRUE(input.is_open());
    const std::string source
    {
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()
    };
    ASSERT_FALSE(source.empty());

    heimdall::ParserOptions options;
    options.standard = heimdall::CppStandard::Cpp20;
    options.predefined_macros.emplace("__cplusplus", "202002L");

    const auto tree = heimdall::ParseTree::Parse(source, options);

    EXPECT_TRUE(tree.Diagnostics().empty());
}
