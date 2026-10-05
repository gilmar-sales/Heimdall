#pragma once

#include <Heimdall/AnalysisContext.hpp>
#include <Heimdall/RuleEngine.hpp>
#include <Heimdall/LineTable.hpp>

#include <expected>
#include <functional>
#include <span>

namespace heimdall
{
    struct RuleDescriptor
    {
        RuleId id = RuleId::External;
        std::string code;
        std::string summary;
        Severity severity = Severity::Warning;
        bool enabled = true;
    };

    struct RuleMetrics
    {
        std::string code;
        std::uint64_t execution_count = 0;
        std::uint64_t total_ns = 0;
        std::uint64_t p95_ns = 0;
        std::size_t diagnostics_emitted = 0;
        std::size_t rejected_diagnostics = 0;
        bool failed = false;
        double AverageNs() const noexcept
        {
            return execution_count ? static_cast<double>(total_ns) / execution_count : 0;
        }
    };

    class DiagnosticSink
    {
    public:
        DiagnosticSink(const AnalysisContext &context, const RuleDescriptor &rule,
            std::vector<Diagnostic> &diagnostics, RuleMetrics &metrics, const LineTable &lines);
        // Validates source and fix bounds and assigns the rule's identity. Invalid
        // diagnostics never reach either frontend. Suppressions apply afterwards.
        bool Emit(Diagnostic diagnostic);
        bool Emit(SourceRange range, std::string message);
    private:
        const AnalysisContext &m_context;
        const RuleDescriptor &m_rule;
        std::vector<Diagnostic> &m_diagnostics;
        RuleMetrics &m_metrics;
        const LineTable &m_lines;
    };

    struct ScheduledRule
    {
        RuleDescriptor metadata;
        std::vector<GrammarKind> interests;
        std::function<void(const AnalysisContext &, NodeId, DiagnosticSink &)> on_node;
        std::function<void(const AnalysisContext &, DiagnosticSink &)> on_document;
        // Only external callbacks are contained by an exception boundary. Native
        // rules retain their direct internal API and are not virtualized.
        bool external = false;
    };

    struct ScheduledResult
    {
        std::vector<Diagnostic> diagnostics;
        std::vector<RuleMetrics> metrics;
        bool cancelled = false;
    };

    class RuleScheduler
    {
    public:
        std::expected<void, std::string> Register(ScheduledRule rule);
        ScheduledResult Analyze(const AnalysisContext &context, const RuleEngine &engine,
            bool profiling = false, std::stop_token stop = {}) const;
        std::size_t Size() const noexcept { return m_rules.size(); }
    private:
        std::vector<ScheduledRule> m_rules;
    };
}
