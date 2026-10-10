#include <Heimdall/CompileDatabase.hpp>
#include <Heimdall/Refactoring.hpp>
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>

namespace
{
    using namespace heimdall;
    struct Fixture
    {
        explicit Fixture(std::string source, ParserOptions options = {})
        {
            id = *workspace.Open(
                "rename.cpp", std::make_shared<const std::string>(std::move(source)), 1,
                std::move(options));
        }

        Workspace workspace;
        DocumentId id;

        AnalysisSnapshot Snapshot()
        {
            return workspace.Snapshot();
        }

        std::size_t At(std::string_view name)
        {
            return Snapshot().Source(id)->find(name);
        }

        auto Rename(std::string_view name, std::string_view replacement)
        {
            return RefactoringService::RenameLocal(Snapshot(), id, At(name), replacement);
        }
    };

    TEST(RenameSpec, RenamesOnlyTheLocalEntityAndPreservesTrivia)
    {
        Fixture f("int f(int input)\r\n{\r\n    // input remains a comment\r\n    int value = "
            "input;\r\n    return value + input;\r\n}\r\n");
        const auto plan = f.Rename("input", "argument");
        ASSERT_TRUE(plan) << plan.error().message;
        ASSERT_EQ(plan->documents[0].edits.size(), 3u);
        const auto preview = PreviewRefactoring(f.Snapshot(), *plan);
        ASSERT_TRUE(preview);
        EXPECT_EQ(preview->front().source,
            "int f(int argument)\r\n{\r\n    // input remains a comment\r\n    int value = "
            "argument;\r\n    return value + argument;\r\n}\r\n");
    }

    TEST(RenameSpec, KeepsShadowedVariablesAndOtherFunctionsUntouched)
    {
        Fixture f("int f() { int value = 1; { int value = 2; value++; } return value; }\n"
            "int g() { int value = 3; return value; }");
        const auto plan = f.Rename("value", "renamed");
        ASSERT_TRUE(plan) << plan.error().message;
        ASSERT_EQ(plan->documents[0].edits.size(), 2u);
        const auto preview = PreviewRefactoring(f.Snapshot(), *plan);
        ASSERT_TRUE(preview);
        EXPECT_NE(preview->front().source.find("int value = 2; value++"), std::string::npos);
        EXPECT_NE(preview->front().source.find("int value = 3; return value"), std::string::npos);
    }

    TEST(RenameSpec, RejectsCollisionsInvalidNamesAndGlobalSymbols)
    {
        Fixture f("int global; int f(int input) { int other = input; return other; }");
        const auto collision = f.Rename("input", "other");
        ASSERT_FALSE(collision);
        EXPECT_EQ(collision.error().code, RefactoringErrorCode::NameCollision);
        for (const auto name :
            {
                "int", "and", "1name", "two names", "_Reserved", "has__reserved", "co_await"
        })
        {
            EXPECT_FALSE(f.Rename("input", name)) << name;
        }

        const auto global = f.Rename("global", "renamed");
        ASSERT_FALSE(global);
        EXPECT_EQ(global.error().code, RefactoringErrorCode::UnsupportedSymbol);
    }

    TEST(RenameSpec, BlocksUnmodeledOccurrencesMacrosTemplatesAndCaptures)
    {
        for (const auto text :
            {
                "int f(int value) { auto fn = [&value] { return value; }; return value; }",
                "template<class T> int f(int value) { return value; }",
                "#define USE value\nint f(int value) { return USE; }",
                "#include <vector>\nint f(int value) { return value; }",
                "int f(int value) { int data[]{value}; return value; }"
        })
        {
            Fixture f(text);
            EXPECT_FALSE(f.Rename("value", "renamed")) << text;
        }

        ParserOptions options;
        options.predefined_macros.emplace("HIDDEN", "value");
        Fixture f("int f(int value) { return value; }", options);
        EXPECT_FALSE(f.Rename("value", "renamed"));
    }

