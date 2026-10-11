#include <gtest/gtest.h>
#include <Heimdall/Completion.hpp>
#include <Heimdall/Doxygen.hpp>
#include <Heimdall/Navigation.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    void ExpectDefinition(
        const std::string& source,
        std::size_t reference,
        std::size_t declaration,
        std::string_view name)
    {
        ASSERT_NE(reference, std::string::npos);
        ASSERT_NE(declaration, std::string::npos);
        const auto tree = heimdall::ParseTree::Parse(source);

        const auto targets = heimdall::Navigation::Definition(tree, reference);

        ASSERT_EQ(targets.size(), 1u);
        EXPECT_EQ(targets.front().offset, declaration);
        EXPECT_EQ(targets.front().length, name.size());
        EXPECT_EQ(targets.front().name, name);
    }

    void ExpectNoDefinition(const std::string& source, std::size_t reference)
    {
        ASSERT_NE(reference, std::string::npos);
        const auto tree = heimdall::ParseTree::Parse(source);

        EXPECT_TRUE(heimdall::Navigation::Definition(tree, reference).empty());
    }
}

TEST(DoxygenNavigationSpec, RendersBriefAcrossSupportedCommentFormsAndCommandPrefixes)
{
    for (const std::string prefix :
        {
            "@", "\\"
    })
    {
        for (const std::string comment :
            {
                "/** " + prefix + "brief Adds values. */\n",
                "/*! " + prefix + "brief Adds values. */\n",
                "/// " + prefix + "brief Adds values.\n",
                "//! " + prefix + "brief Adds values.\n"
        })
        {
            SCOPED_TRACE(comment);
            const std::string source = comment +
                "int Add(int value) { return value; }\nint use() { return Add(1); }\n";

            const auto hover = heimdall::CompletionEngine::Hover(
                source, heimdall::ParserOptions {}, source.rfind("Add"));

            ASSERT_TRUE(hover);
            EXPECT_EQ(hover->documentation, "Adds values.");
        }
    }
}

TEST(DoxygenNavigationSpec, RendersParametersAndBothReturnCommandsWithoutRawTags)
{
    for (const std::string prefix :
        {
            "@", "\\"
    })
    {
        for (const std::string returnCommand :
            {
                "return", "returns"
        })
        {
            SCOPED_TRACE(prefix + returnCommand);
            const std::string source =
                "/**\n * " + prefix + "brief Adds two numbers.\n *\n * " + prefix +
                "param left First operand.\n * " + prefix + "param right Second operand.\n * " +
                prefix + returnCommand + " The sum.\n */\n"
                "int Add(int left, int right) { return left + right; }\n"
                "int use() { return Add(1, 2); }\n";

            const auto hover = heimdall::CompletionEngine::Hover(
                source, heimdall::ParserOptions {}, source.rfind("Add"));

            ASSERT_TRUE(hover);
            for (const auto text :
                {
                    "Adds two numbers.", "left", "First operand.",
                    "right", "Second operand.", "The sum."
            })
            {
                EXPECT_NE(hover->documentation.find(text), std::string::npos);
            }

            for (const auto command :
                {
                    "brief", "param", "return", "returns"
            })
            {
                EXPECT_EQ(hover->documentation.find(prefix + command), std::string::npos);
            }
        }
    }
}

TEST(DoxygenNavigationSpec, NavigatesParamNamesWithDirectionsToTheirOwnDeclaration)
{
    for (const std::string prefix :
        {
            "@", "\\"
    })
    {
        for (const std::string direction :
            {
                "", "[in]", "[out]", "[in,out]", " [in,out]"
        })
        {
            SCOPED_TRACE(prefix + "param" + direction);
            const std::string source =
                "/** " + prefix + "param" + direction + " value The input. */\n"
                "int Calculate(int value) { return value; }\n"
                "int Other(int value) { return value; }\n";

            ExpectDefinition(source, source.find("value The") + 1,
                             source.find("value)"), "value");
        }
    }
}

TEST(DoxygenNavigationSpec, NavigatesEachNameInCommaSeparatedParamTag)
{
    const std::string source =
        "/** @param x,y Coordinates. */\nvoid Set(int x, int y) {}\n";

    ExpectDefinition(source, source.find("x,y"), source.find("x, int"), "x");
    ExpectDefinition(source, source.find("x,y") + 2, source.find("y)"), "y");
}

TEST(DoxygenNavigationSpec, NavigatesRefToFunctionAndClassWithEitherPrefix)
{
    for (const std::string prefix :
        {
            "@", "\\"
    })
    {
        const std::string source =
            "class Node {};\nint Calculate(int value) { return value; }\n"
            "/** " + prefix + "brief Uses " + prefix + "ref Node and " + prefix +
            "ref Calculate. */\nvoid Use() {}\n";

        ExpectDefinition(source, source.rfind("Node"), source.find("Node"), "Node");
        ExpectDefinition(source, source.rfind("Calculate"), source.find("Calculate"), "Calculate");
    }
}

TEST(DoxygenNavigationSpec, NavigatesSeeAndSaReferencesIncludingFunctionParentheses)
{
    for (const std::string prefix :
        {
            "@", "\\"
    })
    {
        for (const std::string command :
            {
                "see", "sa"
        })
        {
            SCOPED_TRACE(prefix + command);
            const std::string source =
                "class Node {};\nvoid Reset() {}\n"
                "/** " + prefix + command + " Node, Reset() */\nvoid Use() {}\n";

            ExpectDefinition(source, source.rfind("Node"), source.find("Node"), "Node");
            ExpectDefinition(source, source.rfind("Reset"), source.find("Reset"), "Reset");
        }
    }
}

