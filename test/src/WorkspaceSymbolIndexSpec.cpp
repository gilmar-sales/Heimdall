#include "Document.hpp"
#include "WorkspaceSymbolIndex.hpp"

#include <Heimdall/ParseTree.hpp>
#include <Heimdall/SymbolOutline.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

    using heimdall::lsp::BuildIndexedFile;
    using heimdall::lsp::IndexedFilePtr;
    using heimdall::lsp::LineIndex;
    using heimdall::lsp::SymbolHit;
    using heimdall::lsp::WorkspaceSymbolIndex;

    IndexedFilePtr Indexed(std::string uri, std::string_view source)
    {
        const heimdall::ParseTree tree = heimdall::ParseTree::Parse(source);
        LineIndex lines;
        lines.Build(source);
        return BuildIndexedFile(std::move(uri), heimdall::SymbolOutline::Extract(tree), lines);
    }

    std::vector<std::string> Names(const std::vector<SymbolHit>& hits)
    {
        std::vector<std::string> names;
        for (const SymbolHit& hit : hits)
        {
            names.emplace_back(hit.Symbol().name);
        }

        return names;
    }

    std::vector<std::string> Where(const std::vector<SymbolHit>& hits)
    {
        std::vector<std::string> places;
        for (const SymbolHit& hit : hits)
        {
            places.push_back(hit.file->uri + ":" + std::to_string(hit.Symbol().start.line));
        }

        return places;
    }

} // namespace

TEST(WorkspaceSymbolIndexSpec, EmptyAndBlankQueriesReturnNothing)
{
    WorkspaceSymbolIndex index;
    index.SetDisk("a", Indexed("file:///a.cpp", "int value;\n"));

    EXPECT_TRUE(index.Query("").empty());
    EXPECT_TRUE(index.Query("   ").empty());
    EXPECT_TRUE(index.Query("value", 0).empty());
}

TEST(WorkspaceSymbolIndexSpec, RanksExactThenPrefixThenSubstringThenSubsequence)
{
    WorkspaceSymbolIndex index;
    index.SetDisk("a", Indexed("file:///a.cpp",
                               "int p_a_r_s_e;\n"
                               "int Reparse;\n"
                               "int ParseTree;\n"
                               "int Parse;\n"));

    const auto hits = index.Query("parse");

    EXPECT_EQ(Names(hits),
              (std::vector<std::string> {"Parse", "ParseTree", "Reparse", "p_a_r_s_e"}));
}

TEST(WorkspaceSymbolIndexSpec, MatchesIgnoringCaseAndSurroundingSpaces)
{
    WorkspaceSymbolIndex index;
    index.SetDisk("a", Indexed("file:///a.cpp", "struct HttpRequest {};\n"));

    EXPECT_EQ(Names(index.Query("  HTTPREQUEST ")), (std::vector<std::string> {"HttpRequest"}));
    EXPECT_EQ(Names(index.Query("hr")), (std::vector<std::string> {"HttpRequest"}));
    EXPECT_TRUE(index.Query("xyz").empty());
}

TEST(WorkspaceSymbolIndexSpec, MatchesQualifiedNames)
{
    WorkspaceSymbolIndex index;
    index.SetDisk("a", Indexed("file:///a.cpp",
                               "namespace app { struct Widget { void Run(); void Stop(); }; }\n"
                               "namespace other { struct Widget { void Run(); }; }\n"));

    EXPECT_EQ(index.Query("app::Widget::Run").size(), 1u);
    const auto other = index.Query("other::Widget");
    ASSERT_EQ(other.size(), 2u);
    EXPECT_EQ(other[0].Symbol().name, "Widget");
    EXPECT_EQ(other[0].Symbol().container, "other");
    EXPECT_EQ(other[1].Symbol().name, "Run");
    EXPECT_EQ(Names(index.Query("Widget::Stop")), (std::vector<std::string> {"Stop"}));
    // Members are found through their qualifier only after names that match directly.
    const auto hits = index.Query("widget");
    ASSERT_EQ(hits.size(), 5u);
    EXPECT_EQ(hits[0].Symbol().name, "Widget");
    EXPECT_EQ(hits[1].Symbol().name, "Widget");
}