    TEST(RenameSpec, AllowsExistingNamesInIndependentScopes)
    {
        for (const auto source :
            {
                "int f(int value) { return value; } int g(int result) { return result; }",
                "int f() { { int value = 1; return value; } { int result = 2; return result; } }",
                "int f(int flag) { if (flag) { int value = 1; return value; } else { int result = "
                "2; return result; } }",
                "namespace other { int result; } int f(int value) { return value; }"
        })
        {
            Fixture f(source);
            const auto plan = f.Rename("value", "result");
            ASSERT_TRUE(plan) << source << ": " << plan.error().message;
            EXPECT_EQ(plan->documents.front().edits.size(), 2u);
            const auto preview = PreviewRefactoring(f.Snapshot(), *plan);
            ASSERT_TRUE(preview);
            EXPECT_EQ(preview->front().source.find("value"), std::string::npos);
        }
    }

    TEST(RenameSpec, AllowsHarmlessOuterAndInnerShadowing)
    {
        for (const auto source :
            {
                "int result; int f(int value) { return value; }",
                "int f() { int value = 1; { int result = 2; result++; } return value; }",
                "int f() { int value = 1; { int result = 2; int data[]{result}; } return value; }"
        })
        {
            Fixture f(source);
            const auto plan = f.Rename("value", "result");
            ASSERT_TRUE(plan) << source << ": " << plan.error().message;
            EXPECT_EQ(plan->documents.front().edits.size(), 2u);
        }
    }

    TEST(RenameSpec, RejectsSameScopeAndOutermostBodyRedeclarations)
    {
        for (const auto source :
            {
                "int f() { int value = 1; int result = 2; return value; }",
                "int f(int value) { int result = 2; return 0; }",
                "int f(int result) { int value = 1; return value; }",
                "int f() { for (int value = 0; value < 2; ++value) { int result "
                "= 1; } return 0; }"
        })
        {
            Fixture f(source);
            const auto plan = f.Rename("value", "result");
            ASSERT_FALSE(plan) << source;
            EXPECT_EQ(plan.error().code, RefactoringErrorCode::NameCollision) << source;
        }
    }

    TEST(RenameSpec, RejectsCaptureAtEditedAndUneditedUses)
    {
        for (const auto source :
            {
                "int f() { int value = 1; { int result = 2; return value; } return value; }",
                "int result; int f(int value) { return value + result; }",
                "int result; int f() { int value = result; return value; }"
        })
        {
            Fixture f(source);
            const auto plan = f.Rename("value", "result");
            ASSERT_FALSE(plan) << source;
            EXPECT_EQ(plan.error().code, RefactoringErrorCode::NameCollision) << source;
        }
    }

    TEST(RenameSpec, RejectsUnmodeledNewNameUsesOnlyWhenTheyCouldBeCaptured)
    {
        Fixture f("int result; int f(int value) { int data[]{result}; return value; }");
        const auto plan = f.Rename("value", "result");
        ASSERT_FALSE(plan);
        EXPECT_EQ(plan.error().code, RefactoringErrorCode::IncompleteAnalysis);
        Fixture unrelated("int result; int f(int value) { return value; } int g() { int "
            "data[]{result}; return result; }");
        const auto allowed = unrelated.Rename("value", "result");
        ASSERT_TRUE(allowed) << allowed.error().message;
    }

    TEST(RenameSpec, ReferencesHonorDeclarationFlagAndCursorOnUse)
    {
        Fixture f("int f(int value) { return value + value; }");
        const auto snapshot = f.Snapshot();
        const auto refs = RefactoringService::LocalReferences(
            snapshot, f.id, snapshot.Source(f.id)->rfind("value"), false);
        ASSERT_TRUE(refs) << refs.error().message;
        EXPECT_EQ(refs->tokens.size(), 2u);
        EXPECT_EQ(refs->offset, snapshot.Source(f.id)->rfind("value"));
    }

