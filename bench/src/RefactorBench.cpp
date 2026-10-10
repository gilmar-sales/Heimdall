// Benchmarks for the refactoring subsystem (architecture review, roadmap items 1-4):
// end-to-end rename/extract/inline latency, reference lookup isolation, preview
// application cost, and rejection-path latency (lambda/template/macro early-outs).
// The corpus is synthetic: one target function holding the refactored entity
// plus N background functions that grow tokens/symbols without touching it.
// A function-local refactoring must therefore scale with the function, not the file.
//
// Phase-0 budgets (x64, GCC 16, -Og dev build; machine-specific, compare relative
// against build/RefactorBench_baseline.json, tolerance 10%):
//   RenameLocal (warm snapshot) ......... P95 < 50 ms @ 5k-line TU
//   LocalReferences ..................... flat vs file size (~us)
//   PreviewRefactoring .................. < 0.1 ms @ 800 edits
//   Extract/Inline/ExtractFunction ...... tracked, must not regress (validation
//                                         still re-parses + re-binds; differential
//                                         validation is a later phase)
//   Rejections (lambda/template/macro) .. < 1 ms (early-out, no revalidation)
// Run: RefactorBench --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
#include <benchmark/benchmark.h>

#include <Heimdall/Refactoring.hpp>
#include <Heimdall/Workspace.hpp>

#include <memory>
#include <string>

namespace
{
    using namespace heimdall;

    std::string TargetFunction(int uses)
    {
        std::string body = "int target(int target_param)\n{\n    int targetVar = target_param + 1;\n";
        for (int i = 0; i < uses; ++i)
        {
            body += "    targetVar += target_param + " + std::to_string(i) + ";\n";
        }

        body += "    return targetVar;\n}\n";
        return body;
    }

    std::string BackgroundFunction(int index)
    {
        const std::string n = std::to_string(index);
        return "int background" + n + "(int param" + n + ")\n{\n    int helper" + n +
               " = param" + n + " + " + n + ";\n    int local" + n + " = helper" + n +
               " * 2;\n    return local" + n + " + helper" + n + ";\n}\n";
    }

    std::string Corpus(int background, int uses)
    {
        std::string source = TargetFunction(uses);
        for (int i = 0; i < background; ++i)
        {
            source += BackgroundFunction(i);
        }

        return source;
    }

    struct Fixture
    {
        Workspace                          workspace;
        DocumentId                         id;
        std::shared_ptr<const std::string> source;
        std::size_t                        offset = 0;

        explicit Fixture(std::string text)
            : source(std::make_shared<const std::string>(std::move(text)))
        {
            id     = *workspace.Open("rename.cpp", source, 1);
            offset = source->find("targetVar");
        }

        AnalysisSnapshot Snapshot()
        {
            return workspace.Snapshot();
        }
    };

    // Full RenameLocal: lookup + collision check + preview + reparse/rebind
    // validation. Measures what the user waits for on a rename request.
    void BM_RenameLocal(benchmark::State& state)
    {
        Fixture fixture(Corpus(static_cast<int>(state.range(0)), 20));
        auto    snapshot = fixture.Snapshot();
        // Warm the snapshot caches so every iteration measures the rename
        // itself, not the first parse/bind of the document.
        if (!RefactoringService::RenameLocal(snapshot, fixture.id, fixture.offset, "renamedValue"))
        {
            state.SkipWithError("rename failed on warmup");
            return;
        }

        std::size_t edits = 0;
        for (auto _ : state)
        {
            auto plan = RefactoringService::RenameLocal(
                snapshot, fixture.id, fixture.offset, "renamedValue");
            if (!plan)
            {
                state.SkipWithError(plan.error().message.c_str());
                return;
            }

            edits = plan->documents[0].edits.size();
            benchmark::DoNotOptimize(plan);
        }

        const auto syntax = snapshot.Syntax(fixture.id);
        state.counters["edits"]   = static_cast<double>(edits);
        state.counters["tokens"]  = static_cast<double>(syntax->Tokens().size());
        state.counters["nodes"]   = static_cast<double>(syntax->NodesSoA().size());
        state.counters["symbols"] = static_cast<double>(snapshot.Semantic(fixture.id)->Symbols().Size());
        state.SetBytesProcessed(
            static_cast<std::int64_t>(state.iterations() * fixture.source->size()));
    }

