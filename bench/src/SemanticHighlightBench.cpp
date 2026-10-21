#include <cstdint>
#include <string>

#include <benchmark/benchmark.h>

#include <Heimdall/SemanticHighlight.hpp>

namespace
{
    std::string Corpus()
    {
        std::string source = "struct Item { int value; };\n";
        constexpr int kFunctions = 100;
        for (int index = 0; index < kFunctions; ++index)
        {
            source += "int Function" + std::to_string(index) +
                "(Item object, int value) { int sum = value; "
                "for (int i = 0; i < 10; ++i) { sum += i; } return object.value + sum; }\n";
        }

        return source;
    }

    void LexicalHighlight(benchmark::State& state)
    {
        const auto source = Corpus();
        const auto tree = heimdall::ParseTree::Parse(source);
        for (auto iteration : state)
        {
            const auto spans = heimdall::SyntaxHighlight::Extract(tree);
            benchmark::DoNotOptimize(spans.data());
        }

        state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(source.size()));
    }

    void SemanticHighlight(benchmark::State& state)
    {
        const auto source = Corpus();
        const auto tree = heimdall::ParseTree::Parse(source);
        const auto model = heimdall::Binder::Bind(tree);
        const auto lexical = heimdall::SyntaxHighlight::Extract(tree);
        for (auto iteration : state)
        {
            const auto spans = heimdall::SemanticHighlight::Extract(model, lexical);
            benchmark::DoNotOptimize(spans.data());
        }

        state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(source.size()));
    }

    void HeaderHighlightMetadata(benchmark::State& state)
    {
        const auto source = Corpus();
        const auto tree = heimdall::ParseTree::Parse(source);
        const auto model = heimdall::Binder::Bind(tree);
        for (auto iteration : state)
        {
            const auto index = heimdall::SemanticHighlightIndex::FromModel(model);
            benchmark::DoNotOptimize(index.Find("Item::value"));
        }

        state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(source.size()));
    }
}

BENCHMARK(LexicalHighlight);

BENCHMARK(SemanticHighlight);

BENCHMARK(HeaderHighlightMetadata);
