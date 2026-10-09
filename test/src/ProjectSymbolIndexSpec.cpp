#include <Heimdall/Workspace.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>

namespace
{
    using namespace heimdall;

    DocumentId Open(
        Workspace& workspace,
        std::string_view path,
        std::string_view source,
        ParserOptions options = {})
    {
        auto result = workspace.Open(std::filesystem::path(path),
            std::make_shared<const std::string>(source), 1, options);
        EXPECT_TRUE(result);
        return result ? *result : InvalidDocument;
    }

    void ExpectComplete(const ProjectSymbolIndex& index)
    {
        for (const auto& issue : index.Issues())
        {
            ADD_FAILURE() << "coverage issue " << static_cast<int>(issue.kind) << " in " << issue.path;
        }

        for (const auto& occurrence : index.Occurrences())
        {
            EXPECT_EQ(occurrence.resolution,
                OccurrenceResolution::Resolved) << occurrence.name << " in document " << occurrence.document;
        }

        EXPECT_TRUE(index.Complete());
    }

    struct DiskProject
    {
        std::filesystem::path root = std::filesystem::temp_directory_path() /("heimdall-symbol-project-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        bool owned = std::filesystem::create_directory(root);

        DiskProject()
        {
            EXPECT_TRUE(owned);
            if (!owned)
            {
                return;
            }

            std::filesystem::create_directory(root / "src");
            std::filesystem::create_directory(root / "include");
            Write("include/detail.hpp", "int diskName(int input);");
            Write("include/api.hpp", "#include \"detail.hpp\"\n");
            Write("src/impl.cpp", "#include <api.hpp>\nint fresh(int value) { return value; }");
            Write("src/use.cpp", "#include <api.hpp>\nint run() { return fresh(1); }");
            std::string database = "[";
            for (const auto name :
                {
                    "impl", "use"
            })
            {
                if (database.size() > 1)
                {
                    database += ',';
                }

                database += "{\"directory\":\"" + root.generic_string() + "\",\"file\":\"" +
                    (root / "src" /(std::string(name) + ".cpp")).generic_string() +
                    "\",\"arguments\":[\"c++\",\"-std=c++20\",\"-I\",\"include\",\"-c\",\"src/" + name + ".cpp\"]}";
            }

            database += "]";
            Write("compile_commands.json", database);
        }

        void Write(std::string_view name, std::string_view text)
        {
            std::ofstream file(root / std::filesystem::path(name), std::ios::binary);
            file << text;
            EXPECT_TRUE(file.good());
        }

        ~DiskProject()
        {
            if (!owned)
            {
                return;
            }

            std::error_code error;
            for (const auto name :
                {
                    "src/impl.cpp", "src/use.cpp", "include/detail.hpp", "include/api.hpp",
                    "compile_commands.json", "src", "include"
            })
            {
                std::filesystem::remove(root / name, error);
            }

            std::filesystem::remove(root, error);
        }
    };

    TEST(ProjectSymbolIndexSpec, UnifiesHeaderDeclarationsDefinitionsAndQualifiedCalls)
    {
        Workspace workspace;
        Open(workspace, "symbol-project/api.hpp",
            "namespace api { int twice(int input); extern int count; }");
        Open(workspace, "symbol-project/api.cpp", "#include \"api.hpp\"\nnamespace api { "
            "int twice(int argument) { return argument + argument; } int count = 1; }");
        const std::string source = "#include \"api.hpp\"\nint run() { return api::twice(2); }";
        const auto use = Open(workspace, "symbol-project/use.cpp", source);
        auto built = workspace.Snapshot().SymbolIndex();
        ASSERT_TRUE(built);
        const auto& index = * *built;
        ASSERT_EQ(index.Named("api::twice").size(), 1u);
        const auto id = index.Named("api::twice").front();
        EXPECT_TRUE(index.Entities()[id].supported);
        EXPECT_EQ(index.Entities()[id].occurrences.size(), 3u);
        ASSERT_EQ(index.Named("api::twice::input").size(), 1u);
        EXPECT_EQ(index.Entities()[index.Named("api::twice::input").front()].type, "int");
        ASSERT_EQ(index.Named("api::count").size(), 1u);
        const auto* occurrence = index.At(use, source.find("twice"));
        ASSERT_NE(occurrence, nullptr);
        EXPECT_EQ(occurrence->entity, id);
        EXPECT_EQ(occurrence->resolution, OccurrenceResolution::Resolved);
        EXPECT_EQ(occurrence->role, OccurrenceRole::Call);
        ExpectComplete(index);
        EXPECT_TRUE(index.HasCompleteCoverageFor(id));
    }

