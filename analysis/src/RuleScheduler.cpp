#include <Heimdall/RuleScheduler.hpp>

#include <Heimdall/LineTable.hpp>

#include <algorithm>
#include <array>
#include <chrono>

namespace heimdall
{
    DiagnosticSink::DiagnosticSink(const AnalysisContext &context, const RuleDescriptor &rule,
        std::vector<Diagnostic> &diagnostics, RuleMetrics &metrics, const LineTable &lines)
        : m_context(context), m_rule(rule), m_diagnostics(diagnostics), m_metrics(metrics), m_lines(lines) {}

    bool DiagnosticSink::Emit(Diagnostic diagnostic)
    {
        const auto size = m_context.Syntax().Source().size();
        const auto valid = [size](std::size_t offset, std::size_t length)
        { return offset <= size && length <= size - offset; };
        if (!valid(diagnostic.offset, diagnostic.length) ||
            (diagnostic.has_fix && !valid(diagnostic.fix.offset, diagnostic.fix.length)))
        {
            ++m_metrics.rejected_diagnostics;
            return false;
        }
        diagnostic.rule = m_rule.id;
        diagnostic.code = m_rule.code;
        diagnostic.severity = m_rule.severity;
        const auto position = m_lines.Lookup(diagnostic.offset);
        diagnostic.line = position.line;
        diagnostic.column = position.column;
        m_diagnostics.push_back(std::move(diagnostic));
        ++m_metrics.diagnostics_emitted;
        return true;
    }

    bool DiagnosticSink::Emit(SourceRange range, std::string message)
    {
        Diagnostic diagnostic{};
        diagnostic.offset = range.offset;
        diagnostic.length = range.length;
        diagnostic.message = std::move(message);
        return Emit(std::move(diagnostic));
    }

    std::expected<void, std::string> RuleScheduler::Register(ScheduledRule rule)
    {
        if (rule.metadata.code.empty() || (!rule.on_node && !rule.on_document))
            return std::unexpected("A rule needs a code and a callback");
        if (rule.on_node && rule.interests.empty()) return std::unexpected("A node rule needs interests");
        for (const auto &existing : m_rules) if (existing.metadata.code == rule.metadata.code)
            return std::unexpected("Duplicate rule code: " + rule.metadata.code);
        std::sort(rule.interests.begin(), rule.interests.end());
        rule.interests.erase(std::unique(rule.interests.begin(), rule.interests.end()), rule.interests.end());
        m_rules.push_back(std::move(rule));
        return {};
    }

    ScheduledResult RuleScheduler::Analyze(const AnalysisContext &context, const RuleEngine &engine,
        bool profiling, std::stop_token stop) const
    {
        ScheduledResult result;
        if (!context.Valid()) return result;
        result.metrics.resize(m_rules.size());
        LineTable lines;
        lines.Build(context.Syntax().Source());
        std::array<std::vector<std::size_t>, 256> consumers;
        std::vector<bool> enabled(m_rules.size());
        std::vector<std::vector<std::uint64_t>> samples(profiling ? m_rules.size() : 0);
        for (std::size_t i = 0; i < m_rules.size(); ++i)
        {
            result.metrics[i].code = m_rules[i].metadata.code;
            enabled[i] = engine.Enabled(m_rules[i].metadata.code, m_rules[i].metadata.enabled);
            if (enabled[i] && m_rules[i].on_node) for (auto kind : m_rules[i].interests)
                consumers[static_cast<std::uint8_t>(kind)].push_back(i);
        }
        auto invoke = [&](std::size_t i, NodeId node, bool document)
        {
            auto &metrics = result.metrics[i];
            if (metrics.failed) return;
            DiagnosticSink sink(context, m_rules[i].metadata, result.diagnostics, metrics, lines);
            const auto before = result.diagnostics.size();
            const auto emitted = metrics.diagnostics_emitted;
            const auto start = profiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            ++metrics.execution_count;
            auto callback = [&]
            {
                if (document) m_rules[i].on_document(context, sink);
                else m_rules[i].on_node(context, node, sink);
            };
            if (m_rules[i].external)
            {
                try { callback(); }
                catch (...)
                {
                    metrics.failed = true;
                    result.diagnostics.resize(before);
                    metrics.diagnostics_emitted = emitted;
                }
            }
            else callback();
            if (profiling)
            {
                const auto ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start).count());
                metrics.total_ns += ns;
                samples[i].push_back(ns);
            }
        };
        for (std::size_t i = 0; i < m_rules.size(); ++i)
        {
            if (stop.stop_requested()) break;
            if (enabled[i] && m_rules[i].on_document) invoke(i, InvalidNode, true);
        }
        const auto &soa = context.Syntax().NodesSoA();
        for (NodeId node = 0; node < soa.size() && !stop.stop_requested(); ++node)
            for (auto rule : consumers[static_cast<std::uint8_t>(soa.Kind(node))])
            {
                if (stop.stop_requested()) break;
                invoke(rule, node, false);
            }
        result.cancelled = stop.stop_requested();
        for (auto &metrics : result.metrics) if (metrics.failed)
        {
            std::erase_if(result.diagnostics, [&](const Diagnostic &diagnostic) { return diagnostic.code == metrics.code; });
            metrics.diagnostics_emitted = 0;
        }
        if (result.cancelled) result.diagnostics.clear();
        else result.diagnostics = engine.ApplyPolicy(std::move(result.diagnostics), context.Syntax());
        if (profiling) for (std::size_t i = 0; i < samples.size(); ++i)
        {
            auto &times = samples[i];
            if (times.empty()) continue;
            const auto index = (times.size() * 95 + 99) / 100 - 1;
            std::nth_element(times.begin(), times.begin() + index, times.end());
            result.metrics[i].p95_ns = times[index];
        }
        return result;
    }
}