    TEST(RenameSpec, RenamesTheRealCliConstexprLocalWithItsCompilationCommand)
    {
        const auto compiler = std::filesystem::path(HEIMDALL_TEST_CXX_COMPILER).stem().string();
        if (compiler == "cl" || compiler == "clang-cl")
        {
            GTEST_SKIP() << "Compiler-backed macro verification requires GCC/Clang driver options";
        }

        const auto root = std::filesystem::path(HEIMDALL_SOURCE_DIR);
        const auto path = root / "src/cli.cpp";
        std::ifstream input(path, std::ios::binary);
        const std::string source
        {
            std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()
        };
        ASSERT_TRUE(input);
        const auto database = CompileDatabase::Load(
            std::filesystem::path(HEIMDALL_BUILD_DIR) / "compile_commands.json");
        ASSERT_TRUE(database) << database.error();
        const auto* command = database->Find(path);
        ASSERT_NE(command, nullptr);
        Workspace workspace;
        workspace.SetCompilationDatabase(std::make_shared<const CompileDatabase>(*database));
        ParserOptions options;
        options.standard = command->standard;
        options.predefined_macros = command->defines;
        const auto document =
            workspace.Open(path, std::make_shared<const std::string>(source), 1, options);
        ASSERT_TRUE(document);
        const auto snapshot = workspace.Snapshot();
        const auto plan = RefactoringService::RenameLocal(
            snapshot, *document, source.find("kExitUsageError"), "kUsageErrorExitCode");
        ASSERT_TRUE(plan) << plan.error().message;
        EXPECT_EQ(plan->documents.front().edits.size(), 8u);
        const auto preview = PreviewRefactoring(snapshot, *plan);
        ASSERT_TRUE(preview);
        EXPECT_EQ(preview->front().source.find("kExitUsageError"), std::string::npos);
        EXPECT_NE(preview->front().source.find("constexpr int kUsageErrorExitCode = 2;"),
            std::string::npos);
    }

