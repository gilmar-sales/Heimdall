#pragma once

#include <Heimdall/AnalysisFeatures.hpp>
#include <Heimdall/PluginHost.hpp>

#include <algorithm>
#include <iterator>

namespace heimdall
{
    struct AnalysisResult
    {
        std::vector<Diagnostic> diagnostics;
        std::vector<RuleMetrics> rules;
        bool cancelled = false;
    };

    // One policy and coordinated node traversal for native and experimental
    // rules. Legacy built-ins retain their specialized token/model passes.
    class AnalysisEngine
    {
    public:
        explicit AnalysisEngine(RuleOptions options = {}) : m_engine(std::move(options)) {}
        std::expected<void, std::string> RegisterNative(ScheduledRule rule)
        {
            return m_host.RegisterNative(std::move(rule));
        }
        std::expected<void, std::string> RegisterPlugin(Plugin plugin)
        {
            return m_host.Register(std::move(plugin));
        }
        AnalysisResult Analyze(const AnalysisContext &context, bool semantic = true,
            const ProjectContext &project = {}, bool profiling = false, std::stop_token stop = {}) const
        {
            AnalysisResult result;
            if (stop.stop_requested()) { result.cancelled = true; return result; }
            result.diagnostics = AnalysisFeatures::Diagnostics(context, m_engine, semantic, project);
            auto scheduled = m_host.Analyze(context, m_engine, profiling, stop);
            result.cancelled = scheduled.cancelled;
            result.rules = std::move(scheduled.metrics);
            if (result.cancelled) { result.diagnostics.clear(); return result; }
            result.diagnostics.insert(result.diagnostics.end(),
                std::make_move_iterator(scheduled.diagnostics.begin()),
                std::make_move_iterator(scheduled.diagnostics.end()));
            std::stable_sort(result.diagnostics.begin(), result.diagnostics.end(),
                [](const Diagnostic &a, const Diagnostic &b) { return a.offset < b.offset; });
            return result;
        }
    private:
        RuleEngine m_engine;
        PluginHost m_host;
    };
}
