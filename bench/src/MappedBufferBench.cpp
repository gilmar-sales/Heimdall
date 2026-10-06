#include <benchmark/benchmark.h>

#include <Heimdall/MappedBuffer.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace
{

    std::vector<std::string> CorpusFiles()
    {
        std::vector<std::string> out;
        for (const auto& entry : std::filesystem::directory_iterator(HEIMDALL_CORPUS_DIR))
        {
            if (entry.is_regular_file())
            {
                out.push_back(entry.path().string());
            }
        }

        return out;
    }

    void BM_MappedBufferOpen(benchmark::State& state)
    {
        const auto files = CorpusFiles();
        if (files.empty())
        {
            state.SkipWithError("empty corpus");
            return;
        }

        std::size_t bytes = 0;
        for (auto _ : state)
        {
            for (const auto& file : files)
            {
                auto buffer = heimdall::MappedBuffer::Open(file);
                if (!buffer)
                {
                    state.SkipWithError(buffer.error().c_str());
                    return;
                }

                bytes += buffer->size();
                benchmark::DoNotOptimize(buffer->data());
            }
        }

        state.SetBytesProcessed(static_cast<std::int64_t>(bytes));
    }

    BENCHMARK(BM_MappedBufferOpen);

} // namespace