    // Lookup only: isolates References() from validation. It already scans
    // just the enclosing function, so it should stay flat as background grows.
    void BM_LocalReferences(benchmark::State& state)
    {
        Fixture fixture(Corpus(static_cast<int>(state.range(0)), 20));
        auto    snapshot = fixture.Snapshot();

        std::size_t tokens = 0;
        for (auto _ : state)
        {
            auto refs = RefactoringService::LocalReferences(
                snapshot, fixture.id, fixture.offset, true);
            if (!refs)
            {
                state.SkipWithError(refs.error().message.c_str());
                return;
            }

            tokens = refs->tokens.size();
            benchmark::DoNotOptimize(refs);
        }

        state.counters["occurrences"] = static_cast<double>(tokens);
    }

    // Preview application only: E single-character edits spread over a large
    // file. The baseline applies them with one std::string::replace per edit
    // (O(file) memmove each); the target is a single reserved pass.
    void BM_PreviewApply(benchmark::State& state)
    {
        const int   edits_wanted = static_cast<int>(state.range(0));
        Fixture     fixture(Corpus(2000, 0));
        auto        snapshot = fixture.Snapshot();
        const auto& source   = *fixture.source;

        RefactoringPlan plan { "bench", snapshot.Revision(), {} };
        DocumentEdits   edits { fixture.id, snapshot.Path(fixture.id),
                                snapshot.Version(fixture.id), fixture.source, {}, {} };
        for (std::size_t pos = source.find("return "), count = 0;
             pos != std::string::npos && count < static_cast<std::size_t>(edits_wanted);
             pos = source.find("return ", pos + 1), ++count)
        {
            // Shortening replacement: every applied edit shifts the whole tail,
            // which is O(file) per edit with a naive replace-in-loop apply.
            edits.edits.push_back({ pos, 6, "return", "ret" });
        }

        if (edits.edits.empty())
        {
            state.SkipWithError("no edit sites found");
            return;
        }

        plan.documents.push_back(std::move(edits));
        for (auto _ : state)
        {
            auto preview = PreviewRefactoring(snapshot, plan);
            if (!preview)
            {
                state.SkipWithError(preview.error().message.c_str());
                return;
            }

            benchmark::DoNotOptimize(preview);
        }

        state.counters["edits"] = static_cast<double>(plan.documents[0].edits.size());
        state.SetBytesProcessed(
            static_cast<std::int64_t>(state.iterations() * fixture.source->size()));
    }

    // Extract/inline corpora mirror the smallest supported subsets (see
    // RefactoringSpec): a pure scalar return, a same-type literal init with
    // standalone-return reads, and a scalar literal return in a free function.
    // The target stays first so find() hits it even when background literals
    // contain the same digits.
    std::string ExtractCorpus(int background)
    {
        std::string source = "int extract_target() { return 1 + 2; }\n";
        for (int i = 0; i < background; ++i)
        {
            source += BackgroundFunction(i);
        }

        return source;
    }

    std::string InlineCorpus(int background)
    {
        std::string source =
            "int inline_target(int input) { int value = 42; if (input) { return value; } "
            "return value; }\n";
        for (int i = 0; i < background; ++i)
        {
            source += BackgroundFunction(i);
        }

        return source;
    }

    std::string ExtractFunctionCorpus(int background)
    {
        std::string source = "constexpr int function_target() { return 42; }\n";
        for (int i = 0; i < background; ++i)
        {
            source += BackgroundFunction(i);
        }

        return source;
    }

    std::string MemberCorpus(int background, int uses)
    {
        std::string source = "struct Item { int price; int weight; };\n"
                             "int target(Item item) {\n"
                             "    int total = item.price;\n";
        for (int i = 0; i < uses; ++i)
        {
            source += "    total += item.price + " + std::to_string(i) + ";\n";
        }

        source += "    return total;\n}\n";
        for (int i = 0; i < background; ++i)
        {
            source += BackgroundFunction(i);
        }

        return source;
    }

