#include <benchmark/benchmark.h>

#include <Heimdall/MappedBuffer.hpp>
#include <Heimdall/SyntaxTree.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace
{

    void BM_ParseSyntaxTree(benchmark::State& state)
    {
        std::vector<std::string> sources;
        std::size_t source_bytes = 0;
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
                return;
            }

            sources.emplace_back(buffer->view());
            source_bytes += buffer->size();
        }

        if (sources.empty())
        {
            state.SkipWithError("empty corpus");
            return;
        }

        std::size_t node_count = 0;
        for (auto _ : state)
        {
            node_count = 0;
            for (const auto& source : sources)
            {
                auto tree = heimdall::SyntaxTree::Parse(source);
                node_count += tree.Nodes().size();
                benchmark::DoNotOptimize(tree.Nodes().data());
            }
        }

        state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * source_bytes));
        state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * node_count));
    }

    BENCHMARK(BM_ParseSyntaxTree);

} // namespace