    TEST(RenameSpec, PreservesIncludeSearchOrderAndRejectsDirtyTransitiveHeaders)
    {
        const auto compiler = std::filesystem::path(HEIMDALL_TEST_CXX_COMPILER);
        if (compiler.stem() == "cl" || compiler.stem() == "clang-cl")
        {
            GTEST_SKIP() << "Compiler-backed macro verification requires GCC/Clang driver options";
        }

        const auto root =
            std::filesystem::temp_directory_path() /
            ("heimdall-rename-paths-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_TRUE(std::filesystem::create_directory(root));
        struct Cleanup
        {
            std::filesystem::path root;
            ~Cleanup()
            {
                std::error_code error;
                for (const auto name :
                    {
                        "src/local.hpp", "quote/local.hpp", "includes/relative.hpp",
                        "includes/nested.hpp", "compile_commands.json", "src", "quote", "includes",
                        "build"
                })
                {
                    std::filesystem::remove(root / name, error);
                }

                std::filesystem::remove(root, error);
            }
        } cleanup
        {
            root
        };
        for (const auto name :
            {
                "src", "quote", "includes", "build"
        })
        {
            ASSERT_TRUE(std::filesystem::create_directory(root / name));
        }

        const auto write =[&](const std::filesystem::path& path, std::string_view text)
        {
            std::ofstream file(path, std::ios::binary);
            file << text;
            return file.good();
        };
        ASSERT_TRUE(write(root / "src/local.hpp", "// source-local header wins\n"));
        ASSERT_TRUE(write(root / "quote/local.hpp", "#define value wrong_header\n"));
        ASSERT_TRUE(write(root / "includes/relative.hpp", "#include \"nested.hpp\"\n"));
        ASSERT_TRUE(write(root / "includes/nested.hpp", "// saved transitive header\n"));
        const auto path = root / "src/main.cpp";
        ASSERT_TRUE(write(
            root / "compile_commands.json",
            "[{\"directory\":\"" +(root / "build").generic_string() + "\",\"file\":\"" +
            path.generic_string() + "\",\"arguments\":[\"" + compiler.generic_string() +
            "\",\"-std=c++20\",\"-iquote\",\"../quote\",\"-I\",\"../includes\","
            "\"-c\",\"../src/main.cpp\",\"-o\",\"unused.o\"]}]"));
        const auto database = CompileDatabase::Load(root / "compile_commands.json");
        ASSERT_TRUE(database) << database.error();
        Workspace workspace;
        workspace.SetCompilationDatabase(std::make_shared<const CompileDatabase>(*database));
        const std::string source = "#include \"local.hpp\"\n#include <relative.hpp>\n"
        "int f(int value) { return value; }";
        const auto document = workspace.Open(path, std::make_shared<const std::string>(source), 1);
        ASSERT_TRUE(document);
        const auto rename =[&]
        {
            return RefactoringService::RenameLocal(
                workspace.Snapshot(), *document, source.find("value"), "renamed");
        };
        const auto initial = rename();
        ASSERT_TRUE(initial) << initial.error().message;
        EXPECT_EQ(initial->documents.front().edits.size(), 2u);
        // A dirty header shadowed by the source-local include is not a dependency.
        ASSERT_TRUE(workspace.Open(root / "quote/local.hpp",
            std::make_shared<const std::string>("#define renamed 7\n"), 1));
        const auto header =
            workspace.Open(root / "includes/nested.hpp",
            std::make_shared<const std::string>("// saved transitive header\n"), 1);
        ASSERT_TRUE(header);
        const auto clean = rename();
        ASSERT_TRUE(clean) << clean.error().message;
        ASSERT_TRUE(workspace.Update(
            *header, std::make_shared<const std::string>("#define renamed 7\n"), 2));
        const auto dirty = rename();
        ASSERT_FALSE(dirty);
        EXPECT_EQ(dirty.error().code, RefactoringErrorCode::MacroContext);
        EXPECT_NE(dirty.error().message.find("unsaved"), std::string::npos);
        ASSERT_TRUE(workspace.Update(
            *header, std::make_shared<const std::string>("// saved transitive header\n"), 3));
        const auto restored = rename();
        ASSERT_TRUE(restored) << restored.error().message;
    }

    TEST(RefactoringPlanSpec, RejectsStaleVersionAndContentBeforePreview)
    {
        Fixture f("int f(int value) { return value; }");
        const auto plan = f.Rename("value", "renamed");
        ASSERT_TRUE(plan);
        ASSERT_TRUE(f.workspace.Update(
            f.id, std::make_shared<const std::string>("int f(int value) { return value + 1; }"),
            2));
        const auto result = PreviewRefactoring(f.Snapshot(), *plan);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, RefactoringErrorCode::StaleSnapshot);
    }

    TEST(RefactoringPlanSpec, RejectsOverlapsOutOfBoundsAndExpectedTextMismatch)
    {
        Fixture f("int f(int value) { return value; }");
        auto plan = f.Rename("value", "renamed");
        ASSERT_TRUE(plan);
        const auto valid = *plan;
        plan->documents[0].edits.push_back(plan->documents[0].edits[0]);
        EXPECT_EQ(PreviewRefactoring(f.Snapshot(), *plan).error().code,
            RefactoringErrorCode::ConflictingEdits);
        *plan = valid;
        plan->documents[0].edits[0].offset = 9999;
        EXPECT_EQ(PreviewRefactoring(f.Snapshot(), *plan).error().code,
            RefactoringErrorCode::InvalidEdit);
        *plan = valid;
        plan->documents[0].edits[0].expected = "wrong";
        EXPECT_EQ(PreviewRefactoring(f.Snapshot(), *plan).error().code,
            RefactoringErrorCode::InvalidEdit);
    }

