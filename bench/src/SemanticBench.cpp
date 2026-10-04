// Benchmarks for the semantic engine (docs/semantic-engine-architecture.md):
// binding a parsed tree (scopes, symbols, references), typing it (declared
// types and expression types) and running the rules that sit on those models. Trees are parsed once outside the timed region.
#include <benchmark/benchmark.h>

#include <Heimdall/MappedBuffer.hpp>
#include <Heimdall/ParseTree.hpp>
#include <Heimdall/SemanticModel.hpp>
#include <Heimdall/SemanticRules.hpp>
#include <Heimdall/TypeModel.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace
{

struct Corpus
{
    std::vector<std::shared_ptr<const std::string>> sources;
    std::vector<heimdall::ParseTree> trees;
    std::size_t bytes = 0;
};

bool LoadCorpus(benchmark::State& state, Corpus& corpus)
{
    for (const auto& entry : std::filesystem::directory_iterator(HEIMDALL_CORPUS_DIR))
    {
        if (!entry.is_regular_file()) continue;
        auto buffer = heimdall::MappedBuffer::Open(entry.path().string());
        if (!buffer)
        {
            state.SkipWithError(buffer.error().c_str());
            return false;
        }
        corpus.sources.push_back(std::make_shared<const std::string>(buffer->view()));
        corpus.bytes += buffer->size();
    }
    for (const auto& source : corpus.sources) corpus.trees.push_back(heimdall::ParseTree::Parse(*source));
    if (corpus.trees.empty()) state.SkipWithError("empty corpus");
    return !corpus.trees.empty();
}

// `count` classes in a chain, each overriding two virtual functions of its base.
std::string Hierarchy(std::size_t count)
{
    std::string source = "struct C0 { virtual void a(int x); virtual int b() const; };\n";
    for (std::size_t i = 1; i < count; ++i)
    {
        source += "struct C" + std::to_string(i) + " : C" + std::to_string(i - 1) +
            " { void a(int y); int b() const; int field" + std::to_string(i) + "; };\n";
    }
    return source;
}

void BM_Bind(benchmark::State& state)
{
    Corpus corpus;
    if (!LoadCorpus(state, corpus)) return;
    std::size_t symbols = 0;
    std::size_t arena_bytes = 0;
    for (auto _ : state)
    {
        symbols = 0;
        arena_bytes = 0;
        for (const auto& tree : corpus.trees)
        {
            const auto model = heimdall::Binder::Bind(tree);
            symbols += model.Symbols().Size();
            arena_bytes += model.ArenaBytes();
            benchmark::DoNotOptimize(model.Refs().token.data());
        }
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * corpus.bytes));
    state.counters["symbols"] = static_cast<double>(symbols);
    state.counters["arena_bytes_per_symbol"] =
        symbols == 0 ? 0.0 : static_cast<double>(arena_bytes) / static_cast<double>(symbols);
}

void BM_BindHierarchy(benchmark::State& state)
{
    const std::string source = Hierarchy(static_cast<std::size_t>(state.range(0)));
    const auto tree = heimdall::ParseTree::Parse(source);
    for (auto _ : state)
    {
        const auto model = heimdall::Binder::Bind(tree);
        benchmark::DoNotOptimize(model.Symbols().Size());
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * source.size()));
}

void BM_ModernizeOverride(benchmark::State& state)
{
    const std::string source = Hierarchy(static_cast<std::size_t>(state.range(0)));
    const auto tree = heimdall::ParseTree::Parse(source);
    const auto model = heimdall::Binder::Bind(tree);
    std::size_t reported = 0;
    for (auto _ : state)
    {
        reported = heimdall::SemanticRules::AnalyzeOverride(model).size();
        benchmark::DoNotOptimize(reported);
    }
    state.counters["diagnostics"] = static_cast<double>(reported);
}

