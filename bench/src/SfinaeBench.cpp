#include <cstdint>
#include <string>

#include <benchmark/benchmark.h>

#include <Heimdall/TypeModel.hpp>

namespace
{
    [[nodiscard]] std::string CandidateSet(std::int64_t count)
    {
        std::string source;
        for (std::int64_t i = 0; i < count; ++i)
        {
            source += "template<class T> typename T::Member" + std::to_string(i) + " Probe(T);\n";
        }

        source += "long Probe(...); auto result = Probe(1);\n";
        return source;
    }

    void BM_SfinaeCandidateSubstitution(benchmark::State& state)
    {
        const auto source = CandidateSet(state.range(0));
        const auto tree = heimdall::ParseTree::Parse(source);
        const auto model = heimdall::Binder::Bind(tree);
        for (auto iteration : state)
        {
            auto types = heimdall::Typer::Type(model);
            benchmark::DoNotOptimize(types.TemplateCalls().data());
        }

        state.SetItemsProcessed(state.iterations() * state.range(0));
    }

    void BM_SfinaeSnapshotQuery(benchmark::State& state)
    {
        const auto source = CandidateSet(state.range(0));
        const auto tree = heimdall::ParseTree::Parse(source);
        const auto model = heimdall::Binder::Bind(tree);
        const auto types = heimdall::Typer::Type(model);
        if (types.TemplateCalls().size() != 1)
        {
            state.SkipWithError("missing template call result");
            return;
        }

        const auto node = types.TemplateCalls().front().node;
        for (auto iteration : state)
        {
            auto result = types.TemplateCall(node);
            benchmark::DoNotOptimize(result);
        }
    }
}

BENCHMARK(BM_SfinaeCandidateSubstitution)->Arg(1)->Arg(8)->Arg(32)->Arg(64);

BENCHMARK(BM_SfinaeSnapshotQuery)->Arg(1)->Arg(8)->Arg(32)->Arg(64);
