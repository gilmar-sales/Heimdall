#include <Heimdall/IndexSnapshot.hpp>
#include <Heimdall/IndexStore.hpp>
#include <Heimdall/ShardBuilder.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>

namespace
{
    namespace idx = heimdall::index;

    // Owns the source so the tree and model stay valid while the shard is built.
    std::shared_ptr<const idx::FileShard> Shard(std::string path, std::string source)
    {
        const auto text = std::make_shared<const std::string>(std::move(source));
        const auto tree = heimdall::ParseTree::Parse(*text);
        const auto model = heimdall::Binder::Bind(tree);
        return idx::ShardBuilder::Build(model, std::move(path));
    }

    idx::SymbolHash HashOf(const idx::FileShard& shard, std::string_view name, std::size_t nth = 0)
    {
        for (std::uint32_t i = 0; i < shard.Symbols().Size(); ++i)
        {
            if (shard.Name(i) == name && nth--== 0)
            {
                return shard.Symbols().hash[i];
            }
        }

        return 0;
    }

    std::vector<std::string> Paths(
        const idx::IndexSnapshot& snapshot, const std::vector<idx::Location>& locations)
    {
        std::vector<std::string> result;
        for (const auto& location : locations)
        {
            result.push_back(snapshot.Shards()[location.shard]->Path());
        }

        return result;
    }
} // namespace

TEST(FileShard, LocalsAndParametersAreNotIndexed)
{
    const auto shard =
        Shard("a.cpp",
              "namespace n { struct S { int field; }; }\nint f(int param) { int local = param; return local; }\n");

    std::vector<std::string_view> names;
    for (std::uint32_t i = 0; i < shard->Symbols().Size(); ++i)
    {
        names.push_back(shard->Qualified(i));
    }

    EXPECT_NE(std::ranges::find(names, std::string_view("n::S::field")), names.end());
    EXPECT_NE(std::ranges::find(names, std::string_view("f")), names.end());
    EXPECT_EQ(std::ranges::find(names, std::string_view("local")), names.end());
    EXPECT_EQ(std::ranges::find(names, std::string_view("param")), names.end());
}

TEST(FileShard, OccurrencesAreSortedAndSymbolAtFindsDeclarationAndUse)
{
    const std::string source = "int value;\nint f() { return value; }\n";
    const auto shard = Shard("a.cpp", source);

    const auto& offsets = shard->Occurrences().offset;
    EXPECT_TRUE(std::ranges::is_sorted(offsets));
    const auto declared = shard->SymbolAt(static_cast<std::uint32_t>(source.find("value")));
    const auto used = shard->SymbolAt(static_cast<std::uint32_t>(source.rfind("value")));
    ASSERT_NE(declared, idx::kInvalid);
    EXPECT_EQ(declared, used);
    EXPECT_EQ(shard->OccurrencesOf(declared).size(), 2u);
}

TEST(FileShard, DeclarationAndDefinitionShareIdentityAcrossFiles)
{
    const auto header = Shard("lib.hpp", "void work(int);\n");
    const auto source = Shard("lib.cpp", "#include \"lib.hpp\"\nvoid work(int) {}\n");

    const auto snapshot = idx::IndexSnapshot::Build({header, source});

    const auto hash = HashOf(*header, "work");
    EXPECT_EQ(hash, HashOf(*source, "work"));
    EXPECT_EQ(snapshot->Declarations(hash).size(), 2u);
    const auto definitions = snapshot->Definitions(hash);
    ASSERT_EQ(definitions.size(), 1u);
    EXPECT_EQ(snapshot->Shards()[definitions[0].shard]->Path(), "lib.cpp");
}

TEST(FileShard, ForwardDeclaredClassIsNeverADefinition)
{
    const auto shard = Shard("a.hpp", "struct Fwd;\nstruct Full {};\n");

    const auto role =[&](std::string_view name)
    {
        for (std::uint32_t i = 0; i < shard->Symbols().Size(); ++i)
        {
            if (shard->Name(i) == name)
            {
                return shard->Symbols().role[i];
            }
        }

        return idx::Role::Reference;
    };

    EXPECT_NE(role("Fwd"), idx::Role::Definition);
    EXPECT_EQ(role("Full"), idx::Role::Definition);
}

TEST(FileShard, OverloadsHaveDistinctIdentity)
{
    const auto shard = Shard("a.cpp", "void f(int);\nvoid f(double);\n");

    EXPECT_NE(HashOf(*shard, "f", 0), HashOf(*shard, "f", 1));
}

TEST(FileShard, InternalLinkageSymbolsDoNotShareIdentityAcrossFiles)
{
    const auto first = Shard("a.cpp", "static int helper() { return 1; }\n");
    const auto second = Shard("b.cpp", "static int helper() { return 2; }\n");

    EXPECT_NE(HashOf(*first, "helper"), HashOf(*second, "helper"));
}

