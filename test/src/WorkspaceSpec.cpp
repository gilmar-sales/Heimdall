#include <Heimdall/AnalysisFeatures.hpp>
#include <Heimdall/Workspace.hpp>

#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <future>

namespace
{
    using namespace heimdall;
    auto Text(std::string text)
    {
        return std::make_shared<const std::string>(std::move(text));
    }

    TEST(WorkspaceSpec, OldSnapshotKeepsSourceSyntaxAndSemanticAlive)
    {
        Workspace  workspace;
        const auto id = workspace.Open("snapshot.cpp", Text("int value = 1;\n"), 1);
        ASSERT_TRUE(id);
        auto old    = workspace.Snapshot();
        auto syntax = old.Syntax(*id);
        auto types  = old.Types(*id);
        ASSERT_TRUE(workspace.Update(*id, Text("double other = 2;\n"), 2));
        auto current = workspace.Snapshot();
        EXPECT_LT(old.Revision(), current.Revision());
        EXPECT_EQ(old.Version(*id), 1);
        EXPECT_EQ(current.Version(*id), 2);
        EXPECT_EQ(syntax->Source(), "int value = 1;\n");
        EXPECT_NE(current.Syntax(*id), syntax);
        ASSERT_TRUE(workspace.Close(*id));
        old = {};
        syntax.reset();
        EXPECT_EQ(types->Model().Tree().Source(), "int value = 1;\n");
        EXPECT_FALSE(workspace.Snapshot().Contains(*id));
    }

    TEST(WorkspaceSpec, UnchangedDocumentsShareAllCaches)
    {
        Workspace workspace;
        auto      a = workspace.Open("a.cpp", Text("int a;\n"), 1);
        auto      b = workspace.Open("b.cpp", Text("int b;\n"), 1);
        ASSERT_TRUE(a && b);
        auto before   = workspace.Snapshot();
        auto syntax   = before.Syntax(*b);
        auto semantic = before.Semantic(*b);
        auto types    = before.Types(*b);
        ASSERT_TRUE(workspace.Update(*a, Text("int c;\n"), 2));
        auto after = workspace.Snapshot();
        EXPECT_EQ(after.Syntax(*b), syntax);
        EXPECT_EQ(after.Semantic(*b), semantic);
        EXPECT_EQ(after.Types(*b), types);
        EXPECT_EQ(before.Source(*b), after.Source(*b));
        ASSERT_TRUE(workspace.Update(*b, Text("int b;\n"), 2));
        EXPECT_EQ(workspace.Snapshot().Types(*b), types);
    }

    TEST(WorkspaceSpec, RejectsInvalidDocumentsVersionsAndDuplicatePaths)
    {
        Workspace workspace;
        auto      id = workspace.Open("folder/../version.cpp", Text("int x;"), 5);
        ASSERT_TRUE(id);
        EXPECT_EQ(workspace.Snapshot().Find("version.cpp"), *id);
        EXPECT_FALSE(workspace.Open("version.cpp", Text("")));
        EXPECT_FALSE(workspace.Update(*id, Text("int y;"), 5));
        EXPECT_FALSE(workspace.Update(*id, Text("int y;"), 4));
        EXPECT_FALSE(workspace.Update(InvalidDocument, Text(""), 6));
        EXPECT_FALSE(workspace.Update(*id, nullptr, 6));
        EXPECT_EQ(workspace.Snapshot().Version(*id), 5);
        ASSERT_TRUE(workspace.Close(*id));
        auto reopened = workspace.Open("version.cpp", Text("int z;"));
        ASSERT_TRUE(reopened);
        EXPECT_NE(*id, *reopened);
        EXPECT_FALSE(workspace.Close(*id));
    }

