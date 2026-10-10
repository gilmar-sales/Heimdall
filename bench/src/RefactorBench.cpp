// Benchmarks for the refactoring subsystem (architecture review, roadmap items 1-4):
// end-to-end rename latency, reference lookup isolation, and preview application
// cost. The corpus is synthetic: one target function holding the renamed local
// plus N background functions that grow tokens/symbols without touching it.
// A function-local rename must therefore scale with the function, not the file.
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

    BENCHMARK(BM_RenameLocal)->Arg(10)->Arg(100)->Arg(500);

    BENCHMARK(BM_LocalReferences)->Arg(10)->Arg(100)->Arg(500);

    BENCHMARK(BM_PreviewApply)->Arg(50)->Arg(200)->Arg(800);

} // namespace