TEST(FileShard, IncludeSpellingsAreRecorded)
{
    const auto shard = Shard("a.cpp", "#include <vector>\n#include \"dir/x.hpp\"\n");

    ASSERT_EQ(shard->Includes().size(), 2u);
    EXPECT_EQ(shard->Strings().Text(shard->Includes()[0]), "vector");
    EXPECT_EQ(shard->Strings().Text(shard->Includes()[1]), "dir/x.hpp");
}

TEST(IndexSnapshot, ReferencesCrossFilesOnlyThroughIncludes)
{
    const auto header = Shard("src/api.hpp", "int Compute();\n");
    const auto includer = Shard("src/use.cpp", "#include \"api.hpp\"\nint a() { return Compute(); }\n");
    const auto stranger = Shard("src/other.cpp", "int b() { return Compute(); }\n");
    const auto snapshot = idx::IndexSnapshot::Build({header, includer, stranger});

    const auto references = snapshot->References(HashOf(*header, "Compute"));

    const auto paths = Paths(*snapshot, references);
    EXPECT_NE(std::ranges::find(paths, std::string("src/api.hpp")), paths.end());
    EXPECT_NE(std::ranges::find(paths, std::string("src/use.cpp")), paths.end());
    EXPECT_EQ(std::ranges::find(paths, std::string("src/other.cpp")), paths.end());
}

TEST(IndexSnapshot, CrossFileUsesAreMarkedAsNameMatched)
{
    const auto header = Shard("api.hpp", "int Compute();\n");
    const auto includer = Shard("use.cpp", "#include \"api.hpp\"\nint a() { return Compute(); }\n");
    const auto snapshot = idx::IndexSnapshot::Build({header, includer});

    const auto references = snapshot->References(HashOf(*header, "Compute"));

    const auto use = std::ranges::find_if(references,
        [&](const idx::Location& l)
        {
            return snapshot->Shards()[l.shard]->Path() == "use.cpp";
    });
    ASSERT_NE(use, references.end());
    EXPECT_EQ(use->resolution, idx::Resolution::Unresolved);
}

TEST(IndexSnapshot, CandidatesResolveANameThroughIncludedHeadersOnly)
{
    const std::string use = "#include \"api.hpp\"\nint a() { return Compute(); }\n";
    const auto header = Shard("api.hpp", "int Compute();\n");
    const auto includer = Shard("use.cpp", use);
    const auto stranger = Shard("other.cpp", "int Compute();\nint b() { return Compute(); }\n");
    const auto snapshot = idx::IndexSnapshot::Build({header, includer, stranger});
    const auto offset = static_cast<std::uint32_t>(use.rfind("Compute"));

    const auto candidates = snapshot->Candidates(snapshot->ShardFor("use.cpp"), offset);

    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(snapshot->Shards()[candidates[0].shard]->Path(), "api.hpp");
}

TEST(IndexSnapshot, SubtypesAcrossFilesAreFoundByWrittenBaseName)
{
    const auto base = Shard("base.hpp", "struct Base {};\n");
    const auto derived = Shard("derived.hpp", "#include \"base.hpp\"\nstruct Derived : Base {};\n");
    const auto snapshot = idx::IndexSnapshot::Build({base, derived});

    const auto subtypes = snapshot->Subtypes(HashOf(*base, "Base"));

    ASSERT_EQ(subtypes.size(), 1u);
    EXPECT_EQ(snapshot->Shards()[subtypes[0].symbol.shard]->Name(subtypes[0].symbol.symbol),
              "Derived");
    EXPECT_FALSE(subtypes[0].exact);
}

TEST(IndexSnapshot, SubtypesInTheSameFileAreExact)
{
    const auto shard = Shard("a.hpp", "struct Base {};\nstruct Derived : Base {};\n");
    const auto snapshot = idx::IndexSnapshot::Build({shard});

    const auto subtypes = snapshot->Subtypes(HashOf(*shard, "Base"));

    ASSERT_EQ(subtypes.size(), 1u);
    EXPECT_TRUE(subtypes[0].exact);
}

TEST(IndexSnapshot, SupertypesFollowBasesIntoOtherFiles)
{
    const auto base = Shard("base.hpp", "struct Base {};\n");
    const auto derived = Shard("derived.hpp", "#include \"base.hpp\"\nstruct Derived : Base {};\n");
    const auto snapshot = idx::IndexSnapshot::Build({base, derived});

    const auto supertypes = snapshot->Supertypes(HashOf(*derived, "Derived"));

    ASSERT_EQ(supertypes.size(), 1u);
    EXPECT_EQ(snapshot->Shards()[supertypes[0].symbol.shard]->Path(), "base.hpp");
}