    void BM_ExtractVariable(benchmark::State& state)
    {
        Fixture fixture(ExtractCorpus(static_cast<int>(state.range(0))));
        auto    snapshot = fixture.Snapshot();
        const auto offset = fixture.source->find("1 + 2");
        if (offset == std::string::npos)
        {
            state.SkipWithError("extract site not found");
            return;
        }

        if (!RefactoringService::ExtractVariable(snapshot, fixture.id, offset, 5, "result"))
        {
            state.SkipWithError("extract failed on warmup");
            return;
        }

        std::size_t edits = 0;
        for (auto _ : state)
        {
            auto plan =
                RefactoringService::ExtractVariable(snapshot, fixture.id, offset, 5, "result");
            if (!plan)
            {
                state.SkipWithError(plan.error().message.c_str());
                return;
            }

            edits = plan->documents[0].edits.size();
            benchmark::DoNotOptimize(plan);
        }

        state.counters["edits"] = static_cast<double>(edits);
        state.SetBytesProcessed(
            static_cast<std::int64_t>(state.iterations() * fixture.source->size()));
    }

    void BM_InlineVariable(benchmark::State& state)
    {
        Fixture fixture(InlineCorpus(static_cast<int>(state.range(0))));
        auto    snapshot = fixture.Snapshot();
        const auto offset = fixture.source->find("value");
        if (offset == std::string::npos)
        {
            state.SkipWithError("inline site not found");
            return;
        }

        if (!RefactoringService::InlineVariable(snapshot, fixture.id, offset))
        {
            state.SkipWithError("inline failed on warmup");
            return;
        }

        std::size_t edits = 0;
        for (auto _ : state)
        {
            auto plan = RefactoringService::InlineVariable(snapshot, fixture.id, offset);
            if (!plan)
            {
                state.SkipWithError(plan.error().message.c_str());
                return;
            }

            edits = plan->documents[0].edits.size();
            benchmark::DoNotOptimize(plan);
        }

        state.counters["edits"] = static_cast<double>(edits);
        state.SetBytesProcessed(
            static_cast<std::int64_t>(state.iterations() * fixture.source->size()));
    }

    void BM_ExtractFunction(benchmark::State& state)
    {
        Fixture fixture(ExtractFunctionCorpus(static_cast<int>(state.range(0))));
        auto    snapshot = fixture.Snapshot();
        const auto found  = fixture.source->find("return 42");
        if (found == std::string::npos)
        {
            state.SkipWithError("extract-function site not found");
            return;
        }

        const auto offset = found + 7;

        if (!RefactoringService::ExtractFunction(snapshot, fixture.id, offset, 2, "answer"))
        {
            state.SkipWithError("extract-function failed on warmup");
            return;
        }

        std::size_t edits = 0;
        for (auto _ : state)
        {
            auto plan =
                RefactoringService::ExtractFunction(snapshot, fixture.id, offset, 2, "answer");
            if (!plan)
            {
                state.SkipWithError(plan.error().message.c_str());
                return;
            }

            edits = plan->documents[0].edits.size();
            benchmark::DoNotOptimize(plan);
        }

        state.counters["edits"] = static_cast<double>(edits);
        state.SetBytesProcessed(
            static_cast<std::int64_t>(state.iterations() * fixture.source->size()));
    }

    // Fields resolve through the typer, so a member rename also pays a
    // retype per validation; background functions grow parse/bind/type.
    void BM_RenameMember(benchmark::State& state)
    {
        Fixture fixture(MemberCorpus(static_cast<int>(state.range(0)), 20));
        auto    snapshot = fixture.Snapshot();
        const auto offset = fixture.source->find("price");
        if (offset == std::string::npos)
        {
            state.SkipWithError("member site not found");
            return;
        }

        if (!RefactoringService::RenameLocal(snapshot, fixture.id, offset, "cost"))
        {
            state.SkipWithError("member rename failed on warmup");
            return;
        }

        std::size_t edits = 0;
        for (auto _ : state)
        {
            auto plan =
                RefactoringService::RenameLocal(snapshot, fixture.id, offset, "cost");
            if (!plan)
            {
                state.SkipWithError(plan.error().message.c_str());
                return;
            }

            edits = plan->documents[0].edits.size();
            benchmark::DoNotOptimize(plan);
        }

        state.counters["edits"] = static_cast<double>(edits);
        state.SetBytesProcessed(
            static_cast<std::int64_t>(state.iterations() * fixture.source->size()));
    }

