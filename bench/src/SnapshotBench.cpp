#include <Heimdall/Lexer.hpp>
#include <Heimdall/SourceSnapshot.hpp>
#include <Heimdall/TokenSnapshot.hpp>

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{
    std::string MakeSource(std::size_t lines)
    {
        std::string source;
        source.reserve(lines * 30);
        for (std::size_t i = 0; i < lines; ++i)
        {
            source += "int value = 42; // comment\n";
        }
        return source;
    }

    void PersistentTextEdit(benchmark::State& state)
    {
        const auto text   = MakeSource(static_cast<std::size_t>(state.range(0)));
        const auto source = *heimdall::SourceSnapshot::Create(text);
        for (auto _ : state)
        {
            auto next = source.Edit(text.size() / 2, 1, "x");
            benchmark::DoNotOptimize(next);
        }
        state.counters["copied_bytes"] = 1;
    }

    void ContiguousTextEdit(benchmark::State& state)
    {
        const auto text = MakeSource(static_cast<std::size_t>(state.range(0)));
        for (auto _ : state)
        {
            auto next = text;
            next.replace(text.size() / 2, 1, "x");
            benchmark::DoNotOptimize(next);
        }
        state.counters["copied_bytes"] = static_cast<double>(text.size());
    }

    void PersistentTokenEdit(benchmark::State& state)
    {
        const auto text   = MakeSource(static_cast<std::size_t>(state.range(0)));
        const auto before = *heimdall::SourceSnapshot::Create(text);
        const auto tokens = heimdall::TokenSnapshot::Create(heimdall::Lexer(text).Lex());
        const auto offset = text.find("42", text.size() / 2);
        const auto source = *before.Edit(offset, 2, "123");
        const heimdall::Lexer::TextEdit edit { offset, 2, 3 };
        for (auto _ : state)
        {
            benchmark::DoNotOptimize(tokens.Relex(source, edit));
        }
        state.counters["copied_tokens"] =
            static_cast<double>(tokens.Relex(source, edit).CopiedTokens());
    }

    void ContiguousTokenEdit(benchmark::State& state)
    {
        auto       text   = MakeSource(static_cast<std::size_t>(state.range(0)));
        const auto tokens = heimdall::Lexer(text).Lex();
        const auto offset = text.find("42", text.size() / 2);
        text.replace(offset, 2, "123");
        const heimdall::Lexer::TextEdit edit { offset, 2, 3 };
        for (auto _ : state)
        {
            auto next = tokens;
            heimdall::Lexer(text).Relex(next, edit);
            benchmark::DoNotOptimize(next);
        }
        state.counters["copied_tokens"] = static_cast<double>(tokens.size());
    }

    void TokenAoSKindScan(benchmark::State& state)
    {
        const auto tokens =
            heimdall::Lexer(MakeSource(static_cast<std::size_t>(state.range(0)))).Lex();
        for (auto _ : state)
        {
            std::size_t count = 0;
            for (const auto& token : tokens)
            {
                count += token.kind == heimdall::TokenKind::Identifier;
            }
            benchmark::DoNotOptimize(count);
        }
    }

    void TokenSoAKindScan(benchmark::State& state)
    {
        const auto tokens =
            heimdall::Lexer(MakeSource(static_cast<std::size_t>(state.range(0)))).Lex();
        std::vector<heimdall::TokenKind> kinds;
        kinds.reserve(tokens.size());
        for (const auto& token : tokens)
        {
            kinds.push_back(token.kind);
        }
        for (auto _ : state)
        {
            std::size_t count = 0;
            for (const auto kind : kinds)
            {
                count += kind == heimdall::TokenKind::Identifier;
            }
            benchmark::DoNotOptimize(count);
        }
    }

    BENCHMARK(PersistentTextEdit)->Arg(1000)->Arg(20000);
    BENCHMARK(ContiguousTextEdit)->Arg(1000)->Arg(20000);
    BENCHMARK(PersistentTokenEdit)->Arg(1000)->Arg(20000);
    BENCHMARK(ContiguousTokenEdit)->Arg(1000)->Arg(20000);
    BENCHMARK(TokenAoSKindScan)->Arg(1000)->Arg(20000);
    BENCHMARK(TokenSoAKindScan)->Arg(1000)->Arg(20000);
} // namespace