    TEST(ProjectSymbolIndexSpec, DoesNotResolveNamesFromUnincludedFiles)
    {
        Workspace workspace;
        Open(workspace, "visibility/hidden.hpp", "int hidden(int input);");
        const std::string source = "int run() { return hidden(1); }";
        const auto use = Open(workspace, "visibility/use.cpp", source);
        const auto index = ProjectSymbolIndex::Build(workspace.Snapshot());
        ASSERT_TRUE(index);
        const auto* occurrence = index->At(use, source.find("hidden"));
        ASSERT_NE(occurrence, nullptr);
        EXPECT_EQ(occurrence->resolution, OccurrenceResolution::Unresolved);
        EXPECT_FALSE(index->Complete());
    }

    TEST(ProjectSymbolIndexSpec, KeepsOverloadIdentitiesButBlocksUnprovenCalls)
    {
        Workspace workspace;
        Open(workspace, "overloads/api.hpp", "int calculate(int input); int calculate(double input);");
        const std::string source = "#include \"api.hpp\"\nint run() { return calculate(1); }";
        const auto use = Open(workspace, "overloads/use.cpp", source);
        const auto index = ProjectSymbolIndex::Build(workspace.Snapshot());
        ASSERT_TRUE(index);
        EXPECT_EQ(index->Named("calculate").size(), 2u);
        const auto* occurrence = index->At(use, source.find("calculate"));
        ASSERT_NE(occurrence, nullptr);
        EXPECT_EQ(occurrence->resolution, OccurrenceResolution::Ambiguous);
        EXPECT_EQ(occurrence->candidates.size(), 2u);
        EXPECT_EQ(occurrence->entity, InvalidEntity);
        EXPECT_FALSE(index->HasCompleteCoverageFor(index->Named("calculate").front()));
    }

    TEST(ProjectSymbolIndexSpec, SeparatesInternalAndShadowedLocalEntities)
    {
        Workspace workspace;
        const std::string source = "static int helper(int value) { { int value = 2; return value; } return value; }";
        const auto a = Open(workspace, "internal/a.cpp", source);
        const auto b = Open(workspace, "internal/b.cpp", source);
        const auto index = ProjectSymbolIndex::Build(workspace.Snapshot());
        ASSERT_TRUE(index);
        ASSERT_EQ(index->Named("helper").size(), 2u);
        EXPECT_NE(index->At(a, source.find("helper")) -> entity,
            index->At(b, source.find("helper"))->entity);
        const auto parameter = index->At(a, source.find("value")) -> entity;
        const auto shadow = index->At(a, source.find("value =")) -> entity;
        EXPECT_NE(parameter, shadow);
        EXPECT_EQ(index->At(a, source.rfind("value")) -> entity, parameter);
    }

    TEST(ProjectSymbolIndexSpec, ObservesDirtyTransitiveHeadersAndPinsOldViews)
    {
        Workspace workspace;
        const auto header = Open(workspace, "dirty/detail.hpp", "int fresh(int input);");
        Open(workspace, "dirty/api.hpp", "#include \"detail.hpp\"\n");
        const std::string source = "#include \"api.hpp\"\nint run() { return fresh(1); }";
        const auto use = Open(workspace, "dirty/use.cpp", source);
        const auto old = workspace.Snapshot();
        const auto before = old.SymbolIndex();
        ASSERT_TRUE(before);
        EXPECT_EQ((*before)->At(use, source.find("fresh")) -> resolution, OccurrenceResolution::Resolved);
        ASSERT_TRUE(workspace.Update(header, std::make_shared<const std::string>("int changed(int input);"),
            2));
        const auto after = workspace.Snapshot().SymbolIndex();
        ASSERT_TRUE(after);
        EXPECT_NE(before->get(), after->get());
        EXPECT_EQ((*after)->At(use, source.find("fresh")) -> resolution, OccurrenceResolution::Unresolved);
        EXPECT_EQ((*before)->Named("fresh").size(), 1u);
        EXPECT_EQ((*before)->Units().front().version, 1);
        ExpectComplete(* *before);
    }

