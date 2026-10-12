#include <Heimdall/IndexSnapshot.hpp>
#include <Heimdall/IndexStore.hpp>
#include <Heimdall/ShardBuilder.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <string>
#include <vector>

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

TEST(IndexSnapshot, CrossFileUsesAreResolvedThroughTheIncludeClosure)
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
    EXPECT_EQ(use->resolution, idx::Resolution::Resolved);
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

TEST(IndexSnapshot, SubtypesAcrossFilesResolveTheBaseThroughIncludes)
{
    const auto base = Shard("base.hpp", "struct Base {};\n");
    const auto derived = Shard("derived.hpp", "#include \"base.hpp\"\nstruct Derived : Base {};\n");
    const auto snapshot = idx::IndexSnapshot::Build({base, derived});

    const auto subtypes = snapshot->Subtypes(HashOf(*base, "Base"));

    ASSERT_EQ(subtypes.size(), 1u);
    EXPECT_EQ(snapshot->Shards()[subtypes[0].symbol.shard]->Name(subtypes[0].symbol.symbol),
              "Derived");
    EXPECT_TRUE(subtypes[0].exact);
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

namespace
{
    std::size_t UsesNamed(const idx::FileShard& shard, std::string_view name)
    {
        std::size_t uses = 0;
        for (std::uint32_t i = 0; i < shard.Occurrences().Size(); ++i)
        {
            uses += shard.Strings().Text(shard.Occurrences().name[i]) == name &&
                shard.Occurrences().role[i] == idx::Role::Reference;
        }

        return uses;
    }
} // namespace

TEST(FileShard, BaseClassNamesAreRecordedAsResolvedUses)
{
    const auto shard = Shard("a.cpp",
        "struct Base {};\nstruct A : Base {};\nstruct B : public Base {};\n");

    EXPECT_EQ(UsesNamed(*shard, "Base"), 2u);
    std::size_t resolved = 0;
    for (std::uint32_t i = 0; i < shard->Occurrences().Size(); ++i)
    {
        resolved += shard->Strings().Text(shard->Occurrences().name[i]) == "Base" &&
            shard->Occurrences().role[i] == idx::Role::Reference &&
            shard->Occurrences().resolution[i] == idx::Resolution::Resolved;
    }

    EXPECT_EQ(resolved, 2u);
}

TEST(FileShard, TypeNamesInExpressionsAreRecordedAsUses)
{
    const auto shard = Shard("a.cpp",
        "struct W { static int Make(); };\n"
        "void f(int* p)\n{\n"
        "    W a = W{};\n"
        "    W* b = new W;\n"
        "    int n = sizeof(W);\n"
        "    int m = W::Make();\n"
        "    auto c = static_cast<const W&>(a);\n"
        "    try {} catch (const W&) {}\n"
        "}\n");

    EXPECT_EQ(UsesNamed(*shard, "W"), 8u);
}

TEST(FileShard, ForwardDeclarationIsADeclarationSharingTheDefinitionIdentity)
{
    const auto forward = Shard("fwd.hpp",
        "struct Widget;\nclass Other;\nenum class Mode : int;\nnamespace n { struct Inner; }\n");
    const auto definition = Shard("widget.hpp",
        "struct Widget {};\nnamespace n { struct Inner {}; }\n");
    const auto snapshot = idx::IndexSnapshot::Build({forward, definition});

    const auto hash = HashOf(*forward, "Widget");

    EXPECT_NE(hash, 0u);
    EXPECT_EQ(hash, HashOf(*definition, "Widget"));
    EXPECT_NE(HashOf(*forward, "Other"), 0u);
    EXPECT_NE(HashOf(*forward, "Mode"), 0u);
    EXPECT_EQ(HashOf(*forward, "Inner"), HashOf(*definition, "Inner"));
    ASSERT_EQ(snapshot->Definitions(hash).size(), 1u);
    EXPECT_EQ(snapshot->Shards()[snapshot->Definitions(hash)[0].shard]->Path(), "widget.hpp");
    EXPECT_EQ(snapshot->Declarations(hash).size(), 2u);
}

TEST(FileShard, ForwardDeclarationFollowedByDefinitionInOneFileIsOneSymbol)
{
    const auto shard = Shard("a.cpp", "struct Widget;\nstruct Widget {};\nWidget* p;\n");

    std::size_t symbols = 0;
    for (std::uint32_t i = 0; i < shard->Symbols().Size(); ++i)
    {
        symbols += shard->Name(i) == "Widget";
    }

    EXPECT_EQ(symbols, 1u);
    EXPECT_EQ(UsesNamed(*shard, "Widget"), 1u);
    EXPECT_EQ(shard->OccurrencesOf(0).size(), 3u);
}

TEST(IndexSnapshot, BaseClassUsesAndForwardDeclarationsAreReferences)
{
    const auto base = Shard("base.hpp", "struct Base {};\n");
    const auto forward = Shard("fwd.hpp", "struct Base;\n");
    const auto derived = Shard("derived.hpp", "#include \"base.hpp\"\nstruct Derived : Base {};\n");
    const auto snapshot = idx::IndexSnapshot::Build({base, forward, derived});

    const auto paths = Paths(*snapshot, snapshot->References(HashOf(*base, "Base")));

    EXPECT_NE(std::ranges::find(paths, std::string("derived.hpp")), paths.end());
    EXPECT_NE(std::ranges::find(paths, std::string("fwd.hpp")), paths.end());
}

TEST(FileShard, OccurrencesRecordScopeQualifierMemberAndArity)
{
    const std::string source =
        "namespace a { struct B { int Size(); }; int Call(int, int = 1); }\n"
        "int f(a::B& b)\n{\n    a::Call(1);\n    return b.Size();\n}\n";
    const auto shard = Shard("a.cpp", source);

    const auto call = shard->OccurrenceAt(static_cast<std::uint32_t>(source.find("Call(1)")));
    const auto size = shard->OccurrenceAt(static_cast<std::uint32_t>(source.rfind("Size")));

    ASSERT_NE(call, idx::kInvalid);
    ASSERT_NE(size, idx::kInvalid);
    const auto& occ = shard->Occurrences();
    EXPECT_EQ(shard->Strings().Text(occ.qualifier[call]), "a");
    EXPECT_EQ(occ.argCount[call], 1);
    EXPECT_EQ(occ.flags[size] & idx::kOccurrenceMember, idx::kOccurrenceMember);
    EXPECT_EQ(occ.argCount[size], 0);
    EXPECT_EQ(occ.qualifier[size], idx::kInvalid);
}

TEST(FileShard, ScopesRecordNamespaceAndClassExtentsAndOutOfLineBodies)
{
    const std::string source =
        "namespace a { struct B { void F(); }; }\n"
        "void a::B::F() { int inside = 0; }\n";
    const auto shard = Shard("a.cpp", source);

    const auto inClass = shard->Occurrences().scope[shard->OccurrenceAt(
        static_cast<std::uint32_t>(source.find("F();")))];

    ASSERT_NE(inClass, idx::kInvalid);
    EXPECT_EQ(shard->Strings().Text(shard->Scopes().qualified[inClass]), "a::B");
    bool bodyScope = false;
    for (std::uint32_t i = 0; i < shard->Scopes().Size(); ++i)
    {
        bodyScope |= shard->Strings().Text(shard->Scopes().qualified[i]) == "a::B" &&
            shard->Scopes().begin[i] > source.find('\n');
    }

    EXPECT_TRUE(bodyScope);
}

TEST(FileShard, UsingDirectivesDeclarationsAndAliasesAreRecordedOutsideBodies)
{
    const auto shard = Shard("a.cpp",
        "namespace a { struct B {}; }\nusing namespace a;\nusing a::B;\nusing Alias = a::B;\n"
        "void f() { using namespace a; }\n");

    const auto& usings = shard->Usings();

    ASSERT_EQ(usings.Size(), 3u);
    EXPECT_EQ(usings.kind[0], idx::UsingKind::Namespace);
    EXPECT_EQ(shard->Strings().Text(usings.path[0]), "a");
    EXPECT_EQ(usings.kind[1], idx::UsingKind::Declaration);
    EXPECT_EQ(shard->Strings().Text(usings.name[1]), "B");
    EXPECT_EQ(usings.kind[2], idx::UsingKind::Alias);
    EXPECT_EQ(shard->Strings().Text(usings.name[2]), "Alias");
    EXPECT_EQ(shard->Strings().Text(usings.path[2]), "a::B");
}

TEST(FileShard, FunctionsRecordRequiredAndMaximumParameters)
{
    const auto shard = Shard("a.cpp",
        "void none();\nvoid v(void);\nvoid two(int, int = 1);\nvoid var(int, ...);\n");

    const auto bounds =[&](std::string_view name)
    {
        for (std::uint32_t i = 0; i < shard->Symbols().Size(); ++i)
        {
            if (shard->Name(i) == name)
            {
                return std::pair<int, int>(shard->Symbols().paramMin[i], shard->Symbols().paramMax[i]);
            }
        }

        return std::pair<int, int>(-1, -1);
    };

    EXPECT_EQ(bounds("none"), (std::pair<int, int>(0, 0)));
    EXPECT_EQ(bounds("v"), (std::pair<int, int>(0, 0)));
    EXPECT_EQ(bounds("two"), (std::pair<int, int>(1, 2)));
    EXPECT_EQ(bounds("var"), (std::pair<int, int>(1, idx::kNoArity)));
}

namespace
{
    bool UsedIn(const idx::IndexSnapshot& snapshot, const std::vector<idx::Location>& locations,
        std::string_view path)
    {
        return std::ranges::any_of(locations,
                                   [&](const idx::Location& l)
            {
                return snapshot.Shards()[l.shard]->Path() == path;
        });
    }
} // namespace

TEST(IndexResolution, SameNameInTwoNamespacesIsNotConflated)
{
    const auto first = Shard("a.hpp", "namespace a { struct Widget {}; }\n");
    const auto second = Shard("b.hpp", "namespace b { struct Widget {}; }\n");
    const auto user = Shard("use.cpp", "#include \"a.hpp\"\n#include \"b.hpp\"\na::Widget x;\n");
    const auto other = Shard("other.cpp", "#include \"a.hpp\"\n#include \"b.hpp\"\nb::Widget y;\n");
    const auto snapshot = idx::IndexSnapshot::Build({first, second, user, other});

    const auto inA = snapshot->References(HashOf(*first, "Widget"));
    const auto inB = snapshot->References(HashOf(*second, "Widget"));

    EXPECT_TRUE(UsedIn(*snapshot, inA, "use.cpp"));
    EXPECT_FALSE(UsedIn(*snapshot, inA, "other.cpp"));
    EXPECT_TRUE(UsedIn(*snapshot, inB, "other.cpp"));
    EXPECT_FALSE(UsedIn(*snapshot, inB, "use.cpp"));
}

TEST(IndexStore, SnapshotsPinTheirGenerationAcrossUpdates)
{
    idx::IndexStore store;
    store.SetDisk("a.cpp", Shard("a.cpp", "int first;\n"));
    const auto before = store.Snapshot();
    const auto revision = before->Revision();

    store.SetDisk("a.cpp", Shard("a.cpp", "int second;\n"));
    const auto after = store.Snapshot();

    EXPECT_GT(after->Revision(), revision);
    EXPECT_EQ(before->Revision(), revision);
    EXPECT_EQ(store.Snapshot()->Revision(), after->Revision());
}

TEST(IndexSnapshot, HashCollisionsNeverMergeDifferentIdentityKeys)
{
	const auto first = Shard("a.hpp", "struct First {};\n");
	auto second = std::make_shared<idx::FileShard>(*Shard("b.hpp", "struct Second {};\n"));
	auto& symbols = const_cast<idx::SymbolColumns&>(second->Symbols());
	symbols.hash.front() = first->Symbols().hash.front();

	const auto snapshot = idx::IndexSnapshot::Build({first, second});

	EXPECT_TRUE(snapshot->Declarations(first->Symbols().hash.front()).empty());
	EXPECT_TRUE(snapshot->References(first->Symbols().hash.front()).empty());
	EXPECT_EQ(snapshot->Named("First").size(), 1u);
	EXPECT_EQ(snapshot->Named("Second").size(), 1u);
}

TEST(FileShard, FunctionIdentityKeepsParameterSpellingAndCvRefWithoutNamesOrDefaults)
{
	const auto declared = Shard("a.hpp", "struct A { int Run(int value = 1) const &; };\n");
	const auto defined = Shard("a.cpp", "int A::Run(int other) const & { return other; }\n");
	const auto different = Shard("b.hpp", "struct A { int Run(long value) const &; };\n");

	EXPECT_EQ(HashOf(*declared, "Run"), HashOf(*defined, "Run"));
	EXPECT_NE(HashOf(*declared, "Run"), HashOf(*different, "Run"));
}

TEST(IndexSnapshot, ExactIncludeTargetsDoNotExposeHomonymousHeaders)
{
	const auto first = Shard("first/widget.hpp", "struct First {};\n");
	const auto second = Shard("second/widget.hpp", "struct Second {};\n");
	const auto tree = heimdall::ParseTree::Parse("#include \"widget.hpp\"\nSecond value;\n");
	const auto model = heimdall::Binder::Bind(tree);
	const auto user = idx::ShardBuilder::Build(model, "use.cpp", 0,
		[](std::string_view, bool, std::uint32_t) -> std::optional<std::string>
		{
			return "second/widget.hpp";
		});

	const auto snapshot = idx::IndexSnapshot::Build({first, second, user});
	const auto visible = snapshot->Visible(2);

	EXPECT_TRUE(user->ExactIncludes());
	EXPECT_FALSE(visible[0]);
	EXPECT_TRUE(visible[1]);
}

TEST(FileShard, ConfigurationFingerprintIsStableAndSensitiveToSearchOrder)
{
	heimdall::ParserOptions first;
	first.predefined_macros.emplace("FIRST", "1");
	first.predefined_macros.emplace("SECOND", "2");
	heimdall::ParserOptions reordered;
	reordered.predefined_macros.emplace("SECOND", "2");
	reordered.predefined_macros.emplace("FIRST", "1");
	heimdall::CompileCommand command;
	command.include_directories = {"first", "second"};
	auto other = command;
	std::ranges::reverse(other.include_directories);

	EXPECT_EQ(idx::ShardBuilder::ConfigurationHash(first), idx::ShardBuilder::ConfigurationHash(reordered));
	EXPECT_NE(idx::ShardBuilder::ConfigurationHash(first, &command), idx::ShardBuilder::ConfigurationHash(first, &other));
	reordered.predefined_macros["FIRST"] = "3";
	EXPECT_NE(idx::ShardBuilder::ConfigurationHash(first), idx::ShardBuilder::ConfigurationHash(reordered));
}

TEST(IndexResolution, InnerScopeHidesOuterDeclaration)
{
    const auto header = Shard("h.hpp", "struct Item {};\nnamespace n { struct Item {}; }\n");
    const auto user = Shard("use.cpp",
        "#include \"h.hpp\"\nnamespace n { Item inside; }\nItem outside;\n");
    const auto snapshot = idx::IndexSnapshot::Build({header, user});

    const auto outer = snapshot->References(HashOf(*header, "Item", 0));
    const auto inner = snapshot->References(HashOf(*header, "Item", 1));

    const auto usesIn =[&](const std::vector<idx::Location>& locations)
    {
        std::size_t count = 0;
        for (const auto& l : locations)
        {
            count += snapshot->Shards()[l.shard]->Path() == "use.cpp";
        }

        return count;
    };
    EXPECT_EQ(usesIn(outer), 1u);
    EXPECT_EQ(usesIn(inner), 1u);
}

TEST(IndexResolution, UsingNamespaceBringsNamesIntoScope)
{
    const auto header = Shard("h.hpp", "namespace lib { struct Tool {}; }\n");
    const auto user = Shard("use.cpp", "#include \"h.hpp\"\nusing namespace lib;\nTool t;\n");
    const auto bare = Shard("bare.cpp", "#include \"h.hpp\"\nTool t;\n");
    const auto snapshot = idx::IndexSnapshot::Build({header, user, bare});

    const auto refs = snapshot->References(HashOf(*header, "Tool"));

    EXPECT_TRUE(UsedIn(*snapshot, refs, "use.cpp"));
    const auto fallback = std::ranges::find_if(refs,
        [&](const idx::Location& l)
        {
            return snapshot->Shards()[l.shard]->Path() == "bare.cpp";
    });
    ASSERT_NE(fallback, refs.end());
    EXPECT_EQ(fallback->resolution, idx::Resolution::Unresolved);
}

TEST(IndexResolution, UsingDirectiveDoesNotResolveEarlierOccurrences)
{
    const auto header = Shard("h.hpp", "namespace lib { struct Tool {}; }\n");
    const std::string source = "#include \"h.hpp\"\nTool before;\nusing namespace lib;\nTool after;\n";
    const auto user = Shard("use.cpp", source);
    const auto snapshot = idx::IndexSnapshot::Build({header, user});
    const auto before = user->OccurrenceAt(static_cast<std::uint32_t>(source.find("Tool")));
    const auto after = user->OccurrenceAt(static_cast<std::uint32_t>(source.rfind("Tool")));

    ASSERT_NE(before, idx::kInvalid);
    ASSERT_NE(after, idx::kInvalid);
    EXPECT_EQ(snapshot->Resolved(1).state[before], idx::Resolution::Unresolved);
    EXPECT_EQ(snapshot->Resolved(1).state[after], idx::Resolution::Resolved);
}

TEST(IndexResolution, UsingDeclarationDoesNotResolveEarlierOccurrences)
{
    const auto header = Shard("h.hpp", "namespace lib { struct Tool {}; }\n");
    const std::string source = "#include \"h.hpp\"\nTool before;\nusing lib::Tool;\nTool after;\n";
    const auto user = Shard("use.cpp", source);
    const auto snapshot = idx::IndexSnapshot::Build({header, user});
    const auto before = user->OccurrenceAt(static_cast<std::uint32_t>(source.find("Tool")));
    const auto after = user->OccurrenceAt(static_cast<std::uint32_t>(source.rfind("Tool")));

    ASSERT_NE(before, idx::kInvalid);
    ASSERT_NE(after, idx::kInvalid);
    EXPECT_EQ(snapshot->Resolved(1).state[before], idx::Resolution::Unresolved);
    EXPECT_EQ(snapshot->Resolved(1).state[after], idx::Resolution::Resolved);
}

TEST(IndexResolution, CallArityChoosesTheMatchingOverload)
{
    const auto header = Shard("h.hpp", "int Add(int);\nint Add(int, int);\n");
    const auto user = Shard("use.cpp", "#include \"h.hpp\"\nint a() { return Add(1, 2); }\n");
    const auto snapshot = idx::IndexSnapshot::Build({header, user});

    const auto one = snapshot->References(HashOf(*header, "Add", 0));
    const auto two = snapshot->References(HashOf(*header, "Add", 1));

    EXPECT_FALSE(UsedIn(*snapshot, one, "use.cpp"));
    EXPECT_TRUE(UsedIn(*snapshot, two, "use.cpp"));
}

TEST(IndexResolution, EquallyGoodOverloadsAreAmbiguous)
{
    const auto header = Shard("h.hpp", "int Add(int);\nint Add(long);\n");
    const auto user = Shard("use.cpp", "#include \"h.hpp\"\nint a(short value) { return Add(value); }\n");
    const auto snapshot = idx::IndexSnapshot::Build({header, user});

    const auto refs = snapshot->References(HashOf(*header, "Add", 0));

    const auto use = std::ranges::find_if(refs,
        [&](const idx::Location& l)
        {
            return snapshot->Shards()[l.shard]->Path() == "use.cpp";
    });
    ASSERT_NE(use, refs.end());
    EXPECT_EQ(use->resolution, idx::Resolution::Ambiguous);
}

TEST(IndexResolution, IncompatibleCallArityDoesNotResolve)
{
    const auto header = Shard("h.hpp", "int Add(int);\nint Add(int, int);\nint Single(int);\n");
    const std::string source = "#include \"h.hpp\"\nint f() { return Add() + Single(); }\n";
    const auto user = Shard("use.cpp", source);
    const auto snapshot = idx::IndexSnapshot::Build({header, user});

    for (const auto name :
        {
            "Add()", "Single()"
    })
    {
        const auto occurrence = user->OccurrenceAt(static_cast<std::uint32_t>(source.find(name)));

        ASSERT_NE(occurrence, idx::kInvalid);
        EXPECT_EQ(snapshot->Resolved(1).state[occurrence], idx::Resolution::Unresolved);
        EXPECT_TRUE(snapshot->Resolved(1).Targets(occurrence).empty());
    }
}

TEST(IndexResolution, UnknownReceiverDoesNotResolveToTheOnlyVisibleMember)
{
    const auto header = Shard("h.hpp", "struct A { int Size(); };\n");
    const std::string source = "#include \"h.hpp\"\nint f(Unknown& object) { return object.Size(); }\n";
    const auto user = Shard("use.cpp", source);
    const auto snapshot = idx::IndexSnapshot::Build({header, user});
    const auto occurrence = user->OccurrenceAt(static_cast<std::uint32_t>(source.find("Size")));

    ASSERT_NE(occurrence, idx::kInvalid);
    EXPECT_EQ(snapshot->Resolved(1).state[occurrence], idx::Resolution::Unresolved);
    const auto references = snapshot->References(HashOf(*header, "Size"));
    const auto use = std::ranges::find_if(references,[](const idx::Location& location)
        {
            return location.shard == 1;
    });
    ASSERT_NE(use, references.end());
    EXPECT_EQ(use->resolution, idx::Resolution::Unresolved);
}

TEST(IndexResolution, ReceiverTypeSeparatesHomonymousMembers)
{
    const auto header = Shard("h.hpp", "struct A { int Size(); };\nstruct B { int Size(); };\n");
    const std::string source = "#include \"h.hpp\"\nint f(A& a, B* b) { return a.Size() + b->Size(); }\n";
    const auto user = Shard("use.cpp", source);
    const auto snapshot = idx::IndexSnapshot::Build({header, user});

    const auto first = user->OccurrenceAt(static_cast<std::uint32_t>(source.find("Size")));
    const auto second = user->OccurrenceAt(static_cast<std::uint32_t>(source.rfind("Size")));

    ASSERT_NE(first, idx::kInvalid);
    ASSERT_NE(second, idx::kInvalid);
    ASSERT_EQ(snapshot->Resolved(1).state[first], idx::Resolution::Resolved);
    ASSERT_EQ(snapshot->Resolved(1).state[second], idx::Resolution::Resolved);
    ASSERT_EQ(snapshot->Resolved(1).Targets(first).size(), 1u);
    ASSERT_EQ(snapshot->Resolved(1).Targets(second).size(), 1u);
    EXPECT_EQ(snapshot->HashOf(snapshot->Resolved(1).Targets(first).front()),
              HashOf(*header, "Size", 0));
    EXPECT_EQ(snapshot->HashOf(snapshot->Resolved(1).Targets(second).front()),
              HashOf(*header, "Size", 1));
}

TEST(IndexResolution, ReceiverTypeIsLookedUpAtItsDeclaration)
{
    const auto header = Shard("h.hpp",
        "struct A { int Size(); };\nnamespace n { struct A { int Size(); }; }\n");
    const std::string source = "#include \"h.hpp\"\nA object;\nnamespace n { int f() { return object.Size(); } }\n";
    const auto user = Shard("use.cpp", source);
    const auto snapshot = idx::IndexSnapshot::Build({header, user});
    const auto occurrence = user->OccurrenceAt(static_cast<std::uint32_t>(source.find("Size")));

    ASSERT_NE(occurrence, idx::kInvalid);
    ASSERT_EQ(snapshot->Resolved(1).state[occurrence], idx::Resolution::Resolved);
    ASSERT_EQ(snapshot->Resolved(1).Targets(occurrence).size(), 1u);
    EXPECT_EQ(snapshot->HashOf(snapshot->Resolved(1).Targets(occurrence).front()),
              HashOf(*header, "Size", 0));
}

TEST(IndexResolution, MemberOfAnOutOfLineBodyResolvesInsideItsClass)
{
    const auto header = Shard("h.hpp",
        "struct A { int Helper(); int Run(); };\nstruct B { int Helper(); };\n");
    const auto source = Shard("a.cpp", "#include \"h.hpp\"\nint A::Run() { return Helper(); }\n");
    const auto snapshot = idx::IndexSnapshot::Build({header, source});

    const auto inA = snapshot->References(HashOf(*header, "Helper", 0));
    const auto inB = snapshot->References(HashOf(*header, "Helper", 1));

    EXPECT_TRUE(UsedIn(*snapshot, inA, "a.cpp"));
    EXPECT_FALSE(UsedIn(*snapshot, inB, "a.cpp"));
}

TEST(IndexResolution, SameNamedBaseInAnotherNamespaceIsNotASupertype)
{
    const auto first = Shard("a.hpp", "namespace a { struct Base {}; }\n");
    const auto second = Shard("b.hpp", "namespace b { struct Base {}; }\n");
    const auto derived = Shard("d.hpp",
        "#include \"a.hpp\"\n#include \"b.hpp\"\nstruct D : b::Base {};\n");
    const auto snapshot = idx::IndexSnapshot::Build({first, second, derived});

    const auto fromA = snapshot->Subtypes(HashOf(*first, "Base"));
    const auto fromB = snapshot->Subtypes(HashOf(*second, "Base"));
    const auto bases = snapshot->Supertypes(HashOf(*derived, "D"));

    EXPECT_TRUE(fromA.empty());
    ASSERT_EQ(fromB.size(), 1u);
    EXPECT_TRUE(fromB[0].exact);
    ASSERT_EQ(bases.size(), 1u);
    EXPECT_EQ(snapshot->Shards()[bases[0].symbol.shard]->Path(), "b.hpp");
}

TEST(IndexResolution, ResolvedIsComputedOnceAndShared)
{
    const auto header = Shard("h.hpp", "int Compute();\n");
    const auto user = Shard("use.cpp", "#include \"h.hpp\"\nint a() { return Compute(); }\n");
    const auto snapshot = idx::IndexSnapshot::Build({header, user});

    const auto& first = snapshot->Resolved(1);
    const auto& again = snapshot->Resolved(1);

    EXPECT_EQ(&first, &again);
}

TEST(IndexResolution, SpecializationsShareTheIdentityOfTheirPrimaryTemplate)
{
    const auto header = Shard("h.hpp",
        "template <class T> struct Box {};\ntemplate <> struct Box<int> {};\n"
        "template <class T> struct Box<T*> {};\n");
    const auto user = Shard("use.cpp", "#include \"h.hpp\"\nBox<int> b;\n");
    const auto snapshot = idx::IndexSnapshot::Build({header, user});

    const auto hash = HashOf(*header, "Box");
    const auto refs = snapshot->References(hash);

    EXPECT_EQ(snapshot->Declarations(hash).size(), snapshot->Definitions(hash).size());
    EXPECT_GE(snapshot->Declarations(hash).size(), 1u);
    EXPECT_TRUE(UsedIn(*snapshot, refs, "use.cpp"));
}

TEST(IndexResolution, ConcurrentResolutionOfOneShardIsSafe)
{
    const auto header = Shard("h.hpp", "int Compute();\n");
    const auto user = Shard("use.cpp", "#include \"h.hpp\"\nint a() { return Compute(); }\n");
    const auto snapshot = idx::IndexSnapshot::Build({header, user});
    std::vector<std::thread> workers;
    std::atomic<int> resolved = 0;

    for (int i = 0; i < 8; ++i)
    {
        workers.emplace_back([&]
            {
                resolved += static_cast<int>(snapshot->References(HashOf(*header, "Compute")).size());
        });
    }

    for (auto& worker : workers)
    {
        worker.join();
    }

    EXPECT_EQ(resolved, 16);
}

TEST(FileShard, QualifierNamesOfOutOfLineDefinitionsAreUses)
{
    const std::string source =
        "namespace n { struct W { W(); int f(); }; }\nn::W::W() {}\nint n::W::f() { return 0; }\n";
    const auto shard = Shard("a.cpp", source);

    EXPECT_EQ(UsesNamed(*shard, "W"), 2u);
    EXPECT_EQ(UsesNamed(*shard, "n"), 2u);
}

TEST(FileShard, UsingPathsAndFriendNamesAreUses)
{
    const auto shard = Shard("a.cpp",
        "namespace a { struct B {}; }\nusing a::B;\nusing namespace a;\nstruct C { friend struct B; };\n");

    EXPECT_EQ(UsesNamed(*shard, "B"), 2u);
    EXPECT_EQ(UsesNamed(*shard, "a"), 2u);
}
