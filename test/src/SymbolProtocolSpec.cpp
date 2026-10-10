#include "Document.hpp"
#include "SymbolProtocol.hpp"
#include "WorkspaceSymbolIndex.hpp"

#include <Heimdall/ParseTree.hpp>
#include <Heimdall/SymbolOutline.hpp>

#include <gtest/gtest.h>
#include <simdjson.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace
{

    using heimdall::OutlineKind;
    using heimdall::lsp::LineIndex;

    std::vector<heimdall::OutlineSymbol> Outline(std::string_view source)
    {
        const heimdall::ParseTree tree = heimdall::ParseTree::Parse(source);
        return heimdall::SymbolOutline::Extract(tree);
    }

    std::string HierarchicalJson(std::string_view source)
    {
        LineIndex lines;
        lines.Build(source);
        std::string out;
        heimdall::lsp::AppendDocumentSymbols(Outline(source), lines, out);
        return out;
    }

    struct Parsed
    {
        simdjson::dom::parser parser;
        simdjson::dom::element root;
    };

    // The parser must outlive the element, so the caller owns both.
    void ParseJson(Parsed& parsed, const std::string& json)
    {
        ASSERT_EQ(parsed.parser.parse(json).get(parsed.root), simdjson::SUCCESS) << json;
    }

    std::uint64_t Number(simdjson::dom::element element, const char* key)
    {
        std::uint64_t value = 0;
        EXPECT_EQ(element[key].get_uint64().get(value), simdjson::SUCCESS) << key;
        return value;
    }

    std::string_view Text(simdjson::dom::element element, const char* key)
    {
        std::string_view value;
        EXPECT_EQ(element[key].get_string().get(value), simdjson::SUCCESS) << key;
        return value;
    }

    bool Before(simdjson::dom::element a, simdjson::dom::element b)
    {
        const auto line = [](simdjson::dom::element position)
        {
            return std::pair {Number(position, "line"), Number(position, "character")};
        };
        return line(a) <= line(b);
    }

    void ExpectConsistentRanges(simdjson::dom::array symbols)
    {
        for (const simdjson::dom::element symbol : symbols)
        {
            const auto range     = symbol["range"];
            const auto selection = symbol["selectionRange"];
            EXPECT_TRUE(Before(range["start"], selection["start"])) << Text(symbol, "name");
            EXPECT_TRUE(Before(selection["end"], range["end"])) << Text(symbol, "name");
            simdjson::dom::array children;
            ASSERT_EQ(symbol["children"].get_array().get(children), simdjson::SUCCESS);
            ExpectConsistentRanges(children);
        }
    }

} // namespace

TEST(SymbolProtocolSpec, MapsEveryKindToAnLspSymbolKind)
{
    using heimdall::lsp::ToLspSymbolKind;

    EXPECT_EQ(ToLspSymbolKind(OutlineKind::Namespace), 3);
    EXPECT_EQ(ToLspSymbolKind(OutlineKind::Class), 5);
    EXPECT_EQ(ToLspSymbolKind(OutlineKind::Method), 6);
    EXPECT_EQ(ToLspSymbolKind(OutlineKind::Field), 8);
    EXPECT_EQ(ToLspSymbolKind(OutlineKind::Constructor), 9);
    EXPECT_EQ(ToLspSymbolKind(OutlineKind::Enum), 10);
    EXPECT_EQ(ToLspSymbolKind(OutlineKind::Concept), 11);
    EXPECT_EQ(ToLspSymbolKind(OutlineKind::Function), 12);
    EXPECT_EQ(ToLspSymbolKind(OutlineKind::Variable), 13);
    EXPECT_EQ(ToLspSymbolKind(OutlineKind::EnumMember), 22);
    EXPECT_EQ(ToLspSymbolKind(OutlineKind::Struct), 23);
    EXPECT_EQ(ToLspSymbolKind(OutlineKind::Union), 23);
    EXPECT_EQ(ToLspSymbolKind(OutlineKind::Operator), 25);
    EXPECT_EQ(ToLspSymbolKind(OutlineKind::Destructor), 6);
    EXPECT_EQ(ToLspSymbolKind(OutlineKind::TypeAlias), 5);
}

