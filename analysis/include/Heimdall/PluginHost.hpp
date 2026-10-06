#pragma once

#include <Heimdall/PluginApi.hpp>
#include <Heimdall/RuleScheduler.hpp>

namespace heimdall
{
    // Register during setup, then analyze concurrently through const methods.
    // External ABI/runtime selection is intentionally deferred until measured.
    class PluginHost
    {
    public:
        std::expected<void, std::string> Register(Plugin plugin);

        std::expected<void, std::string> RegisterNative(ScheduledRule rule);

        ScheduledResult Analyze(
            const AnalysisContext& context,
            const RuleEngine& engine,
            bool profiling = false,
            std::stop_token stop = {}) const;

        std::size_t Size() const noexcept
        {
            return m_names.size();
        }

    private:
        RuleScheduler m_scheduler;
        std::vector<std::string> m_names;
    };
}