    TEST(WorkspaceSpec, DependencyInvalidationIsTransitiveAndCycleSafe)
    {
        Workspace workspace;
        auto      header      = workspace.Open("header.hpp", Text("struct S {};"), 1);
        auto      user        = workspace.Open("user.cpp", Text("int x;"), 1);
        auto      other       = workspace.Open("other.cpp", Text("int y;"), 1);
        auto      independent = workspace.Open("independent.cpp", Text("int z;"), 1);
        ASSERT_TRUE(header && user && other && independent);
        ASSERT_TRUE(workspace.SetDependencies(*user, std::array { *header }));
        ASSERT_TRUE(workspace.SetDependencies(*other, std::array { *user }));
        ASSERT_TRUE(workspace.SetDependencies(*header, std::array { *other }));
        auto before               = workspace.Snapshot();
        auto user_syntax          = before.Syntax(*user);
        auto user_semantic        = before.Semantic(*user);
        auto other_semantic       = before.Semantic(*other);
        auto independent_semantic = before.Semantic(*independent);
        ASSERT_TRUE(workspace.Update(*header, Text("struct S { int member; };"), 2));
        auto after = workspace.Snapshot();
        EXPECT_EQ(after.Syntax(*user), user_syntax);
        EXPECT_NE(after.Semantic(*user), user_semantic);
        EXPECT_NE(after.Semantic(*other), other_semantic);
        EXPECT_EQ(after.Semantic(*independent), independent_semantic);
        EXPECT_EQ(before.Dependencies(*user)[0], *header);
        EXPECT_FALSE(workspace.SetDependencies(*user, std::array { InvalidDocument }));
        ASSERT_TRUE(workspace.Close(*header));
        EXPECT_TRUE(workspace.Snapshot().Dependencies(*user).empty());
        EXPECT_FALSE(before.Dependencies(*user).empty());
    }

    TEST(WorkspaceSpec, ConcurrentReadersBuildEachCacheOnlyOnce)
    {
        Workspace workspace;
        auto id = workspace.Open("concurrent.cpp", Text("int add(int a, int b) { return a + b; }"));
        ASSERT_TRUE(id);
        auto                                                       snapshot = workspace.Snapshot();
        std::vector<std::future<std::shared_ptr<const TypeModel>>> workers;
        for (int i = 0; i < 8; ++i)
        {
            workers.push_back(std::async(std::launch::async, [snapshot, id = *id] {
                return snapshot.Types(id);
            }));
        }

        auto first = workers.front().get();
        for (std::size_t i = 1; i < workers.size(); ++i)
        {
            EXPECT_EQ(workers[i].get(), first);
        }

        auto metrics = snapshot.Metrics();
        EXPECT_EQ(metrics.parse_count, 1);
        EXPECT_EQ(metrics.bind_count, 1);
        EXPECT_EQ(metrics.type_count, 1);
        EXPECT_GT(metrics.cache_hits, 0);
        EXPECT_GT(metrics.semantic_allocations, 0);
        EXPECT_GT(metrics.type_allocations, 0);
        auto memory = snapshot.Memory();
        EXPECT_GT(memory.source_bytes, 0);
        EXPECT_GT(memory.syntax_bytes, 0);
        EXPECT_GT(memory.semantic_arena_bytes, 0);
        EXPECT_GT(memory.type_arena_bytes, 0);
    }

    TEST(WorkspaceSpec, SourceStorageIsReleasedWhenLastReaderFinishes)
    {
        Workspace                        workspace;
        auto                             text = Text("int retained;");
        std::weak_ptr<const std::string> weak = text;
        auto                             id   = workspace.Open("retained.cpp", text);
        ASSERT_TRUE(id);
        text.reset();
        auto snapshot = workspace.Snapshot();
        auto semantic = snapshot.Semantic(*id);
        ASSERT_TRUE(workspace.Close(*id));
        snapshot = {};
        EXPECT_FALSE(weak.expired());
        semantic.reset();
        EXPECT_TRUE(weak.expired());
    }

    TEST(WorkspaceSpec, ExistingParseIsAdoptedWithoutReparsingOrSourceCopy)
    {
        Workspace workspace;
        auto      text = Text("int existing;");
        auto      id   = workspace.Open("existing.cpp", text);
        ASSERT_TRUE(id);
        auto tree     = std::make_shared<const ParseTree>(ParseTree::ParseSnapshot(text, {}));
        auto snapshot = workspace.Snapshot();
        auto derived  = snapshot.WithSyntax(*id, tree, {});
        ASSERT_TRUE(derived);
        EXPECT_EQ(derived->Syntax(*id), tree);
        EXPECT_EQ(derived->Source(*id), text);
        EXPECT_EQ(derived->Metrics().parse_count, 0);
        EXPECT_FALSE(snapshot.WithSyntax(
            *id,
            std::make_shared<const ParseTree>(ParseTree::ParseSnapshot(Text("int wrong;"), {})),
            {}));
    }