TEST(SymbolProtocolSpec, EmptyDocumentIsAnEmptyArray)
{
    EXPECT_EQ(HierarchicalJson(""), "[]");
}

TEST(SymbolProtocolSpec, NestsChildrenAndKeepsSiblingsAfterThem)
{
    constexpr std::string_view source = "namespace a {\n"
                                        "struct A { int x; int y; };\n"
                                        "int z;\n"
                                        "}\n"
                                        "int w;\n";

    Parsed parsed;
    ParseJson(parsed, HierarchicalJson(source));

    simdjson::dom::array roots;
    ASSERT_EQ(parsed.root.get_array().get(roots), simdjson::SUCCESS);
    ASSERT_EQ(roots.size(), 2u);
    const simdjson::dom::element ns = roots.at(0).value();
    EXPECT_EQ(Text(ns, "name"), "a");
    EXPECT_EQ(Number(ns, "kind"), 3u);
    simdjson::dom::array inner;
    ASSERT_EQ(ns["children"].get_array().get(inner), simdjson::SUCCESS);
    ASSERT_EQ(inner.size(), 2u);
    EXPECT_EQ(Text(inner.at(0).value(), "name"), "A");
    EXPECT_EQ(Text(inner.at(1).value(), "name"), "z");
    simdjson::dom::array members;
    ASSERT_EQ(inner.at(0).value()["children"].get_array().get(members), simdjson::SUCCESS);
    ASSERT_EQ(members.size(), 2u);
    EXPECT_EQ(Text(members.at(0).value(), "name"), "x");
    EXPECT_EQ(Text(members.at(1).value(), "name"), "y");
    EXPECT_EQ(Text(roots.at(1).value(), "name"), "w");
}

TEST(SymbolProtocolSpec, EverySelectionRangeLiesInsideItsRange)
{
    constexpr std::string_view source = "namespace n {\n"
                                        "class C\n"
                                        "{\n"
                                        "public:\n"
                                        "    C();\n"
                                        "    void M(int a) const;\n"
                                        "};\n"
                                        "enum class E { One, Two };\n"
                                        "template <typename T>\n"
                                        "T Twice(T v) { return v; }\n"
                                        "}\n"
                                        "void C::M(int) const {}\n";

    Parsed parsed;
    ParseJson(parsed, HierarchicalJson(source));

    simdjson::dom::array roots;
    ASSERT_EQ(parsed.root.get_array().get(roots), simdjson::SUCCESS);
    ExpectConsistentRanges(roots);
}

TEST(SymbolProtocolSpec, CountsPositionsInUtf16CodeUnits)
{
    // U+1F600 is four UTF-8 bytes and two UTF-16 units; U+00E9 is two bytes and one unit.
    const std::string source = "/* \xF0\x9F\x98\x80\xC3\xA9 */ int value;\n";

    Parsed parsed;
    ParseJson(parsed, HierarchicalJson(source));

    simdjson::dom::array roots;
    ASSERT_EQ(parsed.root.get_array().get(roots), simdjson::SUCCESS);
    ASSERT_EQ(roots.size(), 1u);
    const auto selection = roots.at(0).value()["selectionRange"];
    EXPECT_EQ(Number(selection["start"], "line"), 0u);
    EXPECT_EQ(Number(selection["start"], "character"), 14u);
    EXPECT_EQ(Number(selection["end"], "character"), 19u);
}

