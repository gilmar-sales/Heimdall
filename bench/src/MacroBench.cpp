#include <benchmark/benchmark.h>

#include <Heimdall/MacroEnvironment.hpp>
#include <Heimdall/MacroExpander.hpp>
#include <Heimdall/MacroScanner.hpp>
#include <Heimdall/MacroTable.hpp>
#include <Heimdall/MappedBuffer.hpp>
#include <Heimdall/ParseTree.hpp>
#include <Heimdall/SyntaxHighlight.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{

    constexpr std::size_t kTableMacros = 2000;
    constexpr std::size_t kExpandedStatements = 2000;
    constexpr std::size_t kHeaderCount = 200;
    constexpr std::size_t kMacrosPerHeader = 20;

    std::string ReadCorpus(std::string_view name)
    {
        auto buffer =
            heimdall::MappedBuffer::Open((std::filesystem::path(HEIMDALL_CORPUS_DIR) / name).string());
        return buffer ? std::string(buffer->view()) : std::string();
    }

    // A table of many unrelated macros plus the few the sources below use.
    heimdall::MacroTable MakeTable()
    {
        heimdall::MacroTable table;
        const auto define = [&table](const std::string& text)
        {
            if (auto parsed = heimdall::MacroTable::ParseDefinition(text))
            {
                table.Define(std::move(*parsed));
            }
        };
        for (std::size_t i = 0; i < kTableMacros; ++i)
        {
            define("UNRELATED_" + std::to_string(i) + " " + std::to_string(i));
        }

        define("LIMIT 64");
        define("ADD(a, b) ((a) + (b))");
        define("MUL(a, b) ((a) * (b))");
        define("STR(x) #x");
        define("CAT(a, b) a##b");
        define("LOG(fmt, ...) printf(fmt, __VA_ARGS__)");
        return table;
    }

    std::shared_ptr<const heimdall::MacroResolver> ResolverFor(const std::string& source)
    {
        heimdall::MacroEnvironmentOptions options;
        options.probeCompiler = false;
        return std::make_shared<heimdall::MacroEnvironmentResolver>(
            std::make_shared<const heimdall::MacroEnvironment>(
            heimdall::MacroEnvironment::Build("/nonexistent/bench.cpp", source, nullptr, options)));
    }

    void BM_Expand_ObjectLike(benchmark::State& state)
    {
        const auto table = MakeTable();
        std::string text;
        for (std::size_t i = 0; i < kExpandedStatements; ++i)
        {
            text += "int v" + std::to_string(i) + " = LIMIT + UNRELATED_" + std::to_string(i) + ";\n";
        }

        const heimdall::MacroExpander expander(table);
        for (auto _ : state)
        {
            auto result = expander.Expand(text);
            benchmark::DoNotOptimize(result.tokens.data());
        }

        state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * text.size()));
    }

    void BM_Expand_FunctionLike(benchmark::State& state)
    {
        const auto table = MakeTable();
        std::string text;
        for (std::size_t i = 0; i < kExpandedStatements; ++i)
        {
            text += "int w" + std::to_string(i) + " = ADD(MUL(x, LIMIT), MUL(y, " +
                std::to_string(i) + "));\n";
        }

        const heimdall::MacroExpander expander(table);
        for (auto _ : state)
        {
            auto result = expander.Expand(text);
            benchmark::DoNotOptimize(result.tokens.data());
        }

        state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * text.size()));
    }

    void BM_Expand_StringifyAndPaste(benchmark::State& state)
    {
        const auto table = MakeTable();
        std::string text;
        for (std::size_t i = 0; i < kExpandedStatements; ++i)
        {
            text += "const char* s" + std::to_string(i) + " = STR(a + b); int CAT(id, " +
                std::to_string(i) + ") = 0; LOG(\"%d\", " + std::to_string(i) + ");\n";
        }

        const heimdall::MacroExpander expander(table);
        for (auto _ : state)
        {
            auto result = expander.Expand(text);
            benchmark::DoNotOptimize(result.tokens.data());
        }

        state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * text.size()));
    }

    // The worst honest shape: every level doubles the output (`#define A1 A0 A0`).
    void BM_Expand_DoublingChainHitsTheLimit(benchmark::State& state)
    {
        heimdall::MacroTable table;
        table.Define(*heimdall::MacroTable::ParseDefinition("A0 x"));
        for (int level = 1; level < 40; ++level)
        {
            table.Define(*heimdall::MacroTable::ParseDefinition(
                "A" + std::to_string(level) + " A" + std::to_string(level - 1) + " A" +
                std::to_string(level - 1)));
        }

        heimdall::ExpansionLimits limits;
        limits.maxTokens = 100'000;
        const heimdall::MacroExpander expander(table, limits);
        for (auto _ : state)
        {
            auto result = expander.Expand("A39");
            benchmark::DoNotOptimize(result.tokens.data());
        }
    }

    void BM_Scan_Directives(benchmark::State& state)
    {
        const std::string source = ReadCorpus("preprocessor_sample.hpp");
        if (source.empty())
        {
            state.SkipWithError("empty corpus");
            return;
        }

        for (auto _ : state)
        {
            heimdall::MacroTable table;
            auto result = heimdall::MacroScanner::Scan(source, "sample.hpp", table);
            benchmark::DoNotOptimize(result.activeRanges.data());
        }

        state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * source.size()));
    }

    // A tree of headers: each defines macros, includes the next two, and is guarded.
    class HeaderTree
    {
    public:
        HeaderTree()
        {
            static std::atomic<unsigned> counter {0};
            mRoot = std::filesystem::temp_directory_path() /
                    ("heimdall-macro-bench-" + std::to_string(counter.fetch_add(1)) + "-" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this) % 100000));
            std::filesystem::create_directories(mRoot);
            for (std::size_t i = 0; i < kHeaderCount; ++i)
            {
                std::string text = "#ifndef H" + std::to_string(i) + "_HPP\n#define H" +
                    std::to_string(i) + "_HPP\n";
                for (std::size_t child = 2 * i + 1; child <= 2 * i + 2 && child < kHeaderCount; ++child)
                {
                    text += "#include \"h" + std::to_string(child) + ".hpp\"\n";
                }

                for (std::size_t m = 0; m < kMacrosPerHeader; ++m)
                {
                    text += "#define M" + std::to_string(i) + "_" + std::to_string(m) + "(x) ((x) + " +
                        std::to_string(m) + ")\n";
                }

                text += "#if defined(H0_HPP) && M" + std::to_string(i) + "_0(1) > 0\nint v" +
                    std::to_string(i) + ";\n#endif\n#endif\n";
                std::ofstream(mRoot /("h" + std::to_string(i) + ".hpp")) << text;
            }

            mMain = "#include \"h0.hpp\"\nint x = M5_3(2);\n";
            std::ofstream(mRoot / "main.cpp") << mMain;
        }

        ~HeaderTree()
        {
            std::error_code ec;
            std::filesystem::remove_all(mRoot, ec);
        }

        HeaderTree(const HeaderTree&) = delete;

        HeaderTree& operator= (const HeaderTree&) = delete;

        heimdall::MacroEnvironment Build() const
        {
            heimdall::MacroEnvironmentOptions options;
            options.probeCompiler = false;
            return heimdall::MacroEnvironment::Build(mRoot / "main.cpp", mMain, nullptr, options);
        }

    private:
        std::filesystem::path mRoot;
        std::string mMain;
    };

    // Different directory every iteration, so no header effect is in the cache.
    void BM_Environment_Cold(benchmark::State& state)
    {
        for (auto _ : state)
        {
            state.PauseTiming();
            const HeaderTree tree;
            state.ResumeTiming();
            auto environment = tree.Build();
            auto finalEnv = environment.Final();

            benchmark::DoNotOptimize(finalEnv);
        }
    }

    // Same tree again: every header effect replays instead of being scanned.
    void BM_Environment_Warm(benchmark::State& state)
    {
        const HeaderTree tree;
        (void) tree.Build();
        for (auto _ : state)
        {
            auto environment = tree.Build();
            auto finalEnv = environment.Final();
            benchmark::DoNotOptimize(finalEnv);
        }
    }

    void BM_Parse_WithoutResolver(benchmark::State& state)
    {
        const std::string source = ReadCorpus("parser_sample.hpp");
        for (auto _ : state)
        {
            const auto tree = heimdall::ParseTree::Parse(source);
            benchmark::DoNotOptimize(tree.Tokens().data());
        }

        state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * source.size()));
    }

    void BM_Parse_WithResolver(benchmark::State& state)
    {
        const std::string source = ReadCorpus("parser_sample.hpp");
        heimdall::ParserOptions options;
        options.macro_resolver = ResolverFor(source);
        for (auto _ : state)
        {
            const auto tree = heimdall::ParseTree::Parse(source, options);
            benchmark::DoNotOptimize(tree.Tokens().data());
        }

        state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * source.size()));
    }

    void BM_Highlight(benchmark::State& state, bool withResolver)
    {
        const std::string source = ReadCorpus("parser_sample.hpp");
        heimdall::ParserOptions options;
        std::shared_ptr<const heimdall::MacroResolver> resolver;
        if (withResolver)
        {
            resolver = ResolverFor(source);
            options.macro_resolver = resolver;
        }

        const auto tree = heimdall::ParseTree::Parse(source, options);
        heimdall::HighlightContext context;
        if (resolver)
        {
            context.findMacro = [&resolver](std::string_view name, std::size_t offset)
            {
                return resolver->Find(name, offset);
            };
            context.activeRanges = tree.ActiveRanges();
        }

        for (auto _ : state)
        {
            auto spans = heimdall::SyntaxHighlight::Extract(tree, context);
            benchmark::DoNotOptimize(spans.data());
        }

        state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * source.size()));
    }

    // 5000 lines that each use a function-like and an object-like macro defined in the file:
    // what one highlight request pays on top of the cached environment.
    std::string LargeSource()
    {
        std::string text;
        for (int i = 0; i < 300; ++i)
        {
            text += "#define CONFIG_" + std::to_string(i) + "(x) ((x) + " + std::to_string(i) + ")\n";
        }

        text += "#define LIMIT 64\n";
        for (int i = 0; i < 5000; ++i)
        {
            text += "int value" + std::to_string(i) + " = CONFIG_" + std::to_string(i % 300) +
                "(LIMIT) + " + std::to_string(i) + ";\n";
        }

        return text;
    }

    void BM_Large_Environment(benchmark::State& state)
    {
        const std::string source = LargeSource();
        heimdall::MacroEnvironmentOptions options;
        options.probeCompiler = false;
        for (auto _ : state)
        {
            auto environment = heimdall::MacroEnvironment::Build("/nonexistent/large.cpp", source,
                nullptr, options);

            auto finalEnv = environment.Final();

            benchmark::DoNotOptimize(finalEnv);
        }
    }

    void BM_Large_ParseWithResolver(benchmark::State& state)
    {
        const std::string source = LargeSource();
        heimdall::ParserOptions options;
        options.macro_resolver = ResolverFor(source);
        for (auto _ : state)
        {
            const auto tree = heimdall::ParseTree::Parse(source, options);
            benchmark::DoNotOptimize(tree.Tokens().data());
        }
    }

    void BM_Large_Highlight(benchmark::State& state)
    {
        const std::string source = LargeSource();
        heimdall::ParserOptions options;
        const auto resolver = ResolverFor(source);
        options.macro_resolver = resolver;
        const auto tree = heimdall::ParseTree::Parse(source, options);
        heimdall::HighlightContext context;
        context.findMacro = [&resolver](std::string_view name, std::size_t offset)
        {
            return resolver->Find(name, offset);
        };
        context.activeRanges = tree.ActiveRanges();
        std::size_t spans = 0;
        for (auto _ : state)
        {
            auto result = heimdall::SyntaxHighlight::Extract(tree, context);
            spans = result.size();
            benchmark::DoNotOptimize(result.data());
        }

        state.counters["spans"] = static_cast<double>(spans);
    }

    BENCHMARK(BM_Expand_ObjectLike);

    BENCHMARK(BM_Expand_FunctionLike);

    BENCHMARK(BM_Expand_StringifyAndPaste);

    BENCHMARK(BM_Expand_DoublingChainHitsTheLimit);

    BENCHMARK(BM_Scan_Directives);

    BENCHMARK(BM_Environment_Cold)->Unit(benchmark::kMillisecond);

    BENCHMARK(BM_Environment_Warm)->Unit(benchmark::kMillisecond);

    BENCHMARK(BM_Parse_WithoutResolver);

    BENCHMARK(BM_Parse_WithResolver);

    BENCHMARK(BM_Large_Environment)->Unit(benchmark::kMillisecond);

    BENCHMARK(BM_Large_ParseWithResolver)->Unit(benchmark::kMillisecond);

    BENCHMARK(BM_Large_Highlight)->Unit(benchmark::kMillisecond);

    BENCHMARK_CAPTURE(BM_Highlight, NoMacros, false);

    BENCHMARK_CAPTURE(BM_Highlight, WithMacros, true);

} // namespace