    TEST(WorkspaceSpec, OptionsInvalidateCachesWithoutChangingPinnedViews)
    {
        Workspace workspace;
        auto      id = workspace.Open("dialect.cpp", Text("int x;"));
        ASSERT_TRUE(id);
        auto          before = workspace.Snapshot();
        auto          syntax = before.Syntax(*id);
        ParserOptions options;
        options.standard = CppStandard::Cpp26;
        ASSERT_TRUE(workspace.SetOptions(*id, options));
        EXPECT_EQ(before.Syntax(*id), syntax);
        EXPECT_EQ(before.Syntax(*id)->Standard(), CppStandard::Cpp20);
        EXPECT_EQ(workspace.Snapshot().Syntax(*id)->Standard(), CppStandard::Cpp26);
    }

    TEST(WorkspaceSpec, SnapshotFeaturesMatchExistingTreeFeatures)
    {
        Workspace workspace;
        auto      text = Text("int value; int f() { return value; }\n");
        auto      id   = workspace.Open("features.cpp", text);
        ASSERT_TRUE(id);
        AnalysisContext context(workspace.Snapshot(), *id);
        const auto      offset   = text->rfind("value");
        auto            targets  = AnalysisFeatures::Definition(context, offset);
        auto            expected = Navigation::Definition(context.Syntax(), offset);
        ASSERT_EQ(targets.size(), expected.size());
        ASSERT_FALSE(targets.empty());
        EXPECT_EQ(targets[0].offset, expected[0].offset);
        const auto symbol = context.ResolveSymbol(offset);
        EXPECT_NE(symbol, kNone);
        EXPECT_EQ(context.Semantic().Names().Text(context.Symbols().name[symbol]), "value");
        EXPECT_FALSE(context.References(symbol).empty());
        auto completed = AnalysisFeatures::Complete(context, offset + 3);
        auto old       = CompletionEngine::Complete(context.Syntax(), {}, offset + 3);
        EXPECT_EQ(completed.size(), old.size());
        EXPECT_EQ(context.NodeRange(InvalidNode).length, 0);
        EXPECT_EQ(context.ResolveSymbol(text->size() + 10), kNone);
        EXPECT_TRUE(context.References(kNone).empty());
        AnalysisContext invalid({}, InvalidDocument);
        EXPECT_TRUE(AnalysisFeatures::Complete(invalid, 0).empty());
        EXPECT_TRUE(AnalysisFeatures::Diagnostics(invalid, RuleEngine()).empty());
    }

    TEST(WorkspaceSpec, WorkspaceUpdatesReuseUnchangedTopLevelItems)
    {
        Workspace workspace;
        auto      id = workspace.Open(
            "incremental.cpp", Text("int a() { return 1; }\nint b() { return 2; }\n"), 1);
        ASSERT_TRUE(id);
        auto before = workspace.Snapshot().Syntax(*id);
        ASSERT_TRUE(
            workspace.Update(*id, Text("int a() { return 3; }\nint b() { return 2; }\n"), 2));
        auto current = workspace.Snapshot();
        EXPECT_GT(current.Memory().retained_base_bytes, 0);
        auto after = current.Syntax(*id);
        EXPECT_EQ(current.Memory().retained_base_bytes, 0);
        EXPECT_GT(after->ReusedItems(), 0);
        EXPECT_EQ(before->Source(), "int a() { return 1; }\nint b() { return 2; }\n");
    }

    TEST(WorkspaceSpec, RegisteredQuotedIncludesBuildAndUpdateDependencyGraph)
    {
        Workspace workspace;
        auto      source =
            workspace.Open("project/source.cpp",
                           Text("#include /* \"misleading.hpp\" */ \"header.hpp\"\nint x;"), 1);
        ASSERT_TRUE(source);
        EXPECT_TRUE(workspace.Snapshot().Dependencies(*source).empty());
        auto header = workspace.Open("project/header.hpp", Text("struct S {};"), 1);
        ASSERT_TRUE(header);
        auto before = workspace.Snapshot();
        ASSERT_EQ(before.Dependencies(*source).size(), 1);
        EXPECT_EQ(before.Dependencies(*source)[0], *header);
        auto semantic = before.Semantic(*source);
        ASSERT_TRUE(workspace.Update(*header, Text("struct S { int field; };"), 2));
        EXPECT_NE(workspace.Snapshot().Semantic(*source), semantic);
        ASSERT_TRUE(workspace.Update(*source, Text("int x;"), 2));
        EXPECT_TRUE(workspace.Snapshot().Dependencies(*source).empty());
        EXPECT_FALSE(before.Dependencies(*source).empty());
    }

