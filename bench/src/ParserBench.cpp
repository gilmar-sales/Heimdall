// Benchmarks for ParseTree/GrammarParser (review section 5, item 1):
// full lex + preprocess + grammar pass over the bench corpus, plus a
// dedicated incomplete-code input with an unclosed `(` (F5 recovery path).
#include <benchmark/benchmark.h>

#include <Heimdall/MappedBuffer.hpp>
#include <Heimdall/ParseTree.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace
{

    std::vector<std::string> LoadCorpus(benchmark::State& state, std::size_t& corpus_bytes)
    {
        std::vector<std::string> sources;
        corpus_bytes = 0;
        for (const auto& entry : std::filesystem::directory_iterator(HEIMDALL_CORPUS_DIR))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            auto buffer = heimdall::MappedBuffer::Open(entry.path().string());
            if (!buffer)
            {
                state.SkipWithError(buffer.error().c_str());
                return {};
            }

            sources.emplace_back(buffer->view());
            corpus_bytes += buffer->size();
        }

        if (sources.empty())
        {
            state.SkipWithError("empty corpus");
        }

        return sources;
    }

    void BM_Parse(benchmark::State& state)
    {
        std::size_t corpus_bytes = 0;
        const auto sources = LoadCorpus(state, corpus_bytes);
        if (sources.empty())
        {
            return;
        }

        std::size_t node_count = 0;
        for (auto _ : state)
        {
            node_count = 0;
            for (const auto& source : sources)
            {
                auto tree = heimdall::ParseTree::Parse(source);
                node_count += tree.NodesSoA().size();
                benchmark::DoNotOptimize(tree.NodesSoA().kind.data());
            }

            benchmark::DoNotOptimize(node_count);
        }

        state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * corpus_bytes));
        state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * node_count));
    }

    void BM_ParseUnclosedParen(benchmark::State& state)
    {
        const std::string source =
            "namespace demo {\n"
        "struct Widget { int value; };\n"
        "int broken(\n"
        "int use(Widget w) { return w.value; }\n"
        "int factory(int left, int right) { return left + right; }\n"
        "}\n";
        for (auto _ : state)
        {
            auto tree = heimdall::ParseTree::Parse(source);
            benchmark::DoNotOptimize(tree.NodesSoA().kind.data());
            benchmark::ClobberMemory();
        }

        state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * source.size()));
    }

    BENCHMARK(BM_Parse);

    BENCHMARK(BM_ParseUnclosedParen);

} // namespace