    TEST(ProjectSymbolIndexSpec, RecordsCoverageGapsAndDependentOrUnsupportedUses)
    {
        Workspace workspace;
        const std::string source = "#include \"missing.hpp\"\ntemplate<class T> int run(T value) { return value; }";
        const auto use = Open(workspace, "coverage/use.cpp", source);
        const auto index = ProjectSymbolIndex::Build(workspace.Snapshot());
        ASSERT_TRUE(index);
        EXPECT_FALSE(index->Complete());
        EXPECT_TRUE(std::ranges::any_of(index->Issues(),[](const auto& issue)
            {
                return issue.kind == SymbolIndexIssue::MissingInclude;
        }));
        const auto* occurrence = index->At(use, source.rfind("value"));
        ASSERT_NE(occurrence, nullptr);
        EXPECT_EQ(occurrence->resolution, OccurrenceResolution::Dependent);
        for (EntityId id = 0; id < index->Entities().size(); ++id)
        {
            EXPECT_FALSE(index->HasCompleteCoverageFor(id));
        }
    }

    TEST(ProjectSymbolIndexSpec, DoesNotMergeConflictingTypesOrConfigurationVariants)
    {
        Workspace workspace;
        Open(workspace, "conflict/a.cpp", "int calculate(int input);");
        Open(workspace, "conflict/b.cpp", "double calculate(int input);");
        ParserOptions options;
        options.standard = CppStandard::Cpp23;
        Open(workspace, "conflict/c.cpp", "int calculate(int input);", options);
        const auto index = ProjectSymbolIndex::Build(workspace.Snapshot());
        ASSERT_TRUE(index);
        EXPECT_EQ(index->Named("calculate").size(), 2u);
        EXPECT_TRUE(std::ranges::any_of(index->Issues(),[](const auto& issue)
            {
                return issue.kind == SymbolIndexIssue::ConflictingDeclarations;
        }));
        EXPECT_TRUE(std::ranges::any_of(index->Issues(),[](const auto& issue)
            {
                return issue.kind == SymbolIndexIssue::ConfigurationVariants;
        }));
        EXPECT_FALSE(index->Complete());
    }

    TEST(ProjectSymbolIndexSpec, RejectsCancellationAndLimitsWithoutPartialResults)
    {
        Workspace workspace;
        Open(workspace, "limits/a.cpp", "int run(int input) { return input; }");
        std::stop_source stop;
        stop.request_stop();
        EXPECT_EQ(ProjectSymbolIndex::Build(workspace.Snapshot(), {}, stop.get_token()).error(),
            SymbolIndexError::Cancelled);
        EXPECT_EQ(workspace.Snapshot().SymbolIndex(stop.get_token()).error(), SymbolIndexError::Cancelled);
        for (auto limits :
            {
                SymbolIndexLimits
                {
                    0, 10, 10
                }, SymbolIndexLimits{10, 0, 10}, SymbolIndexLimits{10, 10, 0}
        })
        {
            EXPECT_EQ(ProjectSymbolIndex::Build(workspace.Snapshot(), limits).error(),
                SymbolIndexError::LimitReached);
        }
    }

    TEST(ProjectSymbolIndexSpec, ConcurrentReadersShareOneLazyIndex)
    {
        Workspace workspace;
        const auto document = Open(workspace, "cache/a.cpp", "int run(int input) { return input; }");
        const auto snapshot = workspace.Snapshot();
        const auto memory_before = snapshot.Memory().Total();
        std::vector<std::shared_ptr<const ProjectSymbolIndex>> results(8);
        std::vector<std::jthread> threads;
        for (std::size_t i = 0; i < results.size(); ++i)
        {
            threads.emplace_back([&, i]
                {
                    auto result = snapshot.SymbolIndex(); if (result)
                    {
                        results[i] = *result;
                }
            });
        }

        threads.clear();
        ASSERT_NE(results.front(), nullptr);
        for (const auto& result : results)
        {
            EXPECT_EQ(result, results.front());
        }

        EXPECT_EQ(snapshot.Metrics().symbol_index_count, 1u);
        EXPECT_GT(snapshot.Memory().Total(), memory_before);
        ASSERT_TRUE(workspace.Update(document, snapshot.Source(document), 2));
        const auto updated = workspace.Snapshot().SymbolIndex();
        ASSERT_TRUE(updated);
        EXPECT_NE(*updated, results.front());
        EXPECT_EQ((*updated)->Units().front().version, 2);
        EXPECT_EQ(results.front()->Units().front().version, 1);
    }