    TEST(WorkspaceSpec, HeaderSummaryReusesBoundModelAndFingerprintsRelevantChanges)
    {
        Workspace workspace;
        auto      id = workspace.Open("summary.hpp", Text("#define SIZE 4\nstruct S {};"), 1);
        ASSERT_TRUE(id);
        auto before  = workspace.Snapshot();
        auto summary = before.Summary(*id);
        ASSERT_TRUE(summary);
        EXPECT_EQ(summary->ExportCount(), 1);
        EXPECT_EQ(summary->ExportName(0), "S");
        EXPECT_EQ(before.Metrics().parse_count, 1);
        EXPECT_EQ(before.Metrics().bind_count, 1);
        EXPECT_EQ(before.Summary(*id), summary);
        ASSERT_TRUE(workspace.Update(*id, Text("#define SIZE 8\nstruct S {};"), 2));
        EXPECT_NE(workspace.Snapshot().Summary(*id)->SemanticFingerprint(),
                  summary->SemanticFingerprint());
    }

    TEST(WorkspaceSpec, CompilationDatabaseIsPinnedAndDrivesDialectMacrosAndIncludePaths)
    {
        const auto root = std::filesystem::path(HEIMDALL_SOURCE_DIR);
        const auto path =
            std::filesystem::temp_directory_path() / "heimdall_workspace_database_test.json";
        {
            std::ofstream file(path);
            file << "[{\"directory\":\"" << root.generic_string()
                 << "\",\"file\":\"project/"
                    "source.cpp\",\"arguments\":[\"g++\",\"-std=c++26\",\"-DVALUE=42\",\"-Iproject/"
                    "include\"]}]";
        }
        auto loaded = CompileDatabase::Load(path);
        std::filesystem::remove(path);
        ASSERT_TRUE(loaded);
        Workspace workspace;
        auto      database = std::make_shared<const CompileDatabase>(std::move(*loaded));
        workspace.SetCompilationDatabase(database);
        auto source = workspace.Open(root / "project/source.cpp",
                                     Text("#include <header.hpp>\nint x = VALUE;"));
        auto header = workspace.Open(root / "project/include/header.hpp", Text("struct S {};"));
        ASSERT_TRUE(source && header);
        auto pinned = workspace.Snapshot();
        ASSERT_NE(pinned.Command(*source), nullptr);
        EXPECT_EQ(pinned.Command(*source), database->Find(root / "project/source.cpp"));
        EXPECT_EQ(pinned.Options(*source).standard, CppStandard::Cpp26);
        EXPECT_EQ(pinned.Options(*source).Macros().at("VALUE"), "42");
        ASSERT_EQ(pinned.Dependencies(*source).size(), 1);
        EXPECT_EQ(pinned.Dependencies(*source)[0], *header);
        workspace.SetCompilationDatabase(nullptr);
        EXPECT_EQ(workspace.Snapshot().Command(*source), nullptr);
        EXPECT_EQ(workspace.Snapshot().Options(*source).standard, CppStandard::Cpp20);
        EXPECT_TRUE(workspace.Snapshot().Options(*source).Macros().empty());
        EXPECT_NE(pinned.Command(*source), nullptr);
        EXPECT_EQ(pinned.Options(*source).standard, CppStandard::Cpp26);
    }

    TEST(WorkspaceSpec, ProjectIndexIsBuiltLazilyAndOldSnapshotKeepsOldExports)
    {
        Workspace workspace;
        auto      id = workspace.Open("exports.hpp", Text("struct OldType {};"), 1);
        ASSERT_TRUE(id);
        auto before = workspace.Snapshot();
        EXPECT_EQ(before.Metrics().project_index_count, 0);
        auto index = before.Project();
        ASSERT_TRUE(index);
        EXPECT_EQ(index->ExportsNamed("OldType").size(), 1);
        EXPECT_EQ(before.Project(), index);
        EXPECT_EQ(before.Metrics().project_index_count, 1);
        EXPECT_EQ(before.Metrics().parse_count, 1);
        EXPECT_EQ(before.Metrics().bind_count, 1);
        ASSERT_TRUE(workspace.Update(*id, Text("struct NewType {};"), 2));
        auto current = workspace.Snapshot().Project();
        EXPECT_TRUE(current->ExportsNamed("OldType").empty());
        EXPECT_EQ(current->ExportsNamed("NewType").size(), 1);
        EXPECT_EQ(index->ExportsNamed("OldType").size(), 1);
    }

