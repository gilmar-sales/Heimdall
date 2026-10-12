#include "MacroProtocol.hpp"

#include <Heimdall/MacroEnvironment.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace
{

    using heimdall::lsp::IdentifierAt;
    using heimdall::lsp::MacroAt;

    heimdall::MacroEnvironment Environment(std::string_view text)
    {
        heimdall::MacroEnvironmentOptions options;
        options.probeCompiler = false;
        return heimdall::MacroEnvironment::Build("/nonexistent/main.cpp", text, nullptr, options);
    }

    std::size_t Find(std::string_view text, std::string_view needle, std::size_t nth = 0)
    {
        std::size_t at = 0;
        for (std::size_t i = 0; i <= nth; ++i)
        {
            at = text.find(needle, i == 0 ? 0 : at + 1);
        }

        return at;
    }

} // namespace

TEST(MacroProtocolSpec, FindsTheIdentifierAroundAnOffset)
{
    constexpr std::string_view text = "int answer = 42;";

    const auto inside = IdentifierAt(text, 6);
    const auto atEnd = IdentifierAt(text, 10);
    const auto number = IdentifierAt(text, 14);

    ASSERT_TRUE(inside);
    EXPECT_EQ(text.substr(inside->begin, inside->end - inside->begin), "answer");
    ASSERT_TRUE(atEnd);
    EXPECT_EQ(atEnd->begin, inside->begin);
    EXPECT_FALSE(number);
    EXPECT_FALSE(IdentifierAt(text, 11));
}

TEST(MacroProtocolSpec, ResolvesAMacroOnlyWhereItIsLive)
{
    const std::string text = "int a = LIMIT;\n#define LIMIT 8\nint b = LIMIT;\n";
    const auto env = Environment(text);

    const auto before = IdentifierAt(text, Find(text, "LIMIT", 0));
    const auto after = IdentifierAt(text, Find(text, "LIMIT", 2));

    EXPECT_EQ(MacroAt(env, text, *before), nullptr);
    ASSERT_NE(MacroAt(env, text, *after), nullptr);
    EXPECT_EQ(MacroAt(env, text, *after) -> body, "8");
}

TEST(MacroProtocolSpec, TheDefiningNameResolvesToItself)
{
    const std::string text = "#define LIMIT 8\n";
    const auto env = Environment(text);

    const auto word = IdentifierAt(text, Find(text, "LIMIT"));

    EXPECT_NE(MacroAt(env, text, *word), nullptr);
}

TEST(MacroProtocolSpec, FormatsSignaturesOfObjectAndFunctionLikeMacros)
{
    const auto object = heimdall::MacroTable::ParseDefinition("N 42");
    const auto function = heimdall::MacroTable::ParseDefinition("F(a, b) a + b");
    const auto variadic = heimdall::MacroTable::ParseDefinition("LOG(fmt, ...) printf(fmt, __VA_ARGS__)");
    const auto named = heimdall::MacroTable::ParseDefinition("NAMED(args...) f(args)");

    EXPECT_EQ(heimdall::lsp::MacroSignature(*object), "#define N 42");
    EXPECT_EQ(heimdall::lsp::MacroSignature(*function), "#define F(a, b) a + b");
    EXPECT_EQ(heimdall::lsp::MacroSignature(*variadic),
              "#define LOG(fmt, ...) printf(fmt, __VA_ARGS__)");
    EXPECT_EQ(heimdall::lsp::MacroSignature(*named), "#define NAMED(args...) f(args)");
}

TEST(MacroProtocolSpec, PreviewsTheExpansionOfAnInvocation)
{
    const std::string text = "#define SQ(x) ((x) * (x))\n#define K 3\nint v = SQ(K + 1);\nint w = K;\nint f = SQ;\n";
    const auto env = Environment(text);

    const auto call = IdentifierAt(text, Find(text, "SQ(K"));
    const auto object = IdentifierAt(text, Find(text, "K;"));
    const auto bare = IdentifierAt(text, Find(text, "SQ;"));

    EXPECT_EQ(heimdall::lsp::MacroExpansionPreview(env, text, *call, *MacroAt(env, text, *call)),
              "((3 + 1) * (3 + 1))");
    EXPECT_EQ(heimdall::lsp::MacroExpansionPreview(env, text, *object, *MacroAt(env, text, *object)),
              "3");
    EXPECT_TRUE(
        heimdall::lsp::MacroExpansionPreview(env, text, *bare, *MacroAt(env, text, *bare)).empty());
}