    TEST(ProjectSymbolIndexSpec, ImportsClosedTranslationUnitsWithoutOverwritingDirtyHeaders)
    {
        DiskProject project;
        const auto database = CompileDatabase::Load(project.root / "compile_commands.json");
        ASSERT_TRUE(database);
        Workspace workspace;
        workspace.SetCompilationDatabase(std::make_shared<const CompileDatabase>(*database));
        const auto header = workspace.Open(project.root / "include/detail.hpp",
            std::make_shared<const std::string>("int fresh(int input);"), 7);
        ASSERT_TRUE(header);
        const auto before = workspace.Snapshot();
        const auto partial = before.SymbolIndex();
        ASSERT_TRUE(partial);
        EXPECT_TRUE(std::ranges::any_of((*partial)->Issues(),[](const auto& issue)
            {
                return issue.kind == SymbolIndexIssue::MissingTranslationUnit;
        }));
        const auto loaded = workspace.LoadProjectSources();
        ASSERT_TRUE(loaded);
        EXPECT_EQ(loaded->added.size(), 3u);
        EXPECT_TRUE(loaded->unavailable.empty());
        const auto snapshot = workspace.Snapshot();
        EXPECT_EQ(snapshot.Version(*header), 7);
        EXPECT_EQ(*snapshot.Source(*header), "int fresh(int input);");
        EXPECT_EQ(before.Documents().size(), 1u);
        const auto index = snapshot.SymbolIndex();
        ASSERT_TRUE(index);
        ASSERT_EQ((*index)->Named("fresh").size(), 1u);
        EXPECT_EQ((*index)->Entities()[(*index)->Named("fresh").front()].occurrences.size(), 3u);
        ExpectComplete(* *index);
        EXPECT_TRUE((*index)->Named("diskName").empty());
        const auto again = workspace.LoadProjectSources();
        ASSERT_TRUE(again);
        EXPECT_TRUE(again->added.empty());
        EXPECT_EQ(workspace.Snapshot().Revision(), snapshot.Revision());
    }

    TEST(ProjectSymbolIndexSpec, ProjectLoadingCancellationAndLimitsAreTransactional)
    {
        DiskProject project;
        const auto database = CompileDatabase::Load(project.root / "compile_commands.json");
        ASSERT_TRUE(database);
        Workspace workspace;
        workspace.SetCompilationDatabase(std::make_shared<const CompileDatabase>(*database));
        const auto revision = workspace.Snapshot().Revision();
        EXPECT_EQ(workspace.LoadProjectSources({1, 1024}).error(), ProjectLoadError::LimitReached);
        EXPECT_TRUE(workspace.Snapshot().Documents().empty());
        EXPECT_EQ(workspace.Snapshot().Revision(), revision);
        std::stop_source stop;
        stop.request_stop();
        EXPECT_EQ(workspace.LoadProjectSources({}, stop.get_token()).error(), ProjectLoadError::Cancelled);
        EXPECT_TRUE(workspace.Snapshot().Documents().empty());
        std::error_code error;
        ASSERT_TRUE(std::filesystem::remove(project.root / "src/use.cpp", error));
        const auto loaded = workspace.LoadProjectSources();
        ASSERT_TRUE(loaded);
        EXPECT_FALSE(loaded->unavailable.empty());
        const auto index = workspace.Snapshot().SymbolIndex();
        ASSERT_TRUE(index);
        EXPECT_FALSE((*index)->Complete());
    }

    TEST(ProjectSymbolIndexSpec, CanonicalizesBuiltinSynonymsAndIgnoresParameterNamesAndDefaults)
    {
        Workspace workspace;
        Open(workspace, "signature/a.cpp", "int run(signed int input = 1);");
        Open(workspace, "signature/b.cpp", "int run(int other) { return other; }");
        const auto index = ProjectSymbolIndex::Build(workspace.Snapshot());
        ASSERT_TRUE(index);
        ASSERT_EQ(index->Named("run").size(), 1u);
        EXPECT_EQ(index->Entities()[index->Named("run").front()].occurrences.size(), 2u);
    }

    TEST(ProjectSymbolIndexSpec, RespectsIncludeOrderAndQualifiedNamespaceShadowing)
    {
        Workspace workspace;
        Open(workspace, "order/api.hpp", "int hidden();");
        const std::string late = "int run() { return hidden(); }\n#include \"api.hpp\"\n";
        const auto use = Open(workspace, "order/use.cpp", late);
        const std::string shadow = "namespace api { int target(); } namespace inner { namespace api { int other(); } "
        "int run() { return api::target(); } }";
        const auto local = Open(workspace, "order/local.cpp", shadow);
        const auto index = ProjectSymbolIndex::Build(workspace.Snapshot());
        ASSERT_TRUE(index);
        EXPECT_EQ(index->At(use, late.find("hidden")) -> resolution, OccurrenceResolution::Unresolved);
        EXPECT_EQ(index->At(local, shadow.rfind("target")) -> resolution, OccurrenceResolution::Unresolved);
        EXPECT_FALSE(index->Complete());
    }

