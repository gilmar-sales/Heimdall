#include <benchmark/benchmark.h>

#include <Heimdall/Completion.hpp>
#include <Heimdall/Lexer.hpp>
#include <Heimdall/MappedBuffer.hpp>
#include <Heimdall/ParseTree.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace
{

    struct CorpusFile
    {
        std::string             source;
        heimdall::ParseTree     tree;
        heimdall::ParserOptions options;
        std::size_t             complete_offset = 0;
        std::size_t             hover_offset    = 0;
    };

    std::vector<CorpusFile> LoadCorpus(benchmark::State& state)
    {
        std::vector<CorpusFile> files;
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

            CorpusFile file;
            file.source = std::string(buffer->view());
            file.tree   = heimdall::ParseTree::Parse(file.source, file.options);
            file.tree.HoldSource(std::make_shared<const std::string>(file.source));
            // Complete at EOF (expression context in these samples); hover on the
            // first identifier of length >= 3 so the lookup does real work instead
            // of bailing out on whitespace/punctuation.
            file.complete_offset = file.source.size();
            for (const auto& token : file.tree.Tokens())
            {
                if (token.kind == heimdall::TokenKind::Identifier && token.length >= 3)
                {
                    file.hover_offset = token.offset + token.length / 2;
                    break;
                }
            }

            files.push_back(std::move(file));
        }

        if (files.empty())
        {
            state.SkipWithError("empty corpus");
        }

        return files;
    }

    void BM_Complete(benchmark::State& state)
    {
        const auto files = LoadCorpus(state);
        if (files.empty())
        {
            return;
        }

        std::size_t items = 0;
        for (auto _ : state)
        {
            items = 0;
            for (const auto& file : files)
            {
                auto found = heimdall::CompletionEngine::Complete(
                    file.tree, file.options, file.complete_offset, nullptr);
                items += found.size();
                benchmark::DoNotOptimize(found.data());
            }

            benchmark::DoNotOptimize(items);
        }

        state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * items));
    }

    void BM_Hover(benchmark::State& state)
    {
        const auto files = LoadCorpus(state);
        if (files.empty())
        {
            return;
        }

        std::size_t hits = 0;
        for (auto _ : state)
        {
            hits = 0;
            for (const auto& file : files)
            {
                auto hovered = heimdall::CompletionEngine::Hover(
                    file.tree, file.options, file.hover_offset, nullptr);
                hits += hovered.has_value() ? 1 : 0;
                benchmark::DoNotOptimize(hovered);
            }

            benchmark::DoNotOptimize(hits);
        }
    }

    BENCHMARK(BM_Complete);

    BENCHMARK(BM_Hover);

} // namespace