TEST(MacroProtocolSpec, HoverNamesTheOrigin)
{
    const auto builtin = heimdall::MacroTable::ParseDefinition("__cplusplus 202002L");
    auto declared = *builtin;
    declared.origin = heimdall::MacroOrigin::Builtin;
    auto file = *heimdall::MacroTable::ParseDefinition("X 1");
    file.file = "/src/config.h";

    const auto builtinText = heimdall::lsp::MacroHoverMarkdown(declared, "");
    const auto fileText = heimdall::lsp::MacroHoverMarkdown(file, "1");

    EXPECT_NE(builtinText.find("Predefined by the compiler"), std::string::npos);
    EXPECT_NE(fileText.find("`config.h`"), std::string::npos);
    EXPECT_NE(fileText.find("Expands to"), std::string::npos);
}

TEST(MacroProtocolSpec, LocatesTheNameInsideItsDefineLine)
{
    const std::string text = "int a;\n  #  define   LIMIT 8\n";
    const std::size_t line = text.find("  #");

    const auto range = heimdall::lsp::DefinitionNameRange(text, line, "LIMIT");

    ASSERT_TRUE(range);
    EXPECT_EQ(text.substr(range->begin, range->end - range->begin), "LIMIT");
    EXPECT_FALSE(heimdall::lsp::DefinitionNameRange(text, line, "OTHER"));
}

TEST(MacroProtocolSpec, ExpansionEditReplacesTheWholeInvocation)
{
	const std::string text = "#define SQ(x) ((x) * (x))\n#define K 3\nint v = SQ(K + 1);\nint w = K;\n";
	const auto env         = Environment(text);

	const auto call   = IdentifierAt(text, Find(text, "SQ(K"));
	const auto object = IdentifierAt(text, Find(text, "K;"));
	const auto callEdit = heimdall::lsp::MacroExpansionForEdit(env, text, *call, *MacroAt(env, text, *call));
	const auto objectEdit =
		heimdall::lsp::MacroExpansionForEdit(env, text, *object, *MacroAt(env, text, *object));

	ASSERT_TRUE(callEdit);
	EXPECT_EQ(text.substr(callEdit->begin, callEdit->end - callEdit->begin), "SQ(K + 1)");
	EXPECT_EQ(callEdit->text, "((3 + 1) * (3 + 1))");
	ASSERT_TRUE(objectEdit);
	EXPECT_EQ(objectEdit->text, "3");
}

TEST(MacroProtocolSpec, ExpansionEditIsRefusedWhereItWouldBeWrong)
{
	const std::string text = "#define F(a, b) a b\n#define G(x) x\nint a = F(1);\nint b = G;\n#if G(1)\n#endif\n";
	const auto env         = Environment(text);

	const auto wrongArity = IdentifierAt(text, Find(text, "F(1)"));
	const auto notCalled  = IdentifierAt(text, Find(text, "G;"));
	const auto directive  = IdentifierAt(text, Find(text, "G(1)"));

	EXPECT_FALSE(heimdall::lsp::MacroExpansionForEdit(env, text, *wrongArity,
		*MacroAt(env, text, *wrongArity)));
	EXPECT_FALSE(heimdall::lsp::MacroExpansionForEdit(env, text, *notCalled,
		*MacroAt(env, text, *notCalled)));
	EXPECT_FALSE(heimdall::lsp::MacroExpansionForEdit(env, text, *directive,
		*MacroAt(env, text, *directive)));
}

TEST(MacroProtocolSpec, ExpansionEditKeepsNeighbouringOperatorsApart)
{
	const std::string text = "#define NEG -1\nint a = 5 -NEG;\n";
	const auto env         = Environment(text);

	const auto word = IdentifierAt(text, Find(text, "NEG;"));
	const auto edit =
		heimdall::lsp::MacroExpansionForEdit(env, text, *word, *MacroAt(env, text, *word));

	ASSERT_TRUE(edit);
	EXPECT_EQ(edit->text, " -1");
}