    // Cross-file rename: definition + caller with a redeclaration, plus B
    // background documents that grow the snapshot and the symbol index. No
    // directives anywhere, so no compiler spawns disturb the measurement.
    void BM_RenameCrossFile(benchmark::State& state)
    {
        Workspace workspace;
        auto      impl = workspace.Open("impl.cpp",
             std::make_shared<const std::string>("int cross_add(int a, int b) { return a + b; }"),
             1);
        auto      caller = workspace.Open("caller.cpp",
             std::make_shared<const std::string>(
                 "int cross_add(int a, int b);\nint run() { return cross_add(1, 2); }"),
             1);
        if (!impl || !caller)
        {
            state.SkipWithError("workspace open failed");
            return;
        }

        for (int i = 0; i < static_cast<int>(state.range(0)); ++i)
        {
            std::string background;
            for (int f = 0; f < 20; ++f)
            {
                background += BackgroundFunction(i * 20 + f);
            }

            if (!workspace.Open("background" + std::to_string(i) + ".cpp",
                    std::make_shared<const std::string>(std::move(background)), 1))
            {
                state.SkipWithError("background open failed");
                return;
            }
        }

        auto       snapshot = workspace.Snapshot();
        const auto source   = snapshot.Source(*impl);
        const auto offset   = source->find("cross_add");
        if (!RefactoringService::RenameLocal(snapshot, *impl, offset, "cross_sum"))
        {
            state.SkipWithError("cross-file rename failed on warmup");
            return;
        }

        std::size_t documents = 0;
        std::size_t edits     = 0;
        for (auto _ : state)
        {
            auto plan = RefactoringService::RenameLocal(snapshot, *impl, offset, "cross_sum");
            if (!plan)
            {
                state.SkipWithError(plan.error().message.c_str());
                return;
            }

            documents = plan->documents.size();
            edits     = 0;
            for (const auto& renamed : plan->documents)
            {
                edits += renamed.edits.size();
            }

            benchmark::DoNotOptimize(plan);
        }

        state.counters["documents"] = static_cast<double>(documents);
        state.counters["edits"]     = static_cast<double>(edits);
    }

    void BM_InlineFunction(benchmark::State& state)
    {
        std::string source =
            "static int twice(int v) { return v * 2; }\n"
            "int inline_target(int x) { return twice(x) + 1; }\n";
        for (int i = 0; i < static_cast<int>(state.range(0)); ++i)
        {
            source += BackgroundFunction(i);
        }

        Fixture     fixture(std::move(source));
        auto        snapshot = fixture.Snapshot();
        const auto  offset   = fixture.source->find("twice(x)");
        if (offset == std::string::npos)
        {
            state.SkipWithError("inline site not found");
            return;
        }

        if (!RefactoringService::InlineFunction(snapshot, fixture.id, offset))
        {
            state.SkipWithError("inline failed on warmup");
            return;
        }

        for (auto _ : state)
        {
            auto plan = RefactoringService::InlineFunction(snapshot, fixture.id, offset);
            if (!plan)
            {
                state.SkipWithError(plan.error().message.c_str());
                return;
            }

            benchmark::DoNotOptimize(plan);
        }

        state.SetBytesProcessed(
            static_cast<std::int64_t>(state.iterations() * fixture.source->size()));
    }

    void BM_ExtractStatements(benchmark::State& state)
    {
        std::string source = "int statements_target(int x) {\n"
                             "    int a = x + 1;\n"
                             "    int b = a * 2;\n"
                             "    return b;\n"
                             "}\n";
        for (int i = 0; i < static_cast<int>(state.range(0)); ++i)
        {
            source += BackgroundFunction(i);
        }

        Fixture    fixture(std::move(source));
        auto       snapshot = fixture.Snapshot();
        const auto start    = fixture.source->find("int a");
        const auto end      = fixture.source->find("return b");
        if (start == std::string::npos || end == std::string::npos || end <= start)
        {
            state.SkipWithError("statement range not found");
            return;
        }

        if (!RefactoringService::ExtractFunction(snapshot, fixture.id, start, end - start, "combine"))
        {
            state.SkipWithError("statement extract failed on warmup");
            return;
        }

        for (auto _ : state)
        {
            auto plan = RefactoringService::ExtractFunction(
                snapshot, fixture.id, start, end - start, "combine");
            if (!plan)
            {
                state.SkipWithError(plan.error().message.c_str());
                return;
            }

            benchmark::DoNotOptimize(plan);
        }

        state.SetBytesProcessed(
            static_cast<std::int64_t>(state.iterations() * fixture.source->size()));
    }

