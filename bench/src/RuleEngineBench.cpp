#include <benchmark/benchmark.h>

#include <Heimdall/MappedBuffer.hpp>
#include <Heimdall/RuleEngine.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace
{

void BM_RuleEngine(benchmark::State& state)
{
    std::vector<std::string> sources;
    std::size_t source_bytes = 0;
    for (const auto& entry : std::filesystem::directory_iterator(HEIMDALL_CORPUS_DIR))
    {
        if (!entry.is_regular_file()) continue;
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

    const heimdall::RuleEngine engine;
    std::size_t diagnostic_count = 0;
    for (auto _ : state)
    {
        diagnostic_count = 0;
        for (const auto& source : sources)
        {
            const auto diagnostics = engine.Analyze(source);
            diagnostic_count += diagnostics.size();
            benchmark::DoNotOptimize(diagnostics.data());
        }
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * source_bytes));
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * diagnostic_count));
}

BENCHMARK(BM_RuleEngine);

} // namespace