    TEST(RefactoringPlanSpec, ValidatesAllDocumentsAndPinnedParserOptions)
    {
        Fixture f("int f(int value) { return value; }");
        auto plan = f.Rename("value", "renamed");
        ASSERT_TRUE(plan);
        const auto valid = *plan;
        plan->documents[0].options->standard = CppStandard::Cpp23;
        EXPECT_EQ(PreviewRefactoring(f.Snapshot(), *plan).error().code,
            RefactoringErrorCode::StaleSnapshot);
        *plan = valid;
        plan->documents.push_back(plan->documents.front());
        EXPECT_EQ(PreviewRefactoring(f.Snapshot(), *plan).error().code,
            RefactoringErrorCode::ConflictingEdits);
    }

    TEST(RefactoringPlanSpec, CancellationReturnsNoPartialPreview)
    {
        Fixture f("int f(int value) { return value; }");
        auto plan = f.Rename("value", "renamed");
        ASSERT_TRUE(plan);
        std::stop_source stop;
        stop.request_stop();
        EXPECT_EQ(PreviewRefactoring(f.Snapshot(), *plan, stop.get_token()).error().code,
            RefactoringErrorCode::Cancelled);
        EXPECT_FALSE(RefactoringService::RenameLocal(
            f.Snapshot(), f.id, f.At("value"), "renamed", stop.get_token()));
    }

    TEST(ExtractVariableSpec, ExtractsACompleteConstantReturnAndPreservesCrlf)
    {
        Fixture f("int f()\r\n{\r\n    return 1 + 2;\r\n}\r\n");
        const auto plan =
            RefactoringService::ExtractVariable(f.Snapshot(), f.id, f.At("1 + 2"), 5, "result");
        ASSERT_TRUE(plan) << plan.error().message;
        const auto preview = PreviewRefactoring(f.Snapshot(), *plan);
        ASSERT_TRUE(preview);
        EXPECT_EQ(preview->front().source,
            "int f()\r\n{\r\n    auto result = 1 + 2;\r\n    return result;\r\n}\r\n");
    }

    TEST(ExtractVariableSpec, RejectsEffectsPartialSelectionsUnbracedStatementsAndDeducedReturn)
    {
        for (const auto source :
            {
                "int f(int x) { return x++; }", "int f() { return g(); }",
                "int f(int x) { if (x) return 1 + 2; return 0; }", "auto f() { return 1 + 2; }",
                "decltype(auto) f() { return 1 + 2; }"
        })
        {
            Fixture f(source);
            const auto offset = f.Snapshot().Source(f.id)->find("return ") + 7;
            const auto end = f.Snapshot().Source(f.id)->find(';', offset);
            EXPECT_FALSE(RefactoringService::ExtractVariable(
                f.Snapshot(), f.id, offset, end - offset, "result"))
            << source;
        }

        Fixture f("int f() { return 1 + 2; }");
        EXPECT_FALSE(
            RefactoringService::ExtractVariable(f.Snapshot(), f.id, f.At("1 + 2"), 1, "result"));
    }

    TEST(ExtractVariableSpec, BlocksFloatingOperandsAndUserDefinedLiterals)
    {
        for (const auto expression :
            {
                "1.5", "1.5 < 2.5", "42_custom"
        })
        {
            Fixture f("bool f() { return " + std::string(expression) + "; }");
            EXPECT_FALSE(RefactoringService::ExtractVariable(
                f.Snapshot(), f.id, f.At(expression), std::string_view(expression).size(),
                "result"))
            << expression;
        }

        Fixture f("double f() { return 1.5; }");
        EXPECT_FALSE(
            RefactoringService::ExtractFunction(f.Snapshot(), f.id, f.At("1.5"), 3, "helper"));
        Fixture inline_fixture("double f() { double value = 1.5; return value; }");
        EXPECT_FALSE(RefactoringService::InlineVariable(
            inline_fixture.Snapshot(), inline_fixture.id, inline_fixture.At("value")));
    }

