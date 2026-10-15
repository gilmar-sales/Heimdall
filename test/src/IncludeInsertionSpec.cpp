#include <gtest/gtest.h>

#include <Heimdall/CompileDatabase.hpp>
#include <Heimdall/IncludeIndex.hpp>
#include <Heimdall/IncludeInsertion.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace
{
    std::string Apply(std::string_view source, std::string_view spelled)
    {
        const auto edit = heimdall::PlanIncludeInsertion(source, spelled);
        EXPECT_TRUE(edit.has_value()) << spelled;
        if (!edit)
        {
            return std::string(source);
        }

        std::string result(source);
        result.insert(edit->offset, edit->text);
        return result;
    }
} // namespace

TEST(IncludeInsertionSpec, AlreadyIncludedHeadersNeedNoEdit)
{
    constexpr std::string_view source =
        "#include <vector>\n"
        "#  include \"widget.hpp\" // comment\n"
        "int x;\n";

    EXPECT_FALSE(heimdall::PlanIncludeInsertion(source, "<vector>").has_value());
    EXPECT_FALSE(heimdall::PlanIncludeInsertion(source, "\"widget.hpp\"").has_value());
}

TEST(IncludeInsertionSpec, QuotedIncludeKeepsTheQuotedGroupSorted)
{
    constexpr std::string_view source =
        "#include <vector>\n"
        "\n"
        "#include \"alpha.hpp\"\n"
        "#include \"gamma.hpp\"\n"
        "\n"
        "int x;\n";

    EXPECT_EQ(Apply(source, "\"Beta.hpp\""),
              "#include <vector>\n"
        "\n"
        "#include \"alpha.hpp\"\n"
        "#include \"Beta.hpp\"\n"
        "#include \"gamma.hpp\"\n"
        "\n"
        "int x;\n");
    EXPECT_EQ(Apply(source, "\"zeta.hpp\""),
              "#include <vector>\n"
        "\n"
        "#include \"alpha.hpp\"\n"
        "#include \"gamma.hpp\"\n"
        "#include \"zeta.hpp\"\n"
        "\n"
        "int x;\n");
}

TEST(IncludeInsertionSpec, AngledIncludeGoesBeforeQuotedOnes)
{
    constexpr std::string_view source =
        "#include \"alpha.hpp\"\n"
        "int x;\n";

    EXPECT_EQ(Apply(source, "<map>"),
              "#include <map>\n"
        "#include \"alpha.hpp\"\n"
        "int x;\n");
}

TEST(IncludeInsertionSpec, FirstIncludeFollowsPragmaOnceOrTheGuard)
{
    EXPECT_EQ(Apply("// File header.\n#pragma once\n\nstruct S {};\n", "\"a.hpp\""),
              "// File header.\n#pragma once\n#include \"a.hpp\"\n\nstruct S {};\n");
    EXPECT_EQ(Apply("#ifndef S_HPP\n#define S_HPP\nstruct S {};\n#endif\n", "\"a.hpp\""),
              "#ifndef S_HPP\n#define S_HPP\n#include \"a.hpp\"\nstruct S {};\n#endif\n");
}

TEST(IncludeInsertionSpec, FirstIncludeFollowsTheLeadingFileComment)
{
    EXPECT_EQ(Apply("/* License\n   text */\n// More.\nint x;\n", "<vector>"),
              "/* License\n   text */\n// More.\n#include <vector>\nint x;\n");
    EXPECT_EQ(Apply("int x;\n", "<vector>"), "#include <vector>\nint x;\n");
    EXPECT_EQ(Apply("", "<vector>"), "#include <vector>\n");
}

TEST(IncludeInsertionSpec, ConditionalIncludesAreNotTheIncludeBlock)
{
    constexpr std::string_view source =
        "#include <vector>\n"
        "#ifdef _WIN32\n"
        "#include <windows.h>\n"
        "#endif\n"
        "int x;\n";

    EXPECT_EQ(Apply(source, "<string>"),
              "#include <string>\n"
        "#include <vector>\n"
        "#ifdef _WIN32\n"
        "#include <windows.h>\n"
        "#endif\n"
        "int x;\n");
}

TEST(IncludeInsertionSpec, LastLineWithoutNewline)
{
    EXPECT_EQ(Apply("#include \"a.hpp\"", "\"b.hpp\""), "#include \"a.hpp\"\n#include \"b.hpp\"\n");
}

TEST(IncludeInsertionSpec, HeaderIsSpelledFromTheShortestSearchDirectory)
{
    const std::filesystem::path root = std::filesystem::path("/project");
    heimdall::CompileCommand command;
    command.directory = root;
    command.include_directories = {root / "include", "src"};
    command.arguments = {"heimdall-no-such-compiler"};

    const auto spell = [&](const std::filesystem::path& header, const std::filesystem::path& file)
    {
        return heimdall::IncludeIndex::SpellHeader(header, file, &command);
    };

    EXPECT_EQ(spell(root / "src/app/widget.hpp", root / "src/app/main.cpp"), "\"widget.hpp\"");
    EXPECT_EQ(spell(root / "include/lib/api.hpp", root / "src/main.cpp"), "\"lib/api.hpp\"");
    EXPECT_EQ(spell(root / "src/core/types.hpp", root / "src/app/main.cpp"), "\"core/types.hpp\"");
    EXPECT_EQ(spell(root / "other/x.hpp", root / "src/main.cpp"), "\"../other/x.hpp\"");
    EXPECT_EQ(spell({}, root / "src/main.cpp"), "");
}