    TEST(WorkspaceSpec, EditHullUpdateReusesSyntaxAndPreservesOlderSnapshotsAcrossPages)
    {
        Workspace               workspace;
        std::vector<DocumentId> ids;
        for (int i = 0; i < 130; ++i)
        {
            auto id = workspace.Open("page" + std::to_string(i) + ".cpp",
                                     Text("int First;\nint Second;\nint Third;\n"), 1);
            ASSERT_TRUE(id);
            ids.push_back(*id);
        }
        const auto                before  = workspace.Snapshot();
        const auto                oldTree = before.Syntax(ids[64]);
        const auto                source  = Text("int First;\nint Changed;\nint Third;\n");
        constexpr Lexer::TextEdit kEdit { 15, 6, 7 };

        ASSERT_TRUE(workspace.Update(ids[64], source, 2, kEdit));
        const auto after = workspace.Snapshot();
        const auto tree  = after.Syntax(ids[64]);

        EXPECT_EQ(before.Source(ids[64]), oldTree->SharedSource());
        EXPECT_EQ(before.Version(ids[64]), 1);
        EXPECT_EQ(after.Version(ids[64]), 2);
        EXPECT_GT(tree->ReusedItems(), 0u);
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            if (i != 64)
            {
                EXPECT_EQ(after.Source(ids[i]), before.Source(ids[i]));
            }
        }
    }

    TEST(WorkspaceSpec, InvalidEditHullDoesNotPublishANewRevision)
    {
        Workspace workspace;
        auto      id = workspace.Open("invalid_edit.cpp", Text("int Value;"), 1);
        ASSERT_TRUE(id);
        const auto before = workspace.Snapshot();

        const auto updated =
            workspace.Update(*id, Text("int Other;"), 2, Lexer::TextEdit { 100, 1, 1 });

        EXPECT_FALSE(updated);
        EXPECT_EQ(workspace.Snapshot().Revision(), before.Revision());
        EXPECT_EQ(workspace.Snapshot().Version(*id), 1);
    }

    TEST(WorkspaceSpec, ConcurrentColdQueriesPublishOnlyOneModel)
    {
        Workspace workspace;
        auto      id = workspace.Open("concurrent.cpp", Text("struct Value { int Field; };"), 1);
        ASSERT_TRUE(id);
        const auto                                                 snapshot = workspace.Snapshot();
        std::vector<std::future<std::shared_ptr<const TypeModel>>> queries;

        for (int i = 0; i < 12; ++i)
        {
            queries.push_back(std::async(std::launch::async, [&] { return snapshot.Types(*id); }));
        }
        const auto expected = queries.front().get();

        for (std::size_t i = 1; i < queries.size(); ++i)
        {
            EXPECT_EQ(queries[i].get(), expected);
        }
        const auto metrics = snapshot.Metrics();
        EXPECT_EQ(metrics.parse_count, 1u);
        EXPECT_EQ(metrics.bind_count, 1u);
        EXPECT_EQ(metrics.type_count, 1u);
    }
} // namespace