// `count` functions mixing typed expressions, conditions and loops over containers.
std::string TypedFunctions(std::size_t count)
{
    std::string source = "struct P { int x; double y; int* q; int get() const; };\n";
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::string n = std::to_string(i);
        source += "long run" + n + "(P p, std::vector<int>& v, int* raw, unsigned flags) {\n"
                  "    long total = 0;\n"
                  "    for (int i = 0; i < v.size(); ++i) { total += v[i] * 2 + p.x; }\n"
                  "    for (auto it = v.begin(); it != v.end(); ++it) { total += *it; }\n"
                  "    if (flags && raw) { total += flags + p.get(); }\n"
                  "    auto ratio = p.y * total + (flags ? 1.5 : 2.5);\n"
                  "    return static_cast<long>(ratio) + *p.q;\n"
                  "}\n";
    }
    return source;
}

void BM_Type(benchmark::State& state)
{
    Corpus corpus;
    if (!LoadCorpus(state, corpus)) return;
    std::vector<heimdall::SemanticModel> models;
    for (const auto& tree : corpus.trees) models.push_back(heimdall::Binder::Bind(tree));
    std::size_t types = 0;
    std::size_t arena_bytes = 0;
    std::size_t symbols = 0;
    for (auto _ : state)
    {
        types = 0;
        arena_bytes = 0;
        symbols = 0;
        for (const auto& model : models)
        {
            const auto typed = heimdall::Typer::Type(model);
            types += typed.Types().Size();
            arena_bytes += typed.ArenaBytes();
            symbols += model.Symbols().Size();
            benchmark::DoNotOptimize(typed.SymbolType(0));
        }
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * corpus.bytes));
    state.counters["types"] = static_cast<double>(types);
    state.counters["arena_bytes_per_symbol"] =
        symbols == 0 ? 0.0 : static_cast<double>(arena_bytes) / static_cast<double>(symbols);
}

void BM_TypeFunctions(benchmark::State& state)
{
    const std::string source = TypedFunctions(static_cast<std::size_t>(state.range(0)));
    const auto tree = heimdall::ParseTree::Parse(source);
    const auto model = heimdall::Binder::Bind(tree);
    for (auto _ : state)
    {
        const auto typed = heimdall::Typer::Type(model);
        benchmark::DoNotOptimize(typed.Types().Size());
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * source.size()));
}

void BM_TypeRules(benchmark::State& state)
{
    const std::string source = TypedFunctions(static_cast<std::size_t>(state.range(0)));
    const auto tree = heimdall::ParseTree::Parse(source);
    const auto model = heimdall::Binder::Bind(tree);
    const auto typed = heimdall::Typer::Type(model);
    std::size_t reported = 0;
    for (auto _ : state)
    {
        reported = heimdall::SemanticRules::AnalyzeImplicitBool(typed).size() +
            heimdall::SemanticRules::AnalyzeRangeLoop(typed).size() +
            heimdall::SemanticRules::AnalyzeLoopConvert(typed).size();
        benchmark::DoNotOptimize(reported);
    }
    state.counters["diagnostics"] = static_cast<double>(reported);
}

void BM_AnalyzeAllRules(benchmark::State& state)
{
    Corpus corpus;
    if (!LoadCorpus(state, corpus)) return;
    std::vector<heimdall::SemanticModel> models;
    for (const auto& tree : corpus.trees) models.push_back(heimdall::Binder::Bind(tree));
    std::size_t reported = 0;
    for (auto _ : state)
    {
        reported = 0;
        for (const auto& model : models) reported += heimdall::SemanticRules::Analyze(model).size();
        benchmark::DoNotOptimize(reported);
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * corpus.bytes));
    state.counters["diagnostics"] = static_cast<double>(reported);
}

} // namespace

BENCHMARK(BM_Bind);
BENCHMARK(BM_Type);
BENCHMARK(BM_AnalyzeAllRules);
BENCHMARK(BM_BindHierarchy)->Arg(100)->Arg(1000);
BENCHMARK(BM_ModernizeOverride)->Arg(100)->Arg(1000);
BENCHMARK(BM_TypeFunctions)->Arg(100)->Arg(1000);
BENCHMARK(BM_TypeRules)->Arg(100)->Arg(1000);