TEST(DoxygenNavigationSpec, NavigatesInlineRefInsideReturnsParagraph)
{
    for (const std::string command :
        {
            "return", "returns"
    })
    {
        const std::string source =
            "class Node {};\n/** @" + command + " A pointer to @ref Node. */\n"
            "Node* Create() { return nullptr; }\n";

        ExpectDefinition(source, source.find("Node."), source.find("Node"), "Node");
    }
}

TEST(DoxygenNavigationSpec, ResolvesQualifiedRefsWithoutConfusingSameNamedClasses)
{
    const std::string source =
        "namespace first { class Node {}; }\nnamespace second { class Node {}; }\n"
        "/** @ref second::Node */\nvoid Use() {}\n";

    ExpectDefinition(source, source.rfind("Node"), source.find("Node", source.find("second")),
                     "Node");
}

TEST(DoxygenNavigationSpec, ResolvesRefFunctionSignatureToTheCorrectOverload)
{
    const std::string source =
        "int Convert(int value) { return value; }\n"
        "double Convert(double value) { return value; }\n"
        "/** @ref Convert(double) */\nvoid Use() {}\n";

    ExpectDefinition(source, source.rfind("Convert"), source.find("Convert", source.find("double")),
                     "Convert");
}

TEST(DoxygenNavigationSpec, DoesNotTreatDescriptionsLabelsOrUnknownTargetsAsSymbols)
{
    const std::string source =
        "class Node {};\n/**\n * @brief Node is mentioned only as prose.\n"
        " * @param value Node is the example.\n"
        " * @returns Node is descriptive text.\n"
        " * @ref Node \"Node label\"\n"
        " * @see Missing\n */\nint Get(int value) { return value; }\n";

    ExpectNoDefinition(source, source.find("Node is mentioned"));
    ExpectNoDefinition(source, source.find("Node is the example"));
    ExpectNoDefinition(source, source.find("Node is descriptive"));
    ExpectNoDefinition(source, source.find("Node label"));
    ExpectNoDefinition(source, source.find("Missing"));
    ExpectDefinition(source, source.find("Node \""), source.find("Node"), "Node");
}

TEST(DoxygenNavigationSpec, IgnoresRefInPlainCommentsAndDoxygenCodeBlocks)
{
    const std::string source =
        "class Node {};\n// @ref Node\n/* @ref Node */\n"
        "/** @code\n * @ref Node\n * @endcode\n */\nvoid Use() {}\n";

    ExpectNoDefinition(source, source.find("Node", source.find("//")));
    ExpectNoDefinition(source, source.find("Node", source.find("/* @")));
    ExpectNoDefinition(source, source.find("Node", source.find("@code")));
}

TEST(DoxygenNavigationSpec, PreservesTagsInOrdinaryDocumentationComments)
{
    const std::string source =
        "// @brief This is ordinary comment text.\n"
        "int Get() { return 1; }\nint use() { return Get(); }\n";

    const auto hover = heimdall::CompletionEngine::Hover(
        source, heimdall::ParserOptions {}, source.rfind("Get"));

    ASSERT_TRUE(hover);
    EXPECT_EQ(hover->documentation, "@brief This is ordinary comment text.");
}

TEST(DoxygenNavigationSpec, FormatsSeparateParameterReturnAndSeeAlsoSections)
{
    const std::string source =
        "/** @brief Adds two numbers. @param a First operand. @param b Second operand. "
        "@returns The sum. @see Reset() */\n"
        "int Add(int a, int b) { return a + b; }\n"
        "int use() { return Add(1, 2); }\n";

    const auto hover = heimdall::CompletionEngine::Hover(
        source, heimdall::ParserOptions {}, source.rfind("Add"));

    ASSERT_TRUE(hover);
    EXPECT_EQ(hover->documentation,
              "Adds two numbers.\n\n"
        "**Parameters**\n\n"
        "- `a` \xE2\x80\x94 First operand.\n"
        "- `b` \xE2\x80\x94 Second operand.\n\n"
        "**Returns**\n\n"
        "The sum.\n\n"
        "**See also**\n\n"
        "Reset()");
}

TEST(DoxygenNavigationSpec, FormatsParameterDirectionsAndBackslashReturnSections)
{
    const std::string source =
        "/**\n * \\brief Copies a value.\n *\n"
        " * \\param[in] source Input value.\n"
        " * \\param[out] destination Output value.\n"
        " * \\return The copied value.\n */\n"
        "int Copy(int source, int& destination) { return destination = source; }\n"
        "int use(int& output) { return Copy(1, output); }\n";

    const auto hover = heimdall::CompletionEngine::Hover(
        source, heimdall::ParserOptions {}, source.rfind("Copy"));

    ASSERT_TRUE(hover);
    EXPECT_EQ(hover->documentation,
              "Copies a value.\n\n"
        "**Parameters**\n\n"
        "- `source` (in) \xE2\x80\x94 Input value.\n"
        "- `destination` (out) \xE2\x80\x94 Output value.\n\n"
        "**Returns**\n\n"
        "The copied value.");
}

TEST(DoxygenNavigationSpec, FormatsGroupedParametersAsSeparateBullets)
{
    const auto markdown = heimdall::RenderDoxygen(
        "@brief Adds values. @param left, right Operands. @return The sum.");

    EXPECT_EQ(markdown,
              "Adds values.\n\n"
        "**Parameters**\n\n"
        "- `left` \xE2\x80\x94 Operands.\n"
        "- `right` \xE2\x80\x94 Operands.\n\n"
        "**Returns**\n\n"
        "The sum.");
}
