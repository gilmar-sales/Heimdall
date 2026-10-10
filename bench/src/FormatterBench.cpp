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

    template <bool reuse>
    void ShapingCheckpoint(benchmark::State& state)
    {
        std::string before;
        std::string after;
        for (std::int64_t index = 0; index < state.range(0); ++index)
        {
            before += "void Empty" + std::to_string(index) + "()\n{\n}\n\n";
            after += "void Empty" + std::to_string(index) + "()\n{\n    \n}\n\n";
        }
        const auto initialTree = heimdall::ParseTree::Parse(before);
        const auto changedTree = heimdall::ParseTree::Parse(after);
        const heimdall::Formatter formatter;
        const auto checkpoint = formatter.FormatIncremental(initialTree);
        for (auto _ : state)
        {
            const auto result = formatter.FormatIncremental(
                changedTree, reuse ? &checkpoint : nullptr);
            benchmark::DoNotOptimize(result.Formatted().data());
        }
        state.counters["reused_output"] = reuse ? 1 : 0;
    }

    BENCHMARK_TEMPLATE(ShapingCheckpoint, false)->Arg(100)->Arg(1000);
    BENCHMARK_TEMPLATE(ShapingCheckpoint, true)->Arg(100)->Arg(1000);

} // namespace