    TEST(ExtractVariableSpec, BlocksJumpsAcrossTheInsertedInitializer)
    {
        for (const auto source :
            {
                "int f(int x) { switch (x) { case 0: return 42; default: return 0; } }",
                "int f() { goto done; return 42; done: return 0; }"
        })
        {
            Fixture f(source);
            EXPECT_FALSE(
                RefactoringService::ExtractVariable(f.Snapshot(), f.id, f.At("42"), 2, "result"))
            << source;
        }
    }

    TEST(ExtractVariableSpec, AcceptsIntegralAndBooleanConstantExpressions)
    {
        for (const auto expression :
            {
                "(1 + 2) * 3", "1 < 2", "true && false", "42u"
        })
        {
            Fixture f("int f() { return " + std::string(expression) + "; }");
            const auto plan =
                RefactoringService::ExtractVariable(f.Snapshot(), f.id, f.At(expression),
                std::string_view(expression).size(), "result");
            ASSERT_TRUE(plan) << expression << ": " << plan.error().message;
        }
    }

    TEST(InlineVariableSpec, InlinesSameTypeLiteralAndPreservesUnrelatedBindings)
    {
        Fixture f(
            "int f(int input) { int value = 42; if (input) { return value; } return value; }");
        const auto plan = RefactoringService::InlineVariable(f.Snapshot(), f.id, f.At("value"));
        ASSERT_TRUE(plan) << plan.error().message;
        const auto preview = PreviewRefactoring(f.Snapshot(), *plan);
        ASSERT_TRUE(preview);
        EXPECT_EQ(preview->front().source,
            "int f(int input) {  if (input) { return 42; } return 42; }");
    }

    TEST(InlineVariableSpec, BlocksConversionsEffectsWritesStaticAndDeclarationComments)
    {
        for (const auto source :
            {
                "int f() { int value = g(42); return value; }",
                "int f() { unsigned value = 42; return value; }",
                "int f() { int value = 42; value++; return value; }",
                "int f() { static int value = 42; return value; }",
                "int f() { int value = /* keep */ 42; return value; }",
                "int f() { volatile int value = 42; return value; }",
                "int f() { int value = 42; return value + 1; }"
        })
        {
            Fixture f(source);
            EXPECT_FALSE(RefactoringService::InlineVariable(f.Snapshot(), f.id, f.At("value")))
            << source;
        }
    }

    TEST(ExtractFunctionSpec, ExtractsScalarLiteralToInternalConstexprHelper)
    {
        Fixture f("constexpr int f() { return 42; }");
        const auto plan =
            RefactoringService::ExtractFunction(f.Snapshot(), f.id, f.At("42"), 2, "answer");
        ASSERT_TRUE(plan) << plan.error().message;
        const auto preview = PreviewRefactoring(f.Snapshot(), *plan);
        ASSERT_TRUE(preview);
        EXPECT_EQ(preview->front().source,
            "static constexpr int answer() noexcept\n{\n    return 42;\n}\n\nconstexpr int "
            "f() { return answer(); }");
    }

    TEST(ExtractFunctionSpec, RejectsMembersTemplatesAndExpressionsRequiringInputs)
    {
        for (const auto source :
            {
                "struct S { int f() { return 42; } };", "template<class T> int f() { return 42; }",
                "int f(int value) { return value; }"
        })
        {
            Fixture f(source);
            const auto literal = f.At("42");
            const auto offset = literal == std::string::npos ? f.At("value;") : literal;
            const auto length = literal == std::string::npos ? 5u : 2u;
            EXPECT_FALSE(
                RefactoringService::ExtractFunction(f.Snapshot(), f.id, offset, length, "answer"));
        }
    }
} // namespace