TEST(WorkspaceSymbolIndexSpec, FindsOutOfLineDefinitionsByTheirMemberName)
{
    WorkspaceSymbolIndex index;
    index.SetDisk("a", Indexed("file:///a.cpp", "void Widget::Run() {}\n"));

    const auto hits = index.Query("run");

    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].Symbol().name, "Widget::Run");
    EXPECT_EQ(hits[0].Symbol().start.character, 13u);
}

TEST(WorkspaceSymbolIndexSpec, OrdersEqualMatchesDeterministically)
{
    WorkspaceSymbolIndex index;
    index.SetDisk("b", Indexed("file:///b.cpp", "void Log(int);\nvoid Log(double);\n"));
    index.SetDisk("a", Indexed("file:///a.cpp", "void Log(char);\n"));
    index.SetDisk("c", Indexed("file:///c.cpp", "void Log();\n"));

    const auto first  = Where(index.Query("log"));
    const auto second = Where(index.Query("log"));

    EXPECT_EQ(first, second);
    EXPECT_EQ(first, (std::vector<std::string> {"file:///a.cpp:0", "file:///b.cpp:0",
                                                "file:///b.cpp:1", "file:///c.cpp:0"}));
}

TEST(WorkspaceSymbolIndexSpec, LimitKeepsTheBestMatches)
{
    WorkspaceSymbolIndex index;
    std::string source;
    for (int i = 0; i < 50; ++i)
    {
        source += "int item" + std::to_string(i) + ";\n";
    }

    source += "int item;\n";
    index.SetDisk("a", Indexed("file:///a.cpp", source));

    const auto hits = index.Query("item", 3);

    ASSERT_EQ(hits.size(), 3u);
    EXPECT_EQ(hits[0].Symbol().name, "item");
    EXPECT_EQ(hits[1].Symbol().name, "item0");
    EXPECT_EQ(hits[2].Symbol().name, "item1");
}

TEST(WorkspaceSymbolIndexSpec, OpenBufferShadowsTheDiskEntryUntilItCloses)
{
    WorkspaceSymbolIndex index;
    index.SetDisk("file", Indexed("file:///disk.cpp", "int OnDisk;\n"));
    index.SetOpen("file", Indexed("file:///open.cpp", "int InBuffer;\n"));

    EXPECT_TRUE(index.Query("OnDisk").empty());
    EXPECT_EQ(Where(index.Query("InBuffer")), (std::vector<std::string> {"file:///open.cpp:0"}));
    EXPECT_EQ(index.FileCount(), 1u);

    index.ClearOpen("file");

    EXPECT_TRUE(index.Query("InBuffer").empty());
    EXPECT_EQ(Names(index.Query("OnDisk")), (std::vector<std::string> {"OnDisk"}));
}

TEST(WorkspaceSymbolIndexSpec, BuffersOfUnindexedFilesAreSearchable)
{
    WorkspaceSymbolIndex index;
    index.SetOpen("scratch", Indexed("file:///scratch.cpp", "int Scratch;\n"));

    EXPECT_EQ(Names(index.Query("scratch")), (std::vector<std::string> {"Scratch"}));
    EXPECT_EQ(index.FileCount(), 1u);
}

TEST(WorkspaceSymbolIndexSpec, ReplacingAFileDropsItsOldSymbols)
{
    WorkspaceSymbolIndex index;
    index.SetDisk("a", Indexed("file:///a.cpp", "int Before;\n"));
    index.SetDisk("a", Indexed("file:///a.cpp", "int After;\n"));

    EXPECT_TRUE(index.Query("Before").empty());
    EXPECT_EQ(Names(index.Query("After")), (std::vector<std::string> {"After"}));
}