    void BM_ExtractConstant(benchmark::State& state)
    {
        std::string source = "int constant_target(int x) { return x + 42 + 42; }\n";
        for (int i = 0; i < static_cast<int>(state.range(0)); ++i)
        {
            source += BackgroundFunction(i);
        }

        Fixture    fixture(std::move(source));
        auto       snapshot = fixture.Snapshot();
        const auto offset   = fixture.source->find("42");
        if (offset == std::string::npos)
        {
            state.SkipWithError("constant site not found");
            return;
        }

        if (!RefactoringService::ExtractConstant(snapshot, fixture.id, offset, 2, "answer"))
        {
            state.SkipWithError("constant extract failed on warmup");
            return;
        }

        for (auto _ : state)
        {
            auto plan =
                RefactoringService::ExtractConstant(snapshot, fixture.id, offset, 2, "answer");
            if (!plan)
            {
                state.SkipWithError(plan.error().message.c_str());
                return;
            }

            benchmark::DoNotOptimize(plan);
        }

        state.SetBytesProcessed(
            static_cast<std::int64_t>(state.iterations() * fixture.source->size()));
    }

    void BM_ExtractMethod(benchmark::State& state)
    {
        std::string source =
            "struct Widget { int value; int get() { return value + 1; } };\n";
        for (int i = 0; i < static_cast<int>(state.range(0)); ++i)
        {
            source += BackgroundFunction(i);
        }

        Fixture    fixture(std::move(source));
        auto       snapshot = fixture.Snapshot();
        const auto offset   = fixture.source->find("value + 1");
        if (offset == std::string::npos)
        {
            state.SkipWithError("method site not found");
            return;
        }

        if (!RefactoringService::ExtractMethod(snapshot, fixture.id, offset, 9, "combined"))
        {
            state.SkipWithError("method extract failed on warmup");
            return;
        }

        for (auto _ : state)
        {
            auto plan = RefactoringService::ExtractMethod(
                snapshot, fixture.id, offset, 9, "combined");
            if (!plan)
            {
                state.SkipWithError(plan.error().message.c_str());
                return;
            }

            benchmark::DoNotOptimize(plan);
        }

        state.SetBytesProcessed(
            static_cast<std::int64_t>(state.iterations() * fixture.source->size()));
    }

    void BM_AddParameter(benchmark::State& state)
    {
        Workspace workspace;
        auto      impl = workspace.Open("impl.cpp",
             std::make_shared<const std::string>("int bench_add(int a, int b) { return a + b; }"),
             1);
        auto      caller = workspace.Open("caller.cpp",
             std::make_shared<const std::string>(
                 "int bench_add(int a, int b);\nint run() { return bench_add(1, 2); }"),
             1);
        if (!impl || !caller)
        {
            state.SkipWithError("workspace open failed");
            return;
        }

        for (int i = 0; i < static_cast<int>(state.range(0)); ++i)
        {
            std::string background;
            for (int f = 0; f < 20; ++f)
            {
                background += BackgroundFunction(i * 20 + f);
            }

            if (!workspace.Open("background" + std::to_string(i) + ".cpp",
                    std::make_shared<const std::string>(std::move(background)), 1))
            {
                state.SkipWithError("background open failed");
                return;
            }
        }

        auto       snapshot = workspace.Snapshot();
        const auto source   = snapshot.Source(*impl);
        const auto offset   = source->find("bench_add(int");
        if (!RefactoringService::AddParameter(snapshot, *impl, offset, "int", "c", "0"))
        {
            state.SkipWithError("add-parameter failed on warmup");
            return;
        }

        std::size_t documents = 0;
        for (auto _ : state)
        {
            auto plan =
                RefactoringService::AddParameter(snapshot, *impl, offset, "int", "c", "0");
            if (!plan)
            {
                state.SkipWithError(plan.error().message.c_str());
                return;
            }

            documents = plan->documents.size();
            benchmark::DoNotOptimize(plan);
        }

        state.counters["documents"] = static_cast<double>(documents);
    }

