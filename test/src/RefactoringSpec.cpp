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

    // Three-document workspace (header + definition + caller) with a real
    // compile database so preprocessor verification can run per document.
    struct CrossFile
    {
        explicit CrossFile(std::string_view header_text, std::string_view impl_text,
                           std::string_view caller_text)
        {
            const auto compiler =
                std::filesystem::path(HEIMDALL_TEST_CXX_COMPILER).stem().string();
            if (compiler == "cl" || compiler == "clang-cl")
            {
                return;
            }

            root = std::filesystem::temp_directory_path() /
                   ("heimdall-rename-cross-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directory(root);
            const auto write =[&](const char* name, std::string_view text)
            {
                std::ofstream file(root / name, std::ios::binary);
                file << text;
                return file.good();
            };
            wrote = write("add.hpp", header_text) && write("add.cpp", impl_text) &&
                write("main.cpp", caller_text) &&
                write("compile_commands.json",
                      "[{\"directory\":\"" + root.generic_string() + "\",\"file\":\"" +
                (root / "add.cpp").generic_string() + "\",\"arguments\":[\"" +
                std::filesystem::path(HEIMDALL_TEST_CXX_COMPILER).generic_string() +
                "\",\"-std=c++20\",\"-c\",\"add.cpp\",\"-o\",\"unused.o\"]},"
                "{\"directory\":\"" + root.generic_string() + "\",\"file\":\"" +
                (root / "main.cpp").generic_string() + "\",\"arguments\":[\"" +
                std::filesystem::path(HEIMDALL_TEST_CXX_COMPILER).generic_string() +
                "\",\"-std=c++20\",\"-c\",\"main.cpp\",\"-o\",\"unused.o\"]}]");
            if (!wrote)
            {
                return;
            }

            const auto database = CompileDatabase::Load(root / "compile_commands.json");
            if (!database)
            {
                wrote = false;
                return;
            }

            workspace.SetCompilationDatabase(std::make_shared<const CompileDatabase>(*database));
            auto open =[&](const char* name, std::string_view text)
            {
                std::ifstream input(root / name, std::ios::binary);
                const std::string content
                {
                    std::istreambuf_iterator<char>(input),
                        std::istreambuf_iterator<char>()
                };
                return workspace.Open(root / name,
                                      std::make_shared<const std::string>(content), 1);
            };
            header = open("add.hpp", header_text);
            impl = open("add.cpp", impl_text);
            caller = open("main.cpp", caller_text);
            wrote = wrote && header && impl && caller;
        }

        ~CrossFile()
        {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        bool Ready() const
        {
            const auto compiler =
                std::filesystem::path(HEIMDALL_TEST_CXX_COMPILER).stem().string();
            return wrote && compiler != "cl" && compiler != "clang-cl";
        }

        Workspace workspace;
        std::filesystem::path root;
        bool wrote = false;
        std::expected<DocumentId, WorkspaceError> header, impl, caller;
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

    TEST(RenameSpec, RenamesParameterInItsDoxygenParamTag)
    {
        const std::string source =
            "/**\n"
            " * @brief Describe test_method.\n"
            " *\n"
            " * @param a Describe this parameter.\n"
            " * @param b Describe this parameter.\n"
            " * @param desc Describe this parameter.\n"
            " */\n"
            "void test_method(int a, int b, std::string desc)\n"
            "{\n"
            "    auto value = a + b;\n"
            "}\n";
        Fixture fixture(source);
        const auto snapshot = fixture.Snapshot();

        const auto plan = RefactoringService::RenameLocal(
            snapshot, fixture.id, source.find("int a") + std::string("int ").size(), "first");

        ASSERT_TRUE(plan) << plan.error().message;
        ASSERT_EQ(plan->documents.size(), 1u);
        EXPECT_EQ(plan->documents.front().edits.size(), 3u);
        const auto preview = PreviewRefactoring(snapshot, *plan);
        ASSERT_TRUE(preview);
        ASSERT_EQ(preview->size(), 1u);
        EXPECT_EQ(preview->front().source,
                  "/**\n"
            " * @brief Describe test_method.\n"
            " *\n"
            " * @param first Describe this parameter.\n"
            " * @param b Describe this parameter.\n"
            " * @param desc Describe this parameter.\n"
            " */\n"
            "void test_method(int first, int b, std::string desc)\n"
            "{\n"
            "    auto value = first + b;\n"
            "}\n");
    }

    TEST(RenameSpec, RejectsCollisionsAndInvalidNames)
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

        // Globals resolve across the workspace instead of a single tree, even
        // when the workspace holds a single document.
        const auto global = f.Rename("global", "renamed");
        ASSERT_TRUE(global) << global.error().message;
        EXPECT_EQ(global->documents.size(), 1u);
        EXPECT_EQ(global->documents.front().edits.size(), 1u);
    }

    TEST(RenameSpec, BlocksUnmodeledOccurrencesMacrosAndCaptures)
    {
        for (const auto text :
            {
                "int f(int value) { auto fn = [&value] { return value; }; return value; }",
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

    TEST(RenameSpec, RenamesLexicallyBoundNamesInsideTemplates)
    {
        for (const auto source :
            {
                "template<class T> int f(int value) { return value; }",
                "template<class T> T f(T value) { T local = value; return local; }",
                "template<class T> struct Box { T value; T get() { return value; } };"
        })
        {
            Fixture f(source);
            const auto result = f.Rename("value", "renamed");
            ASSERT_TRUE(result) << source << ": " << result.error().message;
        }
    }

    TEST(RenameSpec, RejectsTemplateParameterNameCapture)
    {
        Fixture f("template<class T> int f(int value) { return value; }");

        const auto result = f.Rename("value", "T");

        EXPECT_FALSE(result);
        EXPECT_EQ(result.error().code, RefactoringErrorCode::NameCollision);
    }

    TEST(RenameSpec, RefusesUnresolvedDependentTemplateMembers)
    {
        Fixture f("template<class T> struct Box { int value; "
            "int get(T object) { return object.value; } };");
        const auto result = f.Rename("value", "renamed");

        EXPECT_FALSE(result);
        EXPECT_EQ(result.error().code, RefactoringErrorCode::IncompleteAnalysis);
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

    TEST(RenameSpec, RenamesForInitVariable)
    {
        Fixture f("int f() { int total = 0; for (int count = 0; count < 3; ++count) { "
            "total += count; } return total; }");
        const auto plan = f.Rename("count", "index");
        ASSERT_TRUE(plan) << plan.error().message;
        EXPECT_EQ(plan->documents.front().edits.size(), 4u);
        const auto preview = PreviewRefactoring(f.Snapshot(), *plan);
        ASSERT_TRUE(preview);
        EXPECT_EQ(preview->front().source.find("count"), std::string::npos);
    }

    TEST(RenameSpec, RenamesRangeForVariable)
    {
        Fixture f("int f() { int total = 0; for (int item : items) { total += item; } "
            "return total; }");
        const auto plan = f.Rename("item", "each");
        ASSERT_TRUE(plan) << plan.error().message;
        EXPECT_EQ(plan->documents.front().edits.size(), 2u);
    }

    TEST(RenameSpec, RenamesStaticLocal)
    {
        Fixture f("int f() { static int tally = 0; ++tally; return tally; }");
        const auto plan = f.Rename("tally", "total");
        ASSERT_TRUE(plan) << plan.error().message;
        EXPECT_EQ(plan->documents.front().edits.size(), 3u);
    }

    TEST(RenameSpec, RenamesCatchParameter)
    {
        Fixture f("int f() { try { throw 1; } catch (const int& err) { return err; } "
            "return 0; }");
        const auto plan = f.Rename("err", "code");
        ASSERT_TRUE(plan) << plan.error().message;
        EXPECT_EQ(plan->documents.front().edits.size(), 2u);
        const auto preview = PreviewRefactoring(f.Snapshot(), *plan);
        ASSERT_TRUE(preview);
        EXPECT_NE(preview->front().source.find("catch (const int& code)"), std::string::npos);
    }

    TEST(RenameSpec, RejectsAmbiguousSiblingCatchParameters)
    {
        Fixture f("int f() { try { throw 1; } catch (int& dup) { return dup; } "
            "catch (long& dup) { return dup; } }");
        const auto plan = f.Rename("dup", "renamed");
        ASSERT_FALSE(plan);
        EXPECT_EQ(plan.error().code, RefactoringErrorCode::IncompleteAnalysis);
    }

    TEST(RenameSpec, RenamesIfSwitchAndWhileInitVariables)
    {
        Fixture if_case("int f(int c) { if (int flag = c) { return flag; } return 0; }");
        const auto if_plan = if_case.Rename("flag", "ok");
        ASSERT_TRUE(if_plan) << if_plan.error().message;
        EXPECT_EQ(if_plan->documents.front().edits.size(), 2u);

        Fixture switch_case("int f(int c) { switch (int key = c) { case 0: return key; "
            "default: return 1; } }");
        const auto switch_plan = switch_case.Rename("key", "code");
        ASSERT_TRUE(switch_plan) << switch_plan.error().message;
        EXPECT_EQ(switch_plan->documents.front().edits.size(), 2u);

        Fixture while_case("int f() { int n = 3; while (int left = n) { n = left - 1; } "
            "return n; }");
        const auto while_plan = while_case.Rename("left", "rest");
        ASSERT_TRUE(while_plan) << while_plan.error().message;
        EXPECT_EQ(while_plan->documents.front().edits.size(), 2u);
    }

    TEST(RenameSpec, RenamesStructuredBindings)
    {
        Fixture f("int f() { auto [alpha, beta] = Pair{1, 2}; return alpha + beta; }");
        const auto plan = f.Rename("alpha", "first");
        ASSERT_TRUE(plan) << plan.error().message;
        EXPECT_EQ(plan->documents.front().edits.size(), 2u);
        const auto preview = PreviewRefactoring(f.Snapshot(), *plan);
        ASSERT_TRUE(preview);
        EXPECT_NE(preview->front().source.find("auto [first, beta]"), std::string::npos);

        Fixture loop("int f() { int total = 0; for (auto [kk, vv] : items) { total += "
            "kk + vv; } return total; }");
        const auto loop_plan = loop.Rename("kk", "key");
        ASSERT_TRUE(loop_plan) << loop_plan.error().message;
        EXPECT_EQ(loop_plan->documents.front().edits.size(), 2u);
    }

    TEST(RenameSpec, AllowsCoroutineTokenOutsideTargetScope)
    {
        Fixture f("int f(bool which) { if (which) { int keep = 1; return keep; } "
            "co_return 0; }");
        const auto plan = f.Rename("keep", "held");
        ASSERT_TRUE(plan) << plan.error().message;
        EXPECT_EQ(plan->documents.front().edits.size(), 2u);

        Fixture inside("int f(bool which) { if (which) { int keep = 1; co_return keep; } "
            "co_return 0; }");
        const auto blocked = inside.Rename("keep", "held");
        ASSERT_FALSE(blocked);
        EXPECT_EQ(blocked.error().code, RefactoringErrorCode::IncompleteAnalysis);
    }

    TEST(RenameSpec, AllowsLambdaOutsideTargetScope)
    {
        // The lambda captures an unrelated parameter; it sits outside the
        // renamed local's scope subtree, so it no longer vetoes the rename.
        Fixture f("int f(bool w) { auto g = [w] { return w; }; if (w) { int keep = 1; "
            "return keep; } return 0; }");
        const auto plan = f.Rename("keep", "held");
        ASSERT_TRUE(plan) << plan.error().message;
        EXPECT_EQ(plan->documents.front().edits.size(), 2u);
    }

    TEST(RenameSpec, AllowsUnrelatedLambdaInsideTargetScope)
    {
        // Mention-based narrowing: the lambda never names the variable, so it
        // cannot interact with the rename.
        Fixture f("int f() { int keep = 1; auto g = [] { return 0; }; return keep; }");
        const auto plan = f.Rename("keep", "held");
        ASSERT_TRUE(plan) << plan.error().message;
        EXPECT_EQ(plan->documents.front().edits.size(), 2u);
    }

    TEST(RenameSpec, RejectsNewNameCapturedInLambda)
    {
        // `[&]` would capture the renamed target: the pre-existing use binds
        // to the global today and to the target after the rename.
        Fixture f("int helper;\n"
            "int f(int old) { auto g = [&] { return helper; }; return old; }");
        const auto plan = f.Rename("old", "helper");
        ASSERT_FALSE(plan);
        EXPECT_EQ(plan.error().code, RefactoringErrorCode::IncompleteAnalysis);
    }

    TEST(RenameSpec, AllowsLambdaParameterShadowingTheNewName)
    {
        // The use already binds to the lambda parameter; shadowing is stable
        // across the rename.
        Fixture f("int f(int old) { auto g = [&](int helper) { return helper; }; "
            "return old; }");
        const auto plan = f.Rename("old", "helper");
        ASSERT_TRUE(plan) << plan.error().message;
        EXPECT_EQ(plan->documents.front().edits.size(), 2u);
    }

    TEST(RenameSpec, RenamesThroughImplicitCaptures)
    {
        // Implicit captures follow the spelling: declaration, body use and
        // outer use are renamed together.
        Fixture f("int f(int old) { auto g = [&] { return old + 1; }; return old; }");
        const auto plan = f.Rename("old", "brand");
        ASSERT_TRUE(plan) << plan.error().message;
        EXPECT_EQ(plan->documents.front().edits.size(), 3u);
        const auto preview = PreviewRefactoring(f.Snapshot(), *plan);
        ASSERT_TRUE(preview);
        EXPECT_EQ(preview->front().source.find("old"), std::string::npos);
    }

    TEST(RenameSpec, RejectsExplicitCapturesOfTheRenamedName)
    {
        for (const auto source :
            {
                "int f(int old) { auto g = [old] { return old; }; return old; }",
                "int f(int old) { auto g = [&old] { return old; }; return old; }",
                "int f(int old) { auto g = [copy = old] { return copy; }; return old; }"
        })
        {
            Fixture f(source);
            const auto plan = f.Rename("old", "brand");
            ASSERT_FALSE(plan) << source;
            EXPECT_EQ(plan.error().code, RefactoringErrorCode::IncompleteAnalysis) << source;
        }
    }

    TEST(RenameSpec, RejectsFieldNameCapturedInLambda)
    {
        Fixture f("int helper;\n"
            "struct S { int m; void run() { auto g = [&] { return helper; }; m = 1; } };");
        const auto plan = f.Rename("m", "helper");
        ASSERT_FALSE(plan);
        EXPECT_EQ(plan.error().code, RefactoringErrorCode::IncompleteAnalysis);

        Fixture shadowed("struct S { int m; void run() { auto g = [&](int helper) { "
            "return helper; }; m = 1; } };");
        const auto allowed = shadowed.Rename("m", "helper");
        ASSERT_TRUE(allowed) << allowed.error().message;
        EXPECT_EQ(allowed->documents.front().edits.size(), 2u);
    }

    TEST(RenameSpec, RenamesFieldThroughObjectAndPointer)
    {
        Fixture f("struct Point { int x; int y; };\n"
            "int f(Point p) { return p.x + p.y; }");
        const auto plan = f.Rename("x", "u");
        ASSERT_TRUE(plan) << plan.error().message;
        EXPECT_EQ(plan->documents.front().edits.size(), 2u);
        const auto preview = PreviewRefactoring(f.Snapshot(), *plan);
        ASSERT_TRUE(preview);
        EXPECT_NE(preview->front().source.find("int u;"), std::string::npos);
        EXPECT_NE(preview->front().source.find("p.u + p.y"), std::string::npos);

        Fixture via_use("struct S { int value; };\n"
            "int f(S* p) { return p->value + p->value; }");
        const auto snapshot = via_use.Snapshot();
        const auto offset =
            snapshot.Source(via_use.id)->find("p->value") + std::string("p->").size();
        const auto use_plan =
            RefactoringService::RenameLocal(snapshot, via_use.id, offset, "amount");
        ASSERT_TRUE(use_plan) << use_plan.error().message;
        EXPECT_EQ(use_plan->documents.front().edits.size(), 3u);
    }

    TEST(RenameSpec, RenamesInheritedField)
    {
        Fixture f("struct B { int base; };\n"
            "struct D : B { int own; };\n"
            "int f(D d) { return d.base + d.own; }");
        const auto plan = f.Rename("base", "basis");
        ASSERT_TRUE(plan) << plan.error().message;
        EXPECT_EQ(plan->documents.front().edits.size(), 2u);
    }

    TEST(RenameSpec, RenamesFieldUsedBareAndThroughThisInMemberFunctions)
    {
        Fixture f("struct S { int m; int get() { return m; } int getThis() { return "
            "this->m; } void set(int v) { m = v; } };");
        const auto plan = f.Rename("m", "value");
        ASSERT_TRUE(plan) << plan.error().message;
        EXPECT_EQ(plan->documents.front().edits.size(), 4u);
    }

    TEST(RenameSpec, KeepsLocalShadowingAFieldUntouched)
    {
        Fixture f("struct S { int m; int get() { int m = 0; return m; } void set(int "
            "v) { m = v; } };");
        const auto plan = f.Rename("m", "value");
        ASSERT_TRUE(plan) << plan.error().message;
        // Field declaration plus the unshadowed use in set(); the shadowing
        // local in get() and its uses keep their spelling.
        EXPECT_EQ(plan->documents.front().edits.size(), 2u);
        const auto preview = PreviewRefactoring(f.Snapshot(), *plan);
        ASSERT_TRUE(preview);
        EXPECT_NE(preview->front().source.find("int value;"), std::string::npos);
        EXPECT_NE(preview->front().source.find("int m = 0"), std::string::npos);
        EXPECT_NE(preview->front().source.find("value = v"), std::string::npos);
    }

    TEST(RenameSpec, RejectsFieldRenameCollisions)
    {
        Fixture same_class("struct S { int a; int b; };\n"
            "int f(S s) { return s.a; }");
        const auto same = same_class.Rename("a", "b");
        ASSERT_FALSE(same);
        EXPECT_EQ(same.error().code, RefactoringErrorCode::NameCollision);

        Fixture base("struct B { int val; };\n"
            "struct D : B { int old; };\n"
            "void f(D d) { d.val = 1; d.old = 2; }");
        const auto hidden = base.Rename("old", "val");
        ASSERT_FALSE(hidden);
        EXPECT_EQ(hidden.error().code, RefactoringErrorCode::NameCollision);
    }

    TEST(RenameSpec, RejectsMethodsAndUnresolvedMembers)
    {
        Fixture method("struct S { int m(); };\n"
            "int f(S s) { return s.m(); }");
        const auto method_plan = method.Rename("m", "renamed");
        ASSERT_FALSE(method_plan);
        EXPECT_EQ(method_plan.error().code, RefactoringErrorCode::UnsupportedSymbol);

        Fixture opaque("struct Opaque;\n"
            "int f(Opaque* p) { return p->x; }");
        const auto opaque_plan = opaque.Rename("x", "renamed");
        ASSERT_FALSE(opaque_plan);
        EXPECT_EQ(opaque_plan.error().code, RefactoringErrorCode::UnsupportedSymbol);
    }

    TEST(RenameCrossFileSpec, RenamesFreeFunctionAcrossThreeDocuments)
    {
        CrossFile files("int add(int a, int b);\n", "#include \"add.hpp\"\n"
            "int add(int a, int b) { return a + b; }\n", "#include \"add.hpp\"\n"
            "int run() { return add(1, 2); }\n");
        ASSERT_TRUE(files.Ready());
        const auto snapshot = files.workspace.Snapshot();
        const auto source = snapshot.Source(*files.impl);
        const auto plan = RefactoringService::RenameLocal(
            snapshot, *files.impl, source->find("add(int"), "sum");
        ASSERT_TRUE(plan) << plan.error().message;
        ASSERT_EQ(plan->documents.size(), 3u);
        for (const auto& documents : plan->documents)
        {
            EXPECT_EQ(documents.edits.size(), 1u);
        }

        const auto preview = PreviewRefactoring(snapshot, *plan);
        ASSERT_TRUE(preview);
        ASSERT_EQ(preview->size(), 3u);
        for (const auto& refactored : *preview)
        {
            // Every document spells the new name; includes still name add.hpp.
            EXPECT_NE(refactored.source.find("sum"), std::string::npos) << refactored.path;
        }
    }

    TEST(RenameCrossFileSpec, RenamesFunctionInDoxygenBriefAndReferences)
    {
        const std::string header =
            "/**\n"
            " * @brief Describe add.\n"
            " * @param a First operand.\n"
            " * @param b Second operand.\n"
            " * @return The sum.\n"
            " */\n"
            "int add(int a, int b);\n";
        const std::string implementation =
            "#include \"add.hpp\"\n"
            "int add(int a, int b) { return a + b; }\n";
        const std::string caller =
            "#include \"add.hpp\"\n"
            "/** @brief Calls @ref add.\n"
            " * @see add\n"
            " */\n"
            "int run() { return add(1, 2); }\n";
        CrossFile files(header, implementation, caller);
        ASSERT_TRUE(files.Ready());
        const auto snapshot = files.workspace.Snapshot();

        const auto plan = RefactoringService::RenameLocal(
            snapshot, *files.impl, implementation.find("add(int"), "sum");

        ASSERT_TRUE(plan) << plan.error().message;
        const auto preview = PreviewRefactoring(snapshot, *plan);
        ASSERT_TRUE(preview);
        ASSERT_EQ(preview->size(), 3u);
        for (const auto& document : *preview)
        {
            if (document.path.filename() == "add.hpp")
            {
                EXPECT_EQ(document.source,
                          "/**\n"
                    " * @brief Describe sum.\n"
                    " * @param a First operand.\n"
                    " * @param b Second operand.\n"
                    " * @return The sum.\n"
                    " */\n"
                    "int sum(int a, int b);\n");
            }
            else if (document.path.filename() == "add.cpp")
            {
                EXPECT_EQ(document.source,
                          "#include \"add.hpp\"\n"
                    "int sum(int a, int b) { return a + b; }\n");
            }
            else
            {
                EXPECT_EQ(document.path.filename(), "main.cpp");
                EXPECT_EQ(document.source,
                          "#include \"add.hpp\"\n"
                    "/** @brief Calls @ref sum.\n"
                    " * @see sum\n"
                    " */\n"
                    "int run() { return sum(1, 2); }\n");
            }
        }
    }

    TEST(RenameSpec, RenamesClassInDoxygenBriefAndReferences)
    {
        const std::string source =
            "/**\n"
            " * @brief Describe Node.\n"
            " */\n"
            "class Node {};\n"
            "/** @brief Uses @ref Node.\n"
            " * @see Node\n"
            " */\n"
            "void use(Node* node) {}\n";
        Fixture fixture(source);
        const auto snapshot = fixture.Snapshot();

        const auto plan = RefactoringService::RenameLocal(
            snapshot, fixture.id, source.find("class Node") + std::string("class ").size(),
            "Element");

        ASSERT_TRUE(plan) << plan.error().message;
        const auto preview = PreviewRefactoring(snapshot, *plan);
        ASSERT_TRUE(preview);
        ASSERT_EQ(preview->size(), 1u);
        EXPECT_EQ(preview->front().source,
                  "/**\n"
            " * @brief Describe Element.\n"
            " */\n"
            "class Element {};\n"
            "/** @brief Uses @ref Element.\n"
            " * @see Element\n"
            " */\n"
            "void use(Element* node) {}\n");
    }

    TEST(RenameCrossFileSpec, FindsReferencesAcrossDocuments)
    {
        Workspace workspace;
        const auto impl = workspace.Open("a.cpp",
            std::make_shared<const std::string>("int add(int a, int b) { return a + b; }"), 1);
        ASSERT_TRUE(impl);
        const auto caller = workspace.Open("b.cpp",
            std::make_shared<const std::string>(
            "int add(int a, int b);\nint run() { return add(1, 2); }"),
            1);
        ASSERT_TRUE(caller);
        const auto snapshot = workspace.Snapshot();
        const auto source = snapshot.Source(*impl);
        const auto refs = RefactoringService::WorkspaceReferences(
            snapshot, *impl, source->find("add"), true);
        ASSERT_TRUE(refs) << refs.error().message;
        ASSERT_EQ(refs->size(), 2u);
        std::size_t tokens = 0;
        for (const auto& document_refs : *refs)
        {
            tokens += document_refs.tokens.size();
        }

        // Definition, redeclaration and call.
        EXPECT_EQ(tokens, 3u);

        const auto excluding = RefactoringService::WorkspaceReferences(
            snapshot, *impl, source->find("add"), false);
        ASSERT_TRUE(excluding) << excluding.error().message;
        std::size_t uses = 0;
        for (const auto& document_refs : *excluding)
        {
            uses += document_refs.tokens.size();
        }

        EXPECT_EQ(uses, 1u);
    }

    TEST(RenameCrossFileSpec, RenamesAcrossDocumentsWithoutIncludes)
    {
        // No directives: the caller redeclares the function, so the index can
        // link the documents without include edges or a compile database.
        Workspace workspace;
        const auto impl = workspace.Open("a.cpp",
            std::make_shared<const std::string>("int add(int a, int b) { return a + b; }"), 1);
        ASSERT_TRUE(impl);
        const auto caller = workspace.Open("b.cpp",
            std::make_shared<const std::string>(
            "int add(int a, int b);\nint run() { return add(1, 2); }"),
            1);
        ASSERT_TRUE(caller);
        const auto snapshot = workspace.Snapshot();
        const auto source = snapshot.Source(*impl);
        const auto plan = RefactoringService::RenameLocal(
            snapshot, *impl, source->find("add"), "sum");
        ASSERT_TRUE(plan) << plan.error().message;
        ASSERT_EQ(plan->documents.size(), 2u);
        std::size_t edits = 0;
        for (const auto& documents : plan->documents)
        {
            edits += documents.edits.size();
        }

        EXPECT_EQ(edits, 3u);
    }

    TEST(RenameCrossFileSpec, RenamesGlobalVariableAcrossDocuments)
    {
        CrossFile files("extern int total;\n", "#include \"add.hpp\"\n"
            "int total = 0;\n", "#include \"add.hpp\"\n"
            "int get() { return total; }\n");
        ASSERT_TRUE(files.Ready());
        const auto snapshot = files.workspace.Snapshot();
        const auto source = snapshot.Source(*files.impl);
        const auto plan = RefactoringService::RenameLocal(
            snapshot, *files.impl, source->find("total"), "count");
        ASSERT_TRUE(plan) << plan.error().message;
        ASSERT_EQ(plan->documents.size(), 3u);
    }

    TEST(RenameCrossFileSpec, RejectsOverloadsAndCollisionsAcrossFiles)
    {
        CrossFile overloaded("int add(int a, int b);\ndouble add(double a, double b);\n",
                             "#include \"add.hpp\"\n"
            "int add(int a, int b) { return a + b; }\n"
            "double add(double a, double b) { return a + b; }\n", "#include \"add.hpp\"\n"
            "int run() { return add(1, 2); }\n");
        ASSERT_TRUE(overloaded.Ready());
        const auto over_snapshot = overloaded.workspace.Snapshot();
        const auto over_source = over_snapshot.Source(*overloaded.impl);
        const auto over_plan = RefactoringService::RenameLocal(
            over_snapshot, *overloaded.impl, over_source->find("add(int"), "sum");
        ASSERT_FALSE(over_plan);
        EXPECT_EQ(over_plan.error().code, RefactoringErrorCode::IncompleteAnalysis);

        CrossFile files("int add(int a, int b);\n", "#include \"add.hpp\"\n"
            "int add(int a, int b) { return a + b; }\n", "#include \"add.hpp\"\n"
            "int run() { return add(1, 2); }\n");
        ASSERT_TRUE(files.Ready());
        const auto snapshot = files.workspace.Snapshot();
        const auto source = snapshot.Source(*files.caller);
        const auto collision = RefactoringService::RenameLocal(
            snapshot, *files.caller, source->find("add("), "run");
        ASSERT_FALSE(collision);
        EXPECT_EQ(collision.error().code, RefactoringErrorCode::NameCollision);
    }

    TEST(ChangeSignatureSpec, AddsDefaultedParameterToDeclarationAndDefinition)
    {
        CrossFile files("int add(int a, int b);\n", "#include \"add.hpp\"\n"
            "int add(int a, int b) { return a + b; }\n", "#include \"add.hpp\"\n"
            "int run() { return add(1, 2); }\n");
        ASSERT_TRUE(files.Ready());
        const auto snapshot = files.workspace.Snapshot();
        const auto source = snapshot.Source(*files.impl);
        const auto plan = RefactoringService::AddParameter(
            snapshot, *files.impl, source->find("add(int"), "int", "c", "0");
        ASSERT_TRUE(plan) << plan.error().message;
        // Declaration and definition gain the parameter; the call is preserved.
        ASSERT_EQ(plan->documents.size(), 2u);
        const auto preview = PreviewRefactoring(snapshot, *plan);
        ASSERT_TRUE(preview);
        for (const auto& refactored : *preview)
        {
            // Declaration and definition gain the parameter...
            EXPECT_NE(refactored.source.find("int add(int a, int b, int c = 0)"),
                      std::string::npos)
            << refactored.path;
            // ...while the existing call keeps working untouched.
            if (refactored.document == *files.caller)
            {
                EXPECT_NE(refactored.source.find("add(1, 2)"), std::string::npos);
            }
            else
            {
                EXPECT_EQ(refactored.source.find("add(1, 2)"), std::string::npos);
            }
        }
    }

    TEST(ChangeSignatureSpec, RemovesUnusedParameterAndRewritesCalls)
    {
        CrossFile files("int add(int a, int b);\n", "#include \"add.hpp\"\n"
            "int add(int a, int b) { return a; }\n", "#include \"add.hpp\"\n"
            "int run() { return add(1, 2) + add(3, 4); }\n");
        ASSERT_TRUE(files.Ready());
        const auto snapshot = files.workspace.Snapshot();
        const auto source = snapshot.Source(*files.impl);
        const auto plan = RefactoringService::RemoveParameter(
            snapshot, *files.impl, source->find("add(int"), "b");
        ASSERT_TRUE(plan) << plan.error().message;
        ASSERT_EQ(plan->documents.size(), 3u);
        const auto preview = PreviewRefactoring(snapshot, *plan);
        ASSERT_TRUE(preview);
        for (const auto& refactored : *preview)
        {
            EXPECT_EQ(refactored.source.find("int b"), std::string::npos) << refactored.path;
        }

        for (const auto& refactored : *preview)
        {
            if (refactored.document == *files.caller)
            {
                EXPECT_NE(refactored.source.find("add(1) + add(3)"), std::string::npos);
            }
        }
    }

    TEST(ChangeSignatureSpec, RejectsUsedParametersAddressTakersAndOverloads)
    {
        CrossFile used("int add(int a, int b);\n", "#include \"add.hpp\"\n"
            "int add(int a, int b) { return a + b; }\n", "#include \"add.hpp\"\n"
            "int run() { return add(1, 2); }\n");
        ASSERT_TRUE(used.Ready());
        const auto used_snapshot = used.workspace.Snapshot();
        const auto used_source = used_snapshot.Source(*used.impl);
        EXPECT_FALSE(RefactoringService::RemoveParameter(
            used_snapshot, *used.impl, used_source->find("add(int"), "b"));

        CrossFile taken("int add(int a, int b);\n", "#include \"add.hpp\"\n"
            "int add(int a, int b) { return a; }\n", "#include \"add.hpp\"\n"
            "auto taken = &add;\nint run() { return taken(1, 2); }\n");
        ASSERT_TRUE(taken.Ready());
        const auto taken_snapshot = taken.workspace.Snapshot();
        const auto taken_source = taken_snapshot.Source(*taken.impl);
        EXPECT_FALSE(RefactoringService::AddParameter(
            taken_snapshot, *taken.impl, taken_source->find("add(int"), "int", "c", "0"));
        EXPECT_FALSE(RefactoringService::RemoveParameter(
            taken_snapshot, *taken.impl, taken_source->find("add(int"), "b"));

        CrossFile collision("int add(int a, int b);\n", "#include \"add.hpp\"\n"
            "int add(int a, int b) { return a + b; }\n", "#include \"add.hpp\"\n"
            "int run() { return add(1, 2); }\n");
        ASSERT_TRUE(collision.Ready());
        const auto collision_snapshot = collision.workspace.Snapshot();
        const auto collision_source = collision_snapshot.Source(*collision.impl);
        const auto colliding = RefactoringService::AddParameter(collision_snapshot,
            *collision.impl, collision_source->find("add(int"), "int", "a", "0");
        ASSERT_FALSE(colliding);
        EXPECT_EQ(colliding.error().code, RefactoringErrorCode::NameCollision);
    }

    TEST(MoveSymbolSpec, MovesFreeFunctionIntoNamespaceWithQualifiedCalls)
    {
        Workspace workspace;
        const auto impl = workspace.Open("a.cpp",
            std::make_shared<const std::string>(
            "namespace util { int helper() { return 1; } }\n"
            "int add(int a, int b) { return a + b; }\n"
            "int run() { return add(1, 2) + util::helper(); }\n"),
            1);
        ASSERT_TRUE(impl);
        const auto snapshot = workspace.Snapshot();
        const auto source = snapshot.Source(*impl);
        const auto plan = RefactoringService::MoveToNamespace(
            snapshot, *impl, source->find("add(int"), "util");
        ASSERT_TRUE(plan) << plan.error().message;
        ASSERT_EQ(plan->documents.size(), 1u);
        const auto preview = PreviewRefactoring(snapshot, *plan);
        ASSERT_TRUE(preview);
        EXPECT_NE(preview->front().source.find("int util::add(int a, int b)"),
                  std::string::npos);
        EXPECT_NE(preview->front().source.find("util::add(1, 2)"), std::string::npos);
        EXPECT_NE(preview->front().source.find("util::helper()"), std::string::npos);
    }

    TEST(MoveSymbolSpec, RejectsExternalUsesMissingNamespacesAndCollisions)
    {
        // Used outside the document.
        {
            Workspace workspace;
            const auto impl = workspace.Open("a.cpp",
                std::make_shared<const std::string>(
                "namespace util { int helper() { return 1; } }\n"
                "int add(int a, int b) { return a + b; }\n"),
                1);
            ASSERT_TRUE(impl);
            const auto caller = workspace.Open("b.cpp",
                std::make_shared<const std::string>(
                "int add(int a, int b);\nint run() { return add(1, 2); }\n"),
                1);
            ASSERT_TRUE(caller);
            const auto snapshot = workspace.Snapshot();
            const auto source = snapshot.Source(*impl);
            const auto plan = RefactoringService::MoveToNamespace(
                snapshot, *impl, source->find("add(int"), "util");
            ASSERT_FALSE(plan);
            EXPECT_EQ(plan.error().code, RefactoringErrorCode::IncompleteAnalysis);
        }

        // Missing namespace and collision.
        {
            Workspace workspace;
            const auto impl = workspace.Open("a.cpp",
                std::make_shared<const std::string>(
                "namespace util { int helper() { return 1; } }\n"
                "int add(int a, int b) { return a + b; }\n"
                "int run() { return add(1, 2); }\n"),
                1);
            ASSERT_TRUE(impl);
            const auto snapshot = workspace.Snapshot();
            const auto source = snapshot.Source(*impl);
            EXPECT_FALSE(RefactoringService::MoveToNamespace(
                snapshot, *impl, source->find("add(int"), "missing"));
            Fixture collision(
                "namespace util { int add(int x); }\n"
                "int add(int a, int b) { return a + b; }\n"
                "int run() { return add(1, 2); }\n");
            const auto collision_snapshot = collision.Snapshot();
            const auto collision_plan = RefactoringService::MoveToNamespace(collision_snapshot,
                collision.id, collision_snapshot.Source(collision.id)->rfind("add(int"), "util");
            ASSERT_FALSE(collision_plan);
            EXPECT_EQ(collision_plan.error().code, RefactoringErrorCode::NameCollision);
        }

        // Member functions and namespace-block definitions stay put.
        {
            Fixture method("struct S { int m() { return 1; } };\n");
            EXPECT_FALSE(RefactoringService::MoveToNamespace(
                method.Snapshot(), method.id, method.At("m()"), "util"));
            Fixture blocked(
                "namespace m { int add(int a, int b) { return a + b; } }\n"
                "namespace util { int helper() { return 1; } }\n");
            EXPECT_FALSE(RefactoringService::MoveToNamespace(
                blocked.Snapshot(), blocked.id, blocked.At("add(int"), "util"));
        }
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

    TEST(RenameSpec, RenameFromAnyUseChangesOnlyBoundOccurrences)
    {
        Fixture f("int f(int value) { return value + value; }\n"
            "int g(int value) { return value; }");
        const auto snapshot = f.Snapshot();
        const auto source = snapshot.Source(f.id);
        const auto plan = RefactoringService::RenameLocal(
            snapshot, f.id, source->find("value +"), "amount");

        ASSERT_TRUE(plan) << plan.error().message;
        ASSERT_EQ(plan->documents.size(), 1u);
        EXPECT_EQ(plan->documents.front().edits.size(), 3u);
        const auto preview = PreviewRefactoring(snapshot, *plan);
        ASSERT_TRUE(preview);
        EXPECT_EQ(preview->front().source,
                  "int f(int amount) { return amount + amount; }\n"
            "int g(int value) { return value; }");
    }

    TEST(RenameSpec, RejectsRenameOnNonIdentifierTokens)
    {
        Fixture f("int f(int value) { return value + 1; }");
        const auto snapshot = f.Snapshot();
        const auto source = snapshot.Source(f.id);

        for (const auto offset :
            {
                source->find("return"), source->find('+'), source->find('1')
        })
        {
            const auto plan =
                RefactoringService::RenameLocal(snapshot, f.id, offset, "amount");
            EXPECT_FALSE(plan) << offset;
            EXPECT_EQ(plan.error().code, RefactoringErrorCode::InvalidSelection) << offset;
        }
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

    TEST(ExtractVariableSpec, AllowsJumpsThatCannotCrossTheInitializer)
    {
        // Single-case switch: dispatch lands on the only label, before the
        // inserted initializer.
        Fixture single("int f(int x) { switch (x) { case 0: return 42; } return 0; }");
        const auto single_plan = RefactoringService::ExtractVariable(
            single.Snapshot(), single.id, single.At("42"), 2, "result");
        ASSERT_TRUE(single_plan) << single_plan.error().message;

        // Goto after the statement and backward gotos execute the initializer.
        Fixture after("int f() { return 42; goto skip; skip: return 0; }");
        const auto after_plan = RefactoringService::ExtractVariable(
            after.Snapshot(), after.id, after.At("42"), 2, "result");
        ASSERT_TRUE(after_plan) << after_plan.error().message;

        Fixture loop("int f(int n) { int i = 0; again: i = n + 1; if (i < 10) goto "
            "again; return 42; }");
        const auto loop_plan = RefactoringService::ExtractVariable(
            loop.Snapshot(), loop.id, loop.At("42"), 2, "result");
        ASSERT_TRUE(loop_plan) << loop_plan.error().message;
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

    TEST(ExtractVariableSpec, ExtractsReadsOfLocalsParametersAndFields)
    {
        Fixture local("int f(int x) { return x + 1; }");
        const auto local_plan = RefactoringService::ExtractVariable(
            local.Snapshot(), local.id, local.At("x + 1"), 5, "result");
        ASSERT_TRUE(local_plan) << local_plan.error().message;
        const auto local_preview = PreviewRefactoring(local.Snapshot(), *local_plan);
        ASSERT_TRUE(local_preview);
        EXPECT_EQ(local_preview->front().source,
                  "int f(int x) { auto result = x + 1; return result; }");

        Fixture field("struct S { int m; };\n"
            "int f(S s) { return s.m + 1; }");
        const auto field_plan = RefactoringService::ExtractVariable(
            field.Snapshot(), field.id, field.At("s.m + 1"), 7, "result");
        ASSERT_TRUE(field_plan) << field_plan.error().message;
    }

    TEST(ExtractVariableSpec, ExtractsConstexprCallsWithPureArguments)
    {
        Fixture f("constexpr int twice(int v) { return v * 2; }\n"
            "int f(int x) { return twice(x) + 1; }");
        const auto plan = RefactoringService::ExtractVariable(
            f.Snapshot(), f.id, f.At("twice(x) + 1"), 12, "result");
        ASSERT_TRUE(plan) << plan.error().message;
    }

    TEST(ExtractConstantSpec, ExtractsRepeatedLiteralsIntoANamedConstant)
    {
        Fixture f("int f(int x) { return x + 42 + 42; }");
        const auto snapshot = f.Snapshot();
        const auto plan = RefactoringService::ExtractConstant(
            snapshot, f.id, snapshot.Source(f.id)->find("42"), 2, "answer");
        ASSERT_TRUE(plan) << plan.error().message;
        ASSERT_EQ(plan->documents.front().edits.size(), 3u);
        const auto preview = PreviewRefactoring(snapshot, *plan);
        ASSERT_TRUE(preview);
        EXPECT_EQ(preview->front().source,
                  "int f(int x) { static constexpr int answer = 42; return x + answer + answer; }");
    }

    TEST(ExtractConstantSpec, ExtractsDoublesAndRejectsNonArithmetic)
    {
        Fixture doubles("double f() { return 1.5 + 1.5; }");
        const auto doubles_snapshot = doubles.Snapshot();
        const auto doubles_plan = RefactoringService::ExtractConstant(doubles_snapshot,
            doubles.id, doubles_snapshot.Source(doubles.id)->find("1.5"), 3, "half_pi");
        ASSERT_TRUE(doubles_plan) << doubles_plan.error().message;

        for (const auto source :
            {
                "int f() { return 42_custom; }",
                "int f() { auto g = [] { return 42; }; return 42; }"
        })
        {
            Fixture f(source);
            const auto snapshot = f.Snapshot();
            // Outermost match: the lambda case selects outside and vetoes on
            // the nested identical literal.
            EXPECT_FALSE(RefactoringService::ExtractConstant(
                snapshot, f.id, snapshot.Source(f.id)->rfind("42"), 2, "answer"))
            << source;
        }
    }

    TEST(ExtractVariableSpec, AllowsMacroFreeFunctionsDespiteFileDirectives)
    {
        // `#pragma once` and unused file-local defines do not interact.
        Fixture pragma("#pragma once\nint f(int x) { return x + 1; }\n");
        const auto pragma_plan = RefactoringService::ExtractVariable(
            pragma.Snapshot(), pragma.id, pragma.At("x + 1"), 5, "result");
        ASSERT_TRUE(pragma_plan) << pragma_plan.error().message;

        Fixture unused("#define LIMIT 100\nint f(int x) { return x + 1; }\n");
        const auto unused_plan = RefactoringService::ExtractVariable(
            unused.Snapshot(), unused.id, unused.At("x + 1"), 5, "result");
        ASSERT_TRUE(unused_plan) << unused_plan.error().message;

        // Pragma inside the function is inert (macro-stack pragmas excluded).
        Fixture omp("int f(int x) {\n#pragma omp simd\nreturn x + 1;\n}\n");
        const auto omp_plan = RefactoringService::ExtractVariable(
            omp.Snapshot(), omp.id, omp.At("x + 1"), 5, "result");
        ASSERT_TRUE(omp_plan) << omp_plan.error().message;
    }

    TEST(ExtractVariableSpec, RejectsMacroInfluencedFunctions)
    {
        // Unknown header macros, configuration branches and macro uses.
        for (const auto source :
            {
                "#include \"nothing.hpp\"\nint f(int x) { return x + 1; }\n",
                "#ifndef H\n#define H\nint f(int x) { return x + 1; }\n#endif\n",
                "#define LIMIT 100\nint f() { return LIMIT + 1; }\n"
        })
        {
            Fixture f(source);
            const auto snapshot = f.Snapshot();
            const auto offset =
                snapshot.Source(f.id)->find("return ") + std::string("return ").size();
            const auto end = snapshot.Source(f.id)->find(';', offset);
            EXPECT_FALSE(RefactoringService::ExtractVariable(
                snapshot, f.id, offset, end - offset, "result"))
            << source;
        }

        // A defined macro shadowing a real declaration: purity passes on the
        // raw text, so only the macro scope check can veto.
        Fixture shadowed("#define x 5\nint f() { int x = 1; return x + 1; }\n");
        {
            const auto snapshot = shadowed.Snapshot();
            EXPECT_FALSE(RefactoringService::ExtractVariable(snapshot, shadowed.id,
                                                             snapshot.Source(shadowed.id)->find("x + 1"), 5, "result"));
        }

        // Predefined macros behave like file defines when named.
        {
            ParserOptions options;
            options.predefined_macros.emplace("LIMIT", "100");
            Fixture clean("int f(int x) { return x + 1; }\n", options);
            EXPECT_TRUE(RefactoringService::ExtractVariable(
                clean.Snapshot(), clean.id, clean.At("x + 1"), 5, "result"));
            Fixture dirty("int f() { int LIMIT = 1; return LIMIT + 1; }\n", options);
            EXPECT_FALSE(RefactoringService::ExtractVariable(dirty.Snapshot(), dirty.id,
                                                             dirty.Snapshot().Source(dirty.id)->find("LIMIT + 1"), 9, "result"));
        }
    }

    TEST(ExtractVariableSpec, RejectsVolatileUnresolvedAndEffectfulReads)
    {
        for (const auto source :
            {
                "int f() { volatile int v = 42; return v + 1; }",
                "int g(int v);\nint f(int x) { return g(x) + 1; }",
                "int f() { return missing + 1; }",
                "struct S { int get(); };\nint f(S s) { return s.get() + 1; }",
                "int f(int x) { return x++ + 1; }"
        })
        {
            Fixture f(source);
            const auto offset = f.Snapshot().Source(f.id)->find("return ") + 7;
            const auto end = f.Snapshot().Source(f.id)->find(';', offset);
            EXPECT_FALSE(RefactoringService::ExtractVariable(
                f.Snapshot(), f.id, offset, end - offset, "result"))
            << source;
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

    TEST(ExtractFunctionSpec, RejectsMembersTemplatesAndEffectfulExpressions)
    {
        for (const auto source :
            {
                "struct S { int f() { return 42; } };", "template<class T> int f() { return 42; }",
                "int f(int x) { return x++ + 1; }",
                "struct S { int m; };\nconstexpr int g(S v);\nint f(S s) { return g(s); }"
        })
        {
            Fixture f(source);
            const auto literal = f.At("42");
            const auto offset = literal == std::string::npos ? f.At("value;") : literal;
            const auto found = offset == std::string::npos
            ? f.Snapshot().Source(f.id)->find("return ") + 7
            : offset;
            const auto end = f.Snapshot().Source(f.id)->find(';', found);
            EXPECT_FALSE(RefactoringService::ExtractFunction(
                f.Snapshot(), f.id, found, end - found, "answer"))
            << source;
        }
    }

    TEST(ExtractFunctionSpec, ExtractsPureExpressionWithInferredParameters)
    {
        Fixture f("int f(int x, int y) { return x * 2 + y; }");
        const auto plan =
            RefactoringService::ExtractFunction(f.Snapshot(), f.id, f.At("x * 2 + y"), 9, "combine");
        ASSERT_TRUE(plan) << plan.error().message;
        const auto preview = PreviewRefactoring(f.Snapshot(), *plan);
        ASSERT_TRUE(preview);
        EXPECT_EQ(preview->front().source,
                  "static constexpr int combine(int x, int y) noexcept\n{\n    return x * 2 + y;\n}\n\nint "
            "f(int x, int y) { return combine(x, y); }");

        Fixture field("struct S { int m; };\n"
            "int f(S s, int k) { return s.m * k; }");
        const auto field_plan = RefactoringService::ExtractFunction(
            field.Snapshot(), field.id, field.At("s.m * k"), 7, "combine");
        ASSERT_TRUE(field_plan) << field_plan.error().message;
        const auto field_preview = PreviewRefactoring(field.Snapshot(), *field_plan);
        ASSERT_TRUE(field_preview);
        EXPECT_NE(field_preview->front().source.find("combine(int m, int k)"), std::string::npos);
        EXPECT_NE(field_preview->front().source.find("return combine(s.m, k);"),
                  std::string::npos);
    }

    TEST(ExtractFunctionSpec, ExtractsStatementRangesWithOneLiveOutput)
    {
        Fixture f("int f(int x) {\n"
            "    int a = x + 1;\n"
            "    int b = a * 2;\n"
            "    return b;\n"
            "}");
        const auto snapshot = f.Snapshot();
        const auto source = snapshot.Source(f.id);
        const auto start = source->find("int a");
        const auto end = source->find("return b");
        const auto plan = RefactoringService::ExtractFunction(
            snapshot, f.id, start, end - start, "combine");
        ASSERT_TRUE(plan) << plan.error().message;
        ASSERT_EQ(plan->documents.front().edits.size(), 2u);
        const auto preview = PreviewRefactoring(snapshot, *plan);
        ASSERT_TRUE(preview);
        EXPECT_NE(preview->front().source.find("static constexpr int combine(int x) noexcept"),
                  std::string::npos);
        EXPECT_NE(preview->front().source.find("return b;"), std::string::npos);
        EXPECT_NE(preview->front().source.find("auto b = combine(x);"), std::string::npos);
    }

    TEST(ExtractFunctionSpec, RejectsStatementRangesWithoutSingleOutput)
    {
        // Two live outputs.
        Fixture two("int f(int x) {\n"
            "    int a = x + 1;\n"
            "    int b = a * 2;\n"
            "    return a + b;\n"
            "}");
        {
            const auto snapshot = two.Snapshot();
            const auto source = snapshot.Source(two.id);
            EXPECT_FALSE(RefactoringService::ExtractFunction(snapshot, two.id,
                                                             source->find("int a"), source->find("return a") - source->find("int a"), "combine"));
        }

        // Dead range: nothing used afterwards.
        Fixture dead("int f(int x) {\n"
            "    int a = x + 1;\n"
            "    return x;\n"
            "}");
        {
            const auto snapshot = dead.Snapshot();
            const auto source = snapshot.Source(dead.id);
            EXPECT_FALSE(RefactoringService::ExtractFunction(snapshot, dead.id,
                                                             source->find("int a"), source->find("return x") - source->find("int a"), "combine"));
        }

        // Same spelling, different values (plain read vs member read).
        Fixture conflict("struct S { int x; };\n"
            "int f(S s, int x) {\n"
            "    int a = x + s.x;\n"
            "    return a;\n"
            "}");
        {
            const auto snapshot = conflict.Snapshot();
            const auto source = snapshot.Source(conflict.id);
            EXPECT_FALSE(RefactoringService::ExtractFunction(snapshot, conflict.id,
                                                             source->find("int a"), source->find("return a") - source->find("int a"), "combine"));
        }

        // Control flow inside the range.
        Fixture flow("int f(int x) {\n"
            "    int a = x + 1;\n"
            "    if (a) { return a; }\n"
            "    return 0;\n"
            "}");
        {
            const auto snapshot = flow.Snapshot();
            const auto source = snapshot.Source(flow.id);
            EXPECT_FALSE(RefactoringService::ExtractFunction(snapshot, flow.id,
                                                             source->find("int a"), source->find("return 0") - source->find("int a"), "combine"));
        }

        // Later case label crossed by the moved span.
        Fixture cases("int f(int x) {\n"
            "    switch (x) {\n"
            "    case 0:\n"
            "        int a = 1;\n"
            "        return a;\n"
            "    default:\n"
            "        return 0;\n"
            "    }\n"
            "}");
        {
            const auto snapshot = cases.Snapshot();
            const auto source = snapshot.Source(cases.id);
            EXPECT_FALSE(RefactoringService::ExtractFunction(snapshot, cases.id,
                                                             source->find("int a"), source->find("return a") - source->find("int a"), "combine"));
        }
    }

    TEST(InlineFunctionSpec, InlinesSingleReturnFunctionsWithParenthesizedArguments)
    {
        Fixture f("static int twice(int v) { return v * 2; }\n"
            "int f(int x) { return twice(x) + 1; }");
        const auto snapshot = f.Snapshot();
        const auto plan = RefactoringService::InlineFunction(
            snapshot, f.id, snapshot.Source(f.id)->find("twice(x)"));
        ASSERT_TRUE(plan) << plan.error().message;
        const auto preview = PreviewRefactoring(snapshot, *plan);
        ASSERT_TRUE(preview);
        EXPECT_EQ(preview->front().source,
                  "static int twice(int v) { return v * 2; }\n"
            "int f(int x) { return ((x) * 2) + 1; }");

        Fixture precedence("static int add(int a, int b) { return a + b; }\n"
            "int f(int x) { return add(x, 1) * 2; }");
        const auto precedence_snapshot = precedence.Snapshot();
        const auto precedence_plan = RefactoringService::InlineFunction(
            precedence_snapshot, precedence.id,
            precedence_snapshot.Source(precedence.id)->find("add(x, 1)"));
        ASSERT_TRUE(precedence_plan) << precedence_plan.error().message;
        const auto precedence_preview = PreviewRefactoring(precedence_snapshot, *precedence_plan);
        ASSERT_TRUE(precedence_preview);
        EXPECT_EQ(precedence_preview->front().source,
                  "static int add(int a, int b) { return a + b; }\n"
            "int f(int x) { return ((x) + (1)) * 2; }");
    }

    TEST(InlineFunctionSpec, RejectsImpureBodiesGlobalsArityAndRecursion)
    {
        for (const auto source :
            {
                "static int f(int x) { int t = x; return t; }\nint g() { return f(1); }",
                "int g;\nstatic int f() { return g; }\nint h() { return f(); }",
                "static int f(int x) { return x; }\nint g() { return f(); }",
                "static int f(int x) { return x; }\nint g() { return f(1, 2); }",
                "static int f(int n) { return n + f(n - 1); }\n"
                "int g() { return f(3); }",
                "static int f(int x) { return x; }\nint g(int y) { return f(y++); }",
                "struct S { int m() { return 1; } };\nint g(S s) { return s.m(); }"
        })
        {
            Fixture f(source);
            const auto snapshot = f.Snapshot();
            const auto call = snapshot.Source(f.id)->rfind('(');
            EXPECT_FALSE(RefactoringService::InlineFunction(snapshot, f.id, call))
            << source;
        }
    }

    TEST(ExtractMethodSpec, ExtractsInstanceMethodKeepingMemberReadsDirect)
    {
        Fixture f("struct S { int m; int get() { return m + 1; } };");
        const auto plan =
            RefactoringService::ExtractMethod(f.Snapshot(), f.id, f.At("m + 1"), 5, "combined");
        ASSERT_TRUE(plan) << plan.error().message;
        ASSERT_EQ(plan->documents.front().edits.size(), 2u);
        const auto preview = PreviewRefactoring(f.Snapshot(), *plan);
        ASSERT_TRUE(preview);
        EXPECT_NE(preview->front().source.find("int combined() const { return m + 1; }"),
                  std::string::npos);
        EXPECT_NE(preview->front().source.find("return this->combined();"), std::string::npos);
    }

    TEST(ExtractMethodSpec, ExtractsStaticMethodForMemberFreeExpressions)
    {
        Fixture f("struct S { int get(int x) { return x * 2; } };");
        const auto plan =
            RefactoringService::ExtractMethod(f.Snapshot(), f.id, f.At("x * 2"), 5, "doubled");
        ASSERT_TRUE(plan) << plan.error().message;
        const auto preview = PreviewRefactoring(f.Snapshot(), *plan);
        ASSERT_TRUE(preview);
        EXPECT_NE(preview->front().source.find("static int doubled(int x) { return x * 2; }"),
                  std::string::npos);
        EXPECT_NE(preview->front().source.find("return doubled(x);"), std::string::npos);
    }

    TEST(ExtractMethodSpec, RejectsTemplatesCollisionsAndFreeFunctions)
    {
        Fixture collision("struct S { int m; int combined; int get() { return m + 1; } };");
        const auto colliding = RefactoringService::ExtractMethod(
            collision.Snapshot(), collision.id, collision.At("m + 1"), 5, "combined");
        ASSERT_FALSE(colliding);
        EXPECT_EQ(colliding.error().code, RefactoringErrorCode::NameCollision);

        Fixture templated("template<class T> struct S { T m; T get() { return m; } };");
        const auto templated_snapshot = templated.Snapshot();
        const auto templated_plan = RefactoringService::ExtractMethod(templated_snapshot,
            templated.id, templated_snapshot.Source(templated.id)->find("return m") + 7, 1,
            "combined");
        ASSERT_FALSE(templated_plan);

        Fixture free("int f(int x) { return x + 1; }");
        const auto free_plan = RefactoringService::ExtractMethod(
            free.Snapshot(), free.id, free.At("x + 1"), 5, "combined");
        ASSERT_FALSE(free_plan);
    }
} // namespace
