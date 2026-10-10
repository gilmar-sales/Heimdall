#include <benchmark/benchmark.h>

#include "Document.hpp"
#include "StbCorpus.hpp"
#include "WorkspaceSymbolIndex.hpp"

#include <Heimdall/ParseTree.hpp>
#include <Heimdall/SymbolOutline.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

    using heimdall::lsp::BuildIndexedFile;
    using heimdall::lsp::IndexedFilePtr;
    using heimdall::lsp::LineIndex;
    using heimdall::lsp::WorkspaceSymbolIndex;

    constexpr std::size_t kSymbolsPerFile = 40;
    constexpr std::size_t kStbLines       = 20000;

    // A header-sized synthetic file: a namespace with a few types and free functions, so the
    // vocabulary (and therefore the match rate of a query) is spread over many files.
    std::string SyntheticSource(std::size_t file)
    {
        std::string source = "namespace module" + std::to_string(file % 97) + "\n{\n";
        for (std::size_t i = 0; i < kSymbolsPerFile / 4; ++i)
        {
            const std::string suffix = std::to_string(file) + "_" + std::to_string(i);
            source += "struct Widget" + suffix + "\n{\n    int count" + suffix + ";\n" +
                      "    void Run" + suffix + "(int times) const;\n" +
                      "    static int Create" + suffix + "();\n};\n";
        }

        source += "}\n";
        return source;
    }

    IndexedFilePtr IndexSource(std::size_t file)
    {
        const std::string source = SyntheticSource(file);
        const heimdall::ParseTree tree = heimdall::ParseTree::Parse(source);
        LineIndex lines;
        lines.Build(source);
        return BuildIndexedFile("file:///workspace/module" + std::to_string(file) + ".hpp",
                                heimdall::SymbolOutline::Extract(tree), lines);
    }

    WorkspaceSymbolIndex& SyntheticIndex(std::size_t files)
    {
        static std::map<std::size_t, std::unique_ptr<WorkspaceSymbolIndex>> indexes;
        auto& slot = indexes[files];
        if (!slot)
        {
            slot = std::make_unique<WorkspaceSymbolIndex>();
            for (std::size_t file = 0; file < files; ++file)
            {
                slot->SetDisk(std::to_string(file), IndexSource(file));
            }
        }

        return *slot;
    }

    void BM_ExtractOutlineStb20k(benchmark::State& state)
    {
        const auto document = heimdall::bench::BuildDocument(HEIMDALL_STB_DIR, kStbLines);
        if (!document)
        {
            state.SkipWithError("stb corpus missing");
            return;
        }

        const heimdall::ParseTree tree = heimdall::ParseTree::Parse(*document);
        std::size_t symbols            = 0;
        for (auto _ : state)
        {
            auto outline = heimdall::SymbolOutline::Extract(tree);
            symbols      = outline.size();
            benchmark::DoNotOptimize(outline);
        }

        state.counters["symbols"]     = static_cast<double>(symbols);
        state.counters["lines"]       = static_cast<double>(heimdall::bench::CountLines(*document));
        state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * document->size()));
    }

    void BM_ParseAndExtractStb20k(benchmark::State& state)
    {
        const auto document = heimdall::bench::BuildDocument(HEIMDALL_STB_DIR, kStbLines);
        if (!document)
        {
            state.SkipWithError("stb corpus missing");
            return;
        }

        for (auto _ : state)
        {
            const heimdall::ParseTree tree = heimdall::ParseTree::Parse(*document);
            auto outline                   = heimdall::SymbolOutline::Extract(tree);
            benchmark::DoNotOptimize(outline);
        }

        state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * document->size()));
    }

    void BM_BuildIndexedFileStb20k(benchmark::State& state)
    {
        const auto document = heimdall::bench::BuildDocument(HEIMDALL_STB_DIR, kStbLines);
        if (!document)
        {
            state.SkipWithError("stb corpus missing");
            return;
        }

        const heimdall::ParseTree tree = heimdall::ParseTree::Parse(*document);
        const auto outline             = heimdall::SymbolOutline::Extract(tree);
        for (auto _ : state)
        {
            LineIndex lines;
            lines.Build(*document);
            auto file = BuildIndexedFile("file:///stb.h", outline, lines);
            benchmark::DoNotOptimize(file);
        }
    }

    void BM_ReplaceOpenBuffer(benchmark::State& state)
    {
        const std::size_t files = static_cast<std::size_t>(state.range(0));
        auto& index             = SyntheticIndex(files);
        const IndexedFilePtr buffer = IndexSource(1);
        for (auto _ : state)
        {
            index.SetOpen("open", buffer);
        }

        index.ClearOpen("open");
    }

    void BM_Query(benchmark::State& state, std::string_view query)
    {
        const std::size_t files = static_cast<std::size_t>(state.range(0));
        const auto& index       = SyntheticIndex(files);
        std::size_t hits        = 0;
        for (auto _ : state)
        {
            auto result = index.Query(query);
            hits        = result.size();
            benchmark::DoNotOptimize(result);
        }

        state.counters["files"]   = static_cast<double>(files);
        state.counters["symbols"] = static_cast<double>(files * (kSymbolsPerFile + 1));
        state.counters["hits"]    = static_cast<double>(hits);
    }

} // namespace

BENCHMARK(BM_ExtractOutlineStb20k)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_ParseAndExtractStb20k)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_BuildIndexedFileStb20k)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_ReplaceOpenBuffer)->Arg(1000)->Arg(20000)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Query, Substring, "widget1")
    ->Arg(1000)
    ->Arg(5000)
    ->Arg(20000)
    ->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_Query, ShortQuery, "w")->Arg(1000)->Arg(20000)->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_Query, Qualified, "module5::widget5_1")
    ->Arg(1000)
    ->Arg(20000)
    ->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_Query, NoMatch, "zzzzq")->Arg(1000)->Arg(20000)->Unit(benchmark::kMillisecond);
