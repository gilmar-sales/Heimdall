#include <benchmark/benchmark.h>

#include <Heimdall/Formatter.hpp>
#include <Heimdall/MappedBuffer.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace
{

    void BM_Format(benchmark::State& state)
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

        const heimdall::Formatter formatter;
        std::size_t               output_bytes = 0;
        for (auto _ : state)
        {
            output_bytes = 0;
            for (const auto& source : sources)
            {
                auto output = formatter.Format(source);
                output_bytes += output.size();
                benchmark::DoNotOptimize(output.data());
            }
        }

        state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * source_bytes));
        state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * output_bytes));
    }

    BENCHMARK(BM_Format);

} // namespace