TEST(SymbolProtocolSpec, LineEndingsDoNotShiftPositions)
{
    constexpr std::string_view source = "int first;\r\nint second;\r\n";

    Parsed parsed;
    ParseJson(parsed, HierarchicalJson(source));

    simdjson::dom::array roots;
    ASSERT_EQ(parsed.root.get_array().get(roots), simdjson::SUCCESS);
    ASSERT_EQ(roots.size(), 2u);
    const auto selection = roots.at(1).value()["selectionRange"];
    EXPECT_EQ(Number(selection["start"], "line"), 1u);
    EXPECT_EQ(Number(selection["start"], "character"), 4u);
    EXPECT_EQ(Number(selection["end"], "character"), 10u);
}

TEST(SymbolProtocolSpec, EscapesQuotesAndBackslashesInDetails)
{
    constexpr std::string_view source = "void Log(const char* text = \"a\\\\b\\\"c\");\n";

    Parsed parsed;
    ParseJson(parsed, HierarchicalJson(source));

    simdjson::dom::array roots;
    ASSERT_EQ(parsed.root.get_array().get(roots), simdjson::SUCCESS);
    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(Text(roots.at(0).value(), "detail"), "(const char* text = \"a\\\\b\\\"c\")");
}

TEST(SymbolProtocolSpec, OmitsTheDetailWhenThereIsNone)
{
    Parsed parsed;
    ParseJson(parsed, HierarchicalJson("namespace plain {}\n"));

    simdjson::dom::array roots;
    ASSERT_EQ(parsed.root.get_array().get(roots), simdjson::SUCCESS);
    ASSERT_EQ(roots.size(), 1u);
    simdjson::dom::element detail;
    EXPECT_NE(roots.at(0).value()["detail"].get(detail), simdjson::SUCCESS);
}

TEST(SymbolProtocolSpec, FlatFormCarriesNestingInContainerName)
{
    constexpr std::string_view source = "namespace app { struct Widget { void Run(); }; }\n";
    LineIndex lines;
    lines.Build(source);
    std::string json;

    heimdall::lsp::AppendFlatDocumentSymbols(Outline(source), "file:///w.cpp", lines, json);

    Parsed parsed;
    ParseJson(parsed, json);
    simdjson::dom::array items;
    ASSERT_EQ(parsed.root.get_array().get(items), simdjson::SUCCESS);
    ASSERT_EQ(items.size(), 3u);
    const simdjson::dom::element run = items.at(2).value();
    EXPECT_EQ(Text(run, "name"), "Run");
    EXPECT_EQ(Text(run, "containerName"), "app::Widget");
    EXPECT_EQ(Number(run, "kind"), 6u);
    EXPECT_EQ(Text(run["location"], "uri"), "file:///w.cpp");
    EXPECT_EQ(Number(run["location"]["range"]["start"], "line"), 0u);
}

TEST(SymbolProtocolSpec, WorkspaceSymbolsReportTheNameRangeAndContainer)
{
    constexpr std::string_view source = "namespace app {\n    struct Widget {};\n}\n";
    LineIndex lines;
    lines.Build(source);
    heimdall::lsp::WorkspaceSymbolIndex index;
    index.SetDisk("w", heimdall::lsp::BuildIndexedFile("file:///w.hpp", Outline(source), lines));
    const auto hits = index.Query("widget");
    std::string json;

    heimdall::lsp::AppendWorkspaceSymbols(hits, json);

    Parsed parsed;
    ParseJson(parsed, json);
    simdjson::dom::array items;
    ASSERT_EQ(parsed.root.get_array().get(items), simdjson::SUCCESS);
    ASSERT_EQ(items.size(), 1u);
    const simdjson::dom::element widget = items.at(0).value();
    EXPECT_EQ(Text(widget, "name"), "Widget");
    EXPECT_EQ(Text(widget, "containerName"), "app");
    EXPECT_EQ(Number(widget, "kind"), 23u);
    EXPECT_EQ(Text(widget["location"], "uri"), "file:///w.hpp");
    const auto range = widget["location"]["range"];
    EXPECT_EQ(Number(range["start"], "line"), 1u);
    EXPECT_EQ(Number(range["start"], "character"), 11u);
    EXPECT_EQ(Number(range["end"], "character"), 17u);
}
