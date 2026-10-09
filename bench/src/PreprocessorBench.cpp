#include <benchmark/benchmark.h>

#include <Heimdall/MappedBuffer.hpp>
#include <Heimdall/Preprocessor.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace
{

    void BM_Preprocess(benchmark::State& state)
    {
        std::vector<std::string> sources;
        std::size_t              source_bytes = 0;
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

        const heimdall::Preprocessor preprocessor;
        std::size_t                  output_bytes = 0;
        for (auto _ : state)
        {
            output_bytes = 0;
            for (const auto& source : sources)
            {
                auto result = preprocessor.Process(source, true);
                output_bytes += result.active_source.size();
                benchmark::DoNotOptimize(result.active_source.data());
            }
        }

        state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * source_bytes));
        state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * output_bytes));
    }

    BENCHMARK(BM_Preprocess);

} // namespace
