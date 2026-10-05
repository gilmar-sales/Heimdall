#include <Heimdall/AnalysisEngine.hpp>

#include <benchmark/benchmark.h>

namespace
{
    using namespace heimdall;
    std::shared_ptr<const std::string> Source(int functions, int value = 1)
    {
        std::string source;
        for (int i = 0; i < functions; ++i)
            source += "int function" + std::to_string(i) + "() { return " + std::to_string(i ? i : value) + "; }\n";
        return std::make_shared<const std::string>(std::move(source));
    }

    void SnapshotPin(benchmark::State &state)
    {
        Workspace workspace;
        for (int i = 0; i < state.range(0); ++i)
            (void)workspace.Open("pin" + std::to_string(i) + ".cpp", Source(1));
        for (auto _ : state) benchmark::DoNotOptimize(workspace.Snapshot());
    }
    BENCHMARK(SnapshotPin)->Arg(1)->Arg(100)->Arg(1000);

    void WarmSemanticCache(benchmark::State &state)
    {
        Workspace workspace;
        auto id = workspace.Open("cache.cpp", Source(static_cast<int>(state.range(0))));
        auto snapshot = workspace.Snapshot();
        benchmark::DoNotOptimize(snapshot.Types(*id));
        for (auto _ : state) benchmark::DoNotOptimize(snapshot.Types(*id));
        const auto metrics = snapshot.Metrics();
        state.counters["parses"] = static_cast<double>(metrics.parse_count);
        state.counters["binds"] = static_cast<double>(metrics.bind_count);
        state.counters["types"] = static_cast<double>(metrics.type_count);
        state.counters["cache_hits"] = static_cast<double>(metrics.cache_hits);
        state.counters["semantic_allocations"] = static_cast<double>(metrics.semantic_allocations);
        state.counters["type_allocations"] = static_cast<double>(metrics.type_allocations);
        state.counters["estimated_bytes"] = static_cast<double>(snapshot.Memory().Total());
    }
    BENCHMARK(WarmSemanticCache)->Arg(10)->Arg(1000);

    void FullParse(benchmark::State &state)
    {
        auto source = Source(static_cast<int>(state.range(0)));
        for (auto _ : state) benchmark::DoNotOptimize(ParseTree::ParseSnapshot(source, {}));
    }
    BENCHMARK(FullParse)->Arg(10)->Arg(1000);

    void IncrementalUpdate(benchmark::State &state)
    {
        Workspace workspace;
        auto first = Source(static_cast<int>(state.range(0)), 1);
        auto second = Source(static_cast<int>(state.range(0)), 2);
        auto id = workspace.Open("edit.cpp", first);
        benchmark::DoNotOptimize(workspace.Snapshot().Syntax(*id));
        std::int64_t version = 0;
        std::size_t reused = 0;
        for (auto _ : state)
        {
            ++version;
            (void)workspace.Update(*id, version % 2 ? second : first, version);
            auto tree = workspace.Snapshot().Syntax(*id);
            reused = tree->ReusedItems();
            benchmark::DoNotOptimize(tree);
        }
        state.counters["reused_items"] = static_cast<double>(reused);
    }
    BENCHMARK(IncrementalUpdate)->Arg(10)->Arg(1000);

    void ScheduledNativeRules(benchmark::State &state)
    {
        Workspace workspace;
        auto id = workspace.Open("rules.cpp", Source(100));
        AnalysisContext context(workspace.Snapshot(), *id);
        RuleScheduler scheduler;
        for (int i = 0; i < state.range(0); ++i)
        {
            ScheduledRule rule;
            rule.metadata.code = "bench/" + std::to_string(i);
            rule.interests = {GrammarKind::ReturnStatement};
            rule.on_node = [](const AnalysisContext &ctx, NodeId node, DiagnosticSink &)
            { benchmark::DoNotOptimize(ctx.Syntax().NodesSoA().FirstToken(node)); };
            (void)scheduler.Register(std::move(rule));
        }
        RuleEngine engine;
        for (auto _ : state) benchmark::DoNotOptimize(scheduler.Analyze(context, engine));
    }
    BENCHMARK(ScheduledNativeRules)->Arg(1)->Arg(10)->Arg(100);
}