TEST(WorkspaceSymbolIndexSpec, RemovesFilesAndWholeFolders)
{
    WorkspaceSymbolIndex index;
    index.SetDisk("/w/dir/a.cpp", Indexed("file:///w/dir/a.cpp", "int InA;\n"));
    index.SetDisk("/w/dir/sub/b.cpp", Indexed("file:///w/dir/sub/b.cpp", "int InB;\n"));
    index.SetDisk("/w/dirx/c.cpp", Indexed("file:///w/dirx/c.cpp", "int InC;\n"));
    index.SetDisk("/w/d.cpp", Indexed("file:///w/d.cpp", "int InD;\n"));

    index.RemoveDisk("/w/d.cpp");
    EXPECT_TRUE(index.Query("InD").empty());

    index.RemoveDiskUnder("/w/dir");

    EXPECT_TRUE(index.Query("InA").empty());
    EXPECT_TRUE(index.Query("InB").empty());
    EXPECT_EQ(Names(index.Query("InC")), (std::vector<std::string> {"InC"}));
    EXPECT_EQ(index.FileCount(), 1u);
}

TEST(WorkspaceSymbolIndexSpec, AnonymousScopesAreTransparent)
{
    WorkspaceSymbolIndex index;
    index.SetDisk("a", Indexed("file:///a.cpp",
                               "namespace { int hidden; }\n"
                               "namespace app { namespace { struct Impl { int field; }; } }\n"
                               "struct { int bare; } instance;\n"));

    EXPECT_TRUE(index.Query("anonymous").empty());
    const auto hidden = index.Query("hidden");
    ASSERT_EQ(hidden.size(), 1u);
    EXPECT_EQ(hidden[0].Symbol().container, "");
    const auto impl = index.Query("app::Impl");
    ASSERT_GE(impl.size(), 1u);
    EXPECT_EQ(impl[0].Symbol().name, "Impl");
    EXPECT_EQ(impl[0].Symbol().container, "app");
    const auto field = index.Query("field");
    ASSERT_EQ(field.size(), 1u);
    EXPECT_EQ(field[0].Symbol().container, "app::Impl");
    const auto bare = index.Query("bare");
    ASSERT_EQ(bare.size(), 1u);
    EXPECT_EQ(bare[0].Symbol().container, "");
}

TEST(WorkspaceSymbolIndexSpec, FreezesPositionsAgainstTheIndexedText)
{
    // The accent and the CRLF make byte offsets differ from LSP positions.
    const std::string source = "// caf\xC3\xA9\r\nint target;\r\n";
    WorkspaceSymbolIndex index;
    index.SetDisk("a", Indexed("file:///a.cpp", source));

    const auto hits = index.Query("target");

    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].Symbol().start.line, 1u);
    EXPECT_EQ(hits[0].Symbol().start.character, 4u);
    EXPECT_EQ(hits[0].Symbol().end.character, 10u);
}

TEST(WorkspaceSymbolIndexSpec, ACancelledQueryYieldsNothing)
{
    WorkspaceSymbolIndex index;
    index.SetDisk("a", Indexed("file:///a.cpp", "int value;\n"));
    std::stop_source source;
    source.request_stop();

    EXPECT_TRUE(index.Query("value", WorkspaceSymbolIndex::kDefaultLimit, source.get_token()).empty());
}

TEST(WorkspaceSymbolIndexSpec, ResultsStayValidWhileTheIndexKeepsChanging)
{
    WorkspaceSymbolIndex index;
    index.SetDisk("a", Indexed("file:///a.cpp", "int Alpha1;\nint Alpha2;\n"));
    const IndexedFilePtr alpha = Indexed("file:///a.cpp", "int Alpha1;\nint Alpha2;\n");
    const IndexedFilePtr beta  = Indexed("file:///a.cpp", "int Beta1;\nint Beta2;\nint Beta3;\n");
    std::atomic<bool> running {true};
    std::thread writer(
        [&]
        {
            for (int i = 0; running.load(); ++i)
            {
                index.SetDisk("a", i % 2 == 0 ? alpha : beta);
                index.SetOpen("b", i % 3 == 0 ? alpha : beta);
                index.ClearOpen("b");
            }
        });

    std::size_t checked = 0;
    for (int i = 0; i < 2000; ++i)
    {
        const auto hits = index.Query("a");
        for (const SymbolHit& hit : hits)
        {
            const std::string_view name = hit.Symbol().name;
            ASSERT_TRUE(name.starts_with("Alpha") || name.starts_with("Beta")) << name;
            ++checked;
        }
    }

    running.store(false);
    writer.join();
    EXPECT_GT(checked, 0u);
}
