// Cross-file index cost: building a shard from a bound file, publishing a snapshot over
// many shards, resolving one includer and answering a references query. Workspaces are
// synthetic: every source includes three headers and calls into them.
#include <benchmark/benchmark.h>

#include <Heimdall/IndexSnapshot.hpp>
#include <Heimdall/IndexRename.hpp>
#include <Heimdall/ParseTree.hpp>
#include <Heimdall/ShardBuilder.hpp>

#include <memory>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace
{
    namespace idx = heimdall::index;

    constexpr int kMembers = 8;

    std::string HeaderText(int number)
    {
        std::string text = "namespace n" + std::to_string(number) + " {\nstruct T" +
            std::to_string(number) + " {\n";
        for (int member = 0; member < kMembers; ++member)
        {
            text += "    int Member" + std::to_string(member) + "(int value);\n";
        }

        text += "};\nint Free" + std::to_string(number) + "(int, int);\n}\n";
        return text;
    }

    std::string SourceText(int number, int headers)
    {
        std::string text;
        for (int k = 0; k < 3; ++k)
        {
            text += "#include \"h" + std::to_string((number + k) % headers) + ".hpp\"\n";
        }

        text += "int Run" + std::to_string(number) + "()\n{\n";
        for (int k = 0; k < 3; ++k)
        {
            const auto h = std::to_string((number + k) % headers);
            text += "    n" + h + "::T" + h + " value" + h + ";\n    value" + h +
                ".Member1(1);\n    n" + h + "::Free" + h + "(1, 2);\n";
        }

        return text + "    return 0;\n}\n";
    }

    std::shared_ptr<const idx::FileShard> Make(std::string path, std::string source)
    {
        const auto tree = heimdall::ParseTree::Parse(source);
        const auto model = heimdall::Binder::Bind(tree);
        return idx::ShardBuilder::Build(model, std::move(path));
    }

    std::vector<std::shared_ptr<const idx::FileShard>> Workspace(int headers, int sources)
    {
        std::vector<std::shared_ptr<const idx::FileShard>> shards;
        for (int i = 0; i < headers; ++i)
        {
            shards.push_back(Make("h" + std::to_string(i) + ".hpp", HeaderText(i)));
        }

        for (int i = 0; i < sources; ++i)
        {
            shards.push_back(Make("s" + std::to_string(i) + ".cpp", SourceText(i, headers)));
        }

        return shards;
    }

    void BuildShard(benchmark::State& state)
    {
        const auto source = SourceText(0, 16);
        const auto tree = heimdall::ParseTree::Parse(source);
        const auto model = heimdall::Binder::Bind(tree);
        for (auto _ : state)
        {
            benchmark::DoNotOptimize(idx::ShardBuilder::Build(model, "s0.cpp"));
        }
    }

    void BuildSnapshot(benchmark::State& state)
    {
        const auto shards = Workspace(static_cast<int>(state.range(0)),
            static_cast<int>(state.range(0)) * 4);
        for (auto _ : state)
        {
            benchmark::DoNotOptimize(idx::IndexSnapshot::Build(shards));
        }

        state.counters["shards"] = static_cast<double>(shards.size());
    }

    void ResolveOneIncluder(benchmark::State& state)
    {
        const auto shards = Workspace(static_cast<int>(state.range(0)),
            static_cast<int>(state.range(0)) * 4);
        for (auto _ : state)
        {
            const auto snapshot = idx::IndexSnapshot::Build(shards);
            auto* resolution = &snapshot->Resolved(static_cast<std::uint32_t>(shards.size()) - 1);
            benchmark::DoNotOptimize(resolution);
        }
    }

    void ReferencesQuery(benchmark::State& state)
    {
        const auto shards = Workspace(static_cast<int>(state.range(0)),
            static_cast<int>(state.range(0)) * 4);
        const auto snapshot = idx::IndexSnapshot::Build(shards);
        const auto symbols = shards[0]->Symbols();
        idx::SymbolHash hash = 0;
        for (std::uint32_t i = 0; i < symbols.Size(); ++i)
        {
            if (shards[0]->Name(i) == "Member1")
            {
                hash = symbols.hash[i];
            }
        }

        for (auto _ : state)
        {
            benchmark::DoNotOptimize(snapshot->References(hash));
        }
    }

    BENCHMARK(BuildShard);

    BENCHMARK(BuildSnapshot)->Arg(50)->Arg(250);

    BENCHMARK(ResolveOneIncluder)->Arg(50)->Arg(250);

    BENCHMARK(ReferencesQuery)->Arg(50)->Arg(250);

    void RenameAndPreview(benchmark::State& state)
    {
        const auto headers = static_cast<int>(state.range(0));
        constexpr int kSourcesPerHeader = 4;
        std::map<std::string, std::shared_ptr<const std::string>> documents;
        for (int i = 0; i < headers; ++i)
        {
            documents.emplace("h" + std::to_string(i) + ".hpp",
                              std::make_shared<const std::string>(HeaderText(i)));
        }

        for (int i = 0; i < headers * kSourcesPerHeader; ++i)
        {
            documents.emplace("s" + std::to_string(i) + ".cpp",
                              std::make_shared<const std::string>(SourceText(i, headers)));
        }

        std::vector<std::shared_ptr<const idx::FileShard>> shards;
        shards.reserve(documents.size());
        for (const auto& [path, source] : documents)
        {
            shards.push_back(Make(path, *source));
        }

        const auto snapshot = idx::IndexSnapshot::Build(std::move(shards));
        heimdall::IndexRenameContext context;
        context.complete = true;
        context.document = [&documents](const std::string& path)->std::optional<heimdall::IndexedDocument>
        {
            const auto found = documents.find(path);
            if (found == documents.end())
            {
                return std::nullopt;
            }

            heimdall::IndexedDocument document;
            document.text = found->second;
            return document;
        };
        const auto offset = documents.at("h0.hpp")->find("T0");
        for (auto _ : state)
        {
            const auto plan = heimdall::IndexRename::Rename(*snapshot, "h0.hpp", offset, "RenamedT0", context);
            if (!plan)
            {
                state.SkipWithError(plan.error().message.c_str());
                break;
            }

            const auto preview = heimdall::IndexRename::Preview(*snapshot, *plan, context);
            if (!preview)
            {
                state.SkipWithError(preview.error().message.c_str());
                break;
            }

            benchmark::DoNotOptimize(preview->data());
        }

        state.counters["shards"] = static_cast<double>(documents.size());
    }

    BENCHMARK(RenameAndPreview)->Arg(25)->Arg(100);
} // namespace