    void BM_MoveToNamespace(benchmark::State& state)
    {
        std::string source =
            "namespace destination { int helper() { return 1; } }\n"
            "int move_target(int a, int b) { return a + b; }\n"
            "int run() { return move_target(1, 2); }\n";
        for (int i = 0; i < static_cast<int>(state.range(0)); ++i)
        {
            source += BackgroundFunction(i);
        }

        Fixture    fixture(std::move(source));
        auto       snapshot = fixture.Snapshot();
        const auto offset   = fixture.source->find("move_target(int");
        if (offset == std::string::npos)
        {
            state.SkipWithError("move site not found");
            return;
        }

        if (!RefactoringService::MoveToNamespace(snapshot, fixture.id, offset, "destination"))
        {
            state.SkipWithError("move failed on warmup");
            return;
        }

        for (auto _ : state)
        {
            auto plan = RefactoringService::MoveToNamespace(
                snapshot, fixture.id, offset, "destination");
            if (!plan)
            {
                state.SkipWithError(plan.error().message.c_str());
                return;
            }

            benchmark::DoNotOptimize(plan);
        }

        state.SetBytesProcessed(
            static_cast<std::int64_t>(state.iterations() * fixture.source->size()));
    }

    // Rejection paths must stay cheap early-outs: no preview, no revalidation.
    // If any of these ever succeeds, the supported subset changed and the bench
    // (plus RefactoringSpec) needs updating — hence the loud skip.
    void BM_RenameRejectsLambda(benchmark::State& state)
    {
        Fixture fixture("int f() { int candidate = 1; auto g = [candidate]() { return "
                        "candidate + 1; }; return g(); }\n");
        auto    snapshot = fixture.Snapshot();
        const auto offset = fixture.source->find("candidate");
        for (auto _ : state)
        {
            auto plan =
                RefactoringService::RenameLocal(snapshot, fixture.id, offset, "renamed");
            if (plan)
            {
                state.SkipWithError("lambda rename unexpectedly accepted");
                return;
            }

            benchmark::DoNotOptimize(plan.error().code);
        }
    }

    void BM_RenameRejectsTemplate(benchmark::State& state)
    {
        Fixture fixture("template<class T> T f(T candidate) { return candidate; }\n");
        auto    snapshot = fixture.Snapshot();
        const auto offset = fixture.source->find("candidate");
        for (auto _ : state)
        {
            auto plan =
                RefactoringService::RenameLocal(snapshot, fixture.id, offset, "renamed");
            if (plan)
            {
                state.SkipWithError("template rename unexpectedly accepted");
                return;
            }

            benchmark::DoNotOptimize(plan.error().code);
        }
    }

    void BM_ExtractRejectsMacro(benchmark::State& state)
    {
        Fixture fixture("#define LIMIT 100\nint f() { return 1 + 2; }\n");
        auto    snapshot = fixture.Snapshot();
        const auto offset = fixture.source->find("1 + 2");
        for (auto _ : state)
        {
            auto plan =
                RefactoringService::ExtractVariable(snapshot, fixture.id, offset, 5, "result");
            if (plan)
            {
                state.SkipWithError("macro extract unexpectedly accepted");
                return;
            }

            benchmark::DoNotOptimize(plan.error().code);
        }
    }

    BENCHMARK(BM_RenameLocal)->Arg(10)->Arg(100)->Arg(500);

    BENCHMARK(BM_LocalReferences)->Arg(10)->Arg(100)->Arg(500);

    BENCHMARK(BM_PreviewApply)->Arg(50)->Arg(200)->Arg(800);

    BENCHMARK(BM_ExtractVariable)->Arg(10)->Arg(100)->Arg(500);

    BENCHMARK(BM_InlineVariable)->Arg(10)->Arg(100)->Arg(500);

    BENCHMARK(BM_ExtractFunction)->Arg(10)->Arg(100)->Arg(500);

    BENCHMARK(BM_RenameMember)->Arg(10)->Arg(100)->Arg(500);

    BENCHMARK(BM_RenameCrossFile)->Arg(1)->Arg(10)->Arg(50);

    BENCHMARK(BM_ExtractConstant)->Arg(10)->Arg(100)->Arg(500);

    BENCHMARK(BM_ExtractMethod)->Arg(10)->Arg(100)->Arg(500);

    BENCHMARK(BM_AddParameter)->Arg(1)->Arg(10)->Arg(50);

    BENCHMARK(BM_MoveToNamespace)->Arg(10)->Arg(100)->Arg(500);

    BENCHMARK(BM_InlineFunction)->Arg(10)->Arg(100)->Arg(500);

    BENCHMARK(BM_ExtractStatements)->Arg(10)->Arg(100)->Arg(500);

    BENCHMARK(BM_RenameRejectsLambda);

    BENCHMARK(BM_RenameRejectsTemplate);

    BENCHMARK(BM_ExtractRejectsMacro);

} // namespace
