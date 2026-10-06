#include <benchmark/benchmark.h>

#include <Heimdall/Arena.hpp>
#include <Heimdall/MappedBuffer.hpp>
#include <Heimdall/LineTable.hpp>

#include <filesystem>
#include <string>

namespace
{

    // Simulates CST-node-sized traffic: many small aligned allocations,
    // arena reset between iterations (per-file / per-TU lifetime).
    void BM_ArenaAlloc64B(benchmark::State& state)
    {
        heimdall::Arena arena;
        for (auto _ : state)
        {
            for (int i = 0; i < 4096; ++i)
            {
                benchmark::DoNotOptimize(arena.Allocate(64, 8));
            }

            state.PauseTiming();
            arena.Reset();
            state.ResumeTiming();
        }

        state.SetItemsProcessed(state.iterations() * 4096);
    }

    BENCHMARK(BM_ArenaAlloc64B);

    void BM_LineTableBuild(benchmark::State& state)
    {
        std::string corpus;
        for (const auto& entry : std::filesystem::directory_iterator(HEIMDALL_CORPUS_DIR))
        {
            if (entry.is_regular_file())
            {
                auto buffer = heimdall::MappedBuffer::Open(entry.path().string());
                if (!buffer)
                {
                    state.SkipWithError(buffer.error().c_str());
                    return;
                }

                corpus.append(buffer->view());
            }
        }

        if (corpus.empty())
        {
            state.SkipWithError("empty corpus");
            return;
        }

        heimdall::LineTable table;
        for (auto _ : state)
        {
            table.Build(corpus);
            benchmark::DoNotOptimize(table.LineCount());
        }

        state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * corpus.size()));
    }

    BENCHMARK(BM_LineTableBuild);

} // namespace
