#include <benchmark/benchmark.h>

#include <Heimdall/MappedBuffer.hpp>
#include <Heimdall/Lexer.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace
{

void BM_Lex(benchmark::State& state)
{
    std::vector<std::string> sources;
    std::size_t corpus_bytes = 0;
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
        corpus_bytes += buffer->size();
    }
    if (sources.empty())
    {
        state.SkipWithError("empty corpus");
        return;
    }

    std::size_t token_count = 0;
    for (auto _ : state)
    {
        token_count = 0;
        for (const auto& source : sources)
        {
            const heimdall::Lexer lexer(source);
            const auto tokens = lexer.Lex();
            token_count += tokens.size();
            benchmark::DoNotOptimize(tokens.data());
        }
        benchmark::DoNotOptimize(token_count);
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * corpus_bytes));
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * token_count));
}

BENCHMARK(BM_Lex);

} // namespace