    TEST(ProjectSymbolIndexSpec, BlocksUnmodeledCallableTypesAndHeaderInternalLinkage)
    {
        Workspace workspace;
        Open(workspace, "unsupported/api.hpp", "static int internal(); int variadic(int input, ...); "
            "int adjusted(const int input); int safe() noexcept;");
        Open(workspace, "unsupported/auto.cpp",
            "auto deduced() { return 1; } const int localConstant = 1;");
        const auto index = ProjectSymbolIndex::Build(workspace.Snapshot());
        ASSERT_TRUE(index);
        EXPECT_FALSE(index->Complete());
        for (const auto name :
            {
                "internal", "variadic", "adjusted", "safe", "deduced", "localConstant"
        })
        {
            const auto ids = index->Named(name);
            ASSERT_EQ(ids.size(), 1u) << name;
            EXPECT_FALSE(index->Entities()[ids.front()].supported) << name;
        }
    }

    TEST(ProjectSymbolIndexSpec, ReportsUnanalyzedCompilationVariants)
    {
        DiskProject project;
        std::string database = "[";
        for (const auto flags :
            {
                "", ",\"-funsigned-char\""
        })
        {
            if (database.size() > 1)
            {
                database += ',';
            }

            database += "{\"directory\":\"" + project.root.generic_string() + "\",\"file\":\"" +
                (project.root / "src/impl.cpp").generic_string() + "\",\"arguments\":[\"c++\",\"-I\",\"include\","
            "\"-c\",\"src/impl.cpp\"" + flags + "]}";
        }

        database += "]";
        project.Write("compile_commands.json", database);
        const auto compilation = CompileDatabase::Load(project.root / "compile_commands.json");
        ASSERT_TRUE(compilation);
        Workspace workspace;
        workspace.SetCompilationDatabase(std::make_shared<const CompileDatabase>(*compilation));
        ASSERT_TRUE(workspace.LoadProjectSources());
        const auto index = workspace.Snapshot().SymbolIndex();
        ASSERT_TRUE(index);
        EXPECT_TRUE(std::ranges::any_of((*index)->Issues(),[](const auto& issue)
            {
                return issue.kind == SymbolIndexIssue::ConfigurationVariants;
        }));
        EXPECT_FALSE((*index)->Complete());
    }

    TEST(ProjectSymbolIndexSpec, DetectsDuplicateDefinitionsAndProducesDeterministicIdentities)
    {
        Workspace workspace;
        Open(workspace, "duplicates/b.cpp", "int run(int input) { return input; }");
        Open(workspace, "duplicates/a.cpp", "int run(int input) { return input; }");
        const auto snapshot = workspace.Snapshot();
        const auto first = ProjectSymbolIndex::Build(snapshot);
        const auto second = ProjectSymbolIndex::Build(snapshot);
        ASSERT_TRUE(first);
        ASSERT_TRUE(second);
        ASSERT_EQ(first->Entities().size(), second->Entities().size());
        for (std::size_t i = 0; i < first->Entities().size(); ++i)
        {
            EXPECT_EQ(first->Entities()[i].identity, second->Entities()[i].identity);
        }

        EXPECT_TRUE(std::ranges::any_of(first->Issues(),[](const auto& issue)
            {
                return issue.kind == SymbolIndexIssue::ConflictingDeclarations;
        }));
        EXPECT_FALSE(first->Complete());
    }

    TEST(ProjectSymbolIndexSpec, DoesNotCertifyAHeaderUnderADifferentTranslationUnitConfiguration)
    {
        Workspace workspace;
        Open(workspace, "contexts/api.hpp", "int external(int input);");
        ParserOptions options;
        options.standard = CppStandard::Cpp23;
        Open(workspace, "contexts/use.cpp", "#include \"api.hpp\"\nint run() { return external(1); }",
            options);
        const auto index = ProjectSymbolIndex::Build(workspace.Snapshot());
        ASSERT_TRUE(index);
        EXPECT_TRUE(std::ranges::any_of(index->Issues(),[](const auto& issue)
            {
                return issue.kind == SymbolIndexIssue::ConfigurationVariants;
        }));
        EXPECT_FALSE(index->Complete());
    }
}