TEST(WorkspaceSpec, PersistentEditsReusePreprocessingAndPreservePinnedVersions)
{
    heimdall::Workspace workspace;
    const std::string   text = "#define VALUE 1\nint first = VALUE;\nint second = 2;\n";
    auto                source =
        std::make_shared<const heimdall::SourceSnapshot>(*heimdall::SourceSnapshot::Create(text));
    auto tokens = std::make_shared<const heimdall::TokenSnapshot>(
        heimdall::TokenSnapshot::Create(heimdall::Lexer(text).Lex()));
    const auto id = workspace.OpenBuffer("persistent.cpp", source, tokens, 1);
    ASSERT_TRUE(id);
    const auto                      before  = workspace.Snapshot();
    const auto                      oldTree = before.Syntax(*id);
    const auto                      offset  = text.find("2");
    const heimdall::Lexer::TextEdit edit { offset, 1, 2 };
    source = std::make_shared<const heimdall::SourceSnapshot>(*source->Edit(offset, 1, "23"));
    tokens = std::make_shared<const heimdall::TokenSnapshot>(tokens->Relex(*source, edit));

    ASSERT_TRUE(workspace.UpdateBuffer(*id, source, tokens, 2, edit));
    const auto tree = workspace.Snapshot().Syntax(*id);
    const auto full = heimdall::ParseTree::Parse(*source->Materialize());

    EXPECT_TRUE(tree->Metrics().reusedPreprocessing);
    EXPECT_EQ(oldTree->Source(), text);
    EXPECT_EQ(tree->Directives().size(), full.Directives().size());
    ASSERT_EQ(tree->NodesSoA().size(), full.NodesSoA().size());
    for (std::size_t i = 0; i < full.NodesSoA().size(); ++i)
    {
        EXPECT_EQ(tree->NodesSoA().Kind(i), full.NodesSoA().Kind(i));
        EXPECT_EQ(tree->NodesSoA().FirstToken(i), full.NodesSoA().FirstToken(i));
        EXPECT_EQ(tree->NodesSoA().TokenCount(i), full.NodesSoA().TokenCount(i));
        EXPECT_EQ(tree->NodesSoA().Parent(i), full.NodesSoA().Parent(i));
    }
    EXPECT_GT(workspace.Snapshot().Memory().Total(), source->Size());
    EXPECT_EQ(before.Version(*id), 1);
}

TEST(WorkspaceSpec, PersistentMacroEditsReprocessAndUpdateTheIncludeGraph)
{
    heimdall::Workspace workspace;
    const auto header = workspace.Open("old.hpp", std::make_shared<const std::string>("int Old;"));
    ASSERT_TRUE(header);
    const std::string text = "#include \"old.hpp\"\nint Value;\n";
    auto              source =
        std::make_shared<const heimdall::SourceSnapshot>(*heimdall::SourceSnapshot::Create(text));
    auto tokens = std::make_shared<const heimdall::TokenSnapshot>(
        heimdall::TokenSnapshot::Create(heimdall::Lexer(text).Lex()));
    const auto id = workspace.OpenBuffer("main.cpp", source, tokens, 1);
    ASSERT_TRUE(id);
    EXPECT_EQ(workspace.Snapshot().Dependencies(*id).size(), 1u);
    (void)workspace.Snapshot().Syntax(*id);
    const auto                      offset = text.find("old");
    const heimdall::Lexer::TextEdit edit { offset, 3, 3 };
    source = std::make_shared<const heimdall::SourceSnapshot>(*source->Edit(offset, 3, "new"));
    tokens = std::make_shared<const heimdall::TokenSnapshot>(tokens->Relex(*source, edit));

    ASSERT_TRUE(workspace.UpdateBuffer(*id, source, tokens, 2, edit));

    EXPECT_FALSE(workspace.Snapshot().Syntax(*id)->Metrics().reusedPreprocessing);
    EXPECT_TRUE(workspace.Snapshot().Dependencies(*id).empty());
}

TEST(WorkspaceSpec, CacheBudgetEvictionKeepsBorrowedModelsAndRebuildsQueries)
{
    heimdall::Workspace workspace;
    const auto          id = workspace.Open(
        "budget.cpp", std::make_shared<const std::string>("struct Value { int field; };"), 1);
    ASSERT_TRUE(id);
    const auto snapshot = workspace.Snapshot();
    const auto tree     = snapshot.Syntax(*id);
    const auto model    = snapshot.Semantic(*id);
    const auto types    = snapshot.Types(*id);

    workspace.TrimCaches(0);

    EXPECT_EQ(tree->Source(), "struct Value { int field; };");
    EXPECT_GT(model->ArenaReservedBytes(), 0u);
    EXPECT_GT(types->ArenaReservedBytes(), 0u);
    EXPECT_NE(snapshot.Syntax(*id), tree);
    EXPECT_NE(snapshot.Semantic(*id), model);
    EXPECT_EQ(snapshot.Version(*id), 1);
}