TEST(IndexSnapshot, SearchRanksExactThenPrefixThenSubstring)
{
    const auto shard = Shard("a.cpp", "int buffer_size;\nint size;\nint size_of;\nint unrelated;\n");
    const auto snapshot = idx::IndexSnapshot::Build({shard});

    const auto hits = snapshot->Search("size", 10);

    ASSERT_EQ(hits.size(), 3u);
    const auto name =[&](std::size_t i)
    {
        return shard->Name(hits[i].symbol);
    };
    EXPECT_EQ(name(0), "size");
    EXPECT_EQ(name(1), "size_of");
    EXPECT_EQ(name(2), "buffer_size");
}

TEST(IndexSnapshot, ShardForFindsByPath)
{
    const auto first = Shard("b.cpp", "int x;\n");
    const auto second = Shard("a.cpp", "int y;\n");
    const auto snapshot = idx::IndexSnapshot::Build({first, second});

    EXPECT_EQ(snapshot->ShardFor("a.cpp"), 1u);
    EXPECT_EQ(snapshot->ShardFor("missing.cpp"), idx::kInvalid);
}

TEST(IndexStore, OpenBufferShadowsDiskShardUntilItIsCleared)
{
    idx::IndexStore store;
    store.SetDisk("a.cpp", Shard("a.cpp", "int fromDisk;\n"));
    store.SetOpen("a.cpp", Shard("a.cpp", "int fromBuffer;\n"));

    const auto shadowed = store.Snapshot();
    store.ClearOpen("a.cpp");
    const auto restored = store.Snapshot();

    EXPECT_EQ(shadowed->Search("fromBuffer", 5).size(), 1u);
    EXPECT_TRUE(shadowed->Search("fromDisk", 5).empty());
    EXPECT_EQ(restored->Search("fromDisk", 5).size(), 1u);
    EXPECT_EQ(store.FileCount(), 1u);
}

TEST(IndexStore, SnapshotIsSharedUntilTheNextChange)
{
    idx::IndexStore store;
    store.SetDisk("a.cpp", Shard("a.cpp", "int a;\n"));

    const auto first = store.Snapshot();
    const auto again = store.Snapshot();
    store.SetDisk("b.cpp", Shard("b.cpp", "int b;\n"));
    const auto second = store.Snapshot();

    EXPECT_EQ(first, again);
    EXPECT_NE(first, second);
    EXPECT_EQ(first->Shards().size(), 1u);
    EXPECT_EQ(second->Shards().size(), 2u);
}

TEST(IndexSnapshot, CandidatesAreEmptyForANameTheShardResolvedItself)
{
    const std::string source = "int Compute();\nint a() { return Compute(); }\n";
    const auto file = Shard("use.cpp", source);
    const auto snapshot = idx::IndexSnapshot::Build({file});

    const auto candidates = snapshot->Candidates(0,
        static_cast<std::uint32_t>(source.rfind("Compute")));

    EXPECT_TRUE(candidates.empty());
}

TEST(IndexSnapshot, CandidatesAreEmptyWhenNothingDeclaresTheName)
{
    const std::string source = "int a() { return Missing(); }\n";
    const auto file = Shard("use.cpp", source);
    const auto snapshot = idx::IndexSnapshot::Build({file});

    const auto candidates = snapshot->Candidates(0, static_cast<std::uint32_t>(source.find("Missing")));

    EXPECT_TRUE(candidates.empty());
}

TEST(IndexStore, RemoveDiskUnderDropsEveryShardBelowAFolder)
{
    idx::IndexStore store;
    store.SetDisk("proj/src/a.cpp", Shard("proj/src/a.cpp", "int a;\n"));
    store.SetDisk("proj/src/b.cpp", Shard("proj/src/b.cpp", "int b;\n"));
    store.SetDisk("proj/keep.cpp", Shard("proj/keep.cpp", "int c;\n"));

    store.RemoveDiskUnder("proj/src");

    EXPECT_EQ(store.FileCount(), 1u);
    EXPECT_EQ(store.Snapshot()->ShardFor("proj/keep.cpp"), 0u);
}

TEST(FileShard, TypeNamesInDeclarationsAreRecordedAsUses)
{
    const std::string source = "struct Widget {};\nWidget make(const Widget& other);\nns::Widget qualified;\n";
    const auto shard = Shard("a.cpp", source);

    std::size_t uses = 0;
    for (std::uint32_t i = 0; i < shard->Occurrences().Size(); ++i)
    {
        uses += shard->Strings().Text(shard->Occurrences().name[i]) == "Widget" &&
            shard->Occurrences().role[i] == idx::Role::Reference;
    }

    EXPECT_EQ(uses, 3u);
}

TEST(IndexSnapshot, TypeUsesInIncludingFilesAreReferences)
{
    const auto header = Shard("widget.hpp", "struct Widget {};\n");
    const auto includer = Shard("use.cpp", "#include \"widget.hpp\"\nWidget make();\n");
    const auto snapshot = idx::IndexSnapshot::Build({header, includer});

    const auto paths = Paths(*snapshot, snapshot->References(HashOf(*header, "Widget")));

    EXPECT_NE(std::ranges::find(paths, std::string("use.cpp")), paths.end());
}
