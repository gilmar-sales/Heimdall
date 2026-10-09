#include <Heimdall/PluginHost.hpp>

#include <algorithm>

namespace heimdall
{
    struct PluginContext::Impl
    {
        AnalysisContext context;

        explicit Impl(AnalysisContext analysis) : context(std::move(analysis)) {}
    };

    namespace
    {
        PluginNodeKind PublicKind(GrammarKind kind)
        {
            switch (kind)
            {
                case GrammarKind::Declaration:
                    return PluginNodeKind::Declaration;
                case GrammarKind::FunctionDefinition:
                case GrammarKind::FunctionDeclaration:
                    return PluginNodeKind::Function;
                case GrammarKind::RecordDefinition:
                    return PluginNodeKind::Record;
                case GrammarKind::NamespaceDefinition:
                    return PluginNodeKind::Namespace;
                case GrammarKind::CallExpression:
                    return PluginNodeKind::Call;
                case GrammarKind::IdentifierExpression:
                    return PluginNodeKind::Identifier;
                case GrammarKind::LiteralExpression:
                    return PluginNodeKind::Literal;
                case GrammarKind::ReturnStatement:
                    return PluginNodeKind::Return;
                default:
                    return PluginNodeKind::Other;
            }
        }
    } // namespace

    PluginContext::PluginContext(std::shared_ptr<const Impl> impl) : m_impl(std::move(impl))
    {
    }

    DocumentId PluginContext::Document() const noexcept
    {
        return m_impl ? m_impl->context.Document() : InvalidDocument;
    }

    PluginNodeKind PluginContext::NodeKind(NodeId node) const
    {
        if (!m_impl || !m_impl->context.Valid())
        {
            return PluginNodeKind::Other;
        }

        const auto& soa = m_impl->context.Syntax().NodesSoA();
        return node < soa.size() ? PublicKind(soa.Kind(node)) : PluginNodeKind::Other;
    }

    PluginRange PluginContext::NodeRange(NodeId node) const
    {
        if (!m_impl || !m_impl->context.Valid())
            return {};
        auto range = m_impl->context.NodeRange(node);
        return { range.offset, range.length };
    }

    std::vector<NodeId> PluginContext::Children(NodeId node) const
    {
        std::vector<NodeId> result;
        if (!m_impl || !m_impl->context.Valid())
        {
            return result;
        }

        const auto& tree = m_impl->context.Syntax();
        if (node >= tree.NodesSoA().size())
        {
            return result;
        }

        for (auto child : tree.DirectChildren(node))
        {
            result.push_back(static_cast<NodeId>(child));
        }

        return result;
    }

    std::vector<NodeId> PluginContext::Descendants(NodeId node) const
    {
        std::vector<NodeId> result;
        if (!m_impl || !m_impl->context.Valid())
        {
            return result;
        }

        const auto& soa = m_impl->context.Syntax().NodesSoA();
        if (node >= soa.size())
        {
            return result;
        }

        for (NodeId id = node + 1; id < soa.SubtreeEnd(node) && id < soa.size(); ++id)
        {
            result.push_back(id);
        }

        return result;
    }

    SymbolId PluginContext::ResolveSymbol(std::size_t offset) const
    {
        return m_impl && m_impl->context.Valid() ? m_impl->context.ResolveSymbol(offset)
                                                 : InvalidHandle;
    }

    TypeId PluginContext::SymbolType(SymbolId symbol) const
    {
        return m_impl && m_impl->context.Valid() ? m_impl->context.Types().SymbolType(symbol) : 0;
    }

    StringId PluginContext::SymbolName(SymbolId symbol) const
    {
        if (!m_impl || !m_impl->context.Valid())
        {
            return InvalidHandle;
        }

        const auto& symbols = m_impl->context.Symbols();
        return symbol < symbols.Size() ? symbols.name[symbol] : InvalidHandle;
    }

    std::string_view PluginContext::String(StringId string) const
    {
        return m_impl && m_impl->context.Valid() ? m_impl->context.Semantic().Names().Text(string)
                                                 : std::string_view {};
    }

    std::vector<SymbolId> PluginContext::SymbolsInScope(std::size_t offset) const
    {
        return m_impl && m_impl->context.Valid() ? m_impl->context.SymbolsInScope(offset)
                                                 : std::vector<SymbolId> {};
    }

    std::vector<PluginRange> PluginContext::References(SymbolId symbol) const
    {
        std::vector<PluginRange> result;
        if (m_impl && m_impl->context.Valid())
        {
            for (auto range : m_impl->context.References(symbol))
            {
                result.push_back({ range.offset, range.length });
            }
        }

        return result;
    }

    std::expected<void, std::string> PluginHost::Register(Plugin plugin)
    {
        if (plugin.required_api != PluginApiVersion)
        {
            return std::unexpected("Unsupported plugin API version");
        }

        if (plugin.name.empty() ||
            std::find(m_names.begin(), m_names.end(), plugin.name) != m_names.end())
        {
            return std::unexpected("Empty or duplicate plugin name");
        }

        // Transactional: a bad rule must not leave half a plugin installed.
        auto scheduler = m_scheduler;
        for (auto& rule : plugin.rules)
        {
            if (FindRuleByCode(rule.code))
            {
                return std::unexpected("Plugin shadows a native rule: " + rule.code);
            }

            if (rule.interests.empty() || !rule.on_node)
            {
                return std::unexpected("Plugin rule needs interests and a callback");
            }

            ScheduledRule scheduled;
            scheduled.metadata.code    = rule.code;
            scheduled.metadata.summary = rule.summary;
            scheduled.metadata.severity =
                rule.severity == PluginSeverity::Error ? Severity::Error : Severity::Warning;
            scheduled.metadata.enabled = rule.enabled;
            scheduled.external         = true;
            for (unsigned kind = 0; kind <= static_cast<unsigned>(GrammarKind::CastExpression);
                 ++kind)
            {
                if (std::find(rule.interests.begin(), rule.interests.end(),
                              PublicKind(static_cast<GrammarKind>(kind))) != rule.interests.end())
                {
                    scheduled.interests.push_back(static_cast<GrammarKind>(kind));
                }
            }

            scheduled.on_node =
                [callback = std::move(rule.on_node)](
                    const AnalysisContext& context, NodeId node, DiagnosticSink& sink) {
                    PluginContext public_context(std::make_shared<PluginContext::Impl>(context));
                    std::vector<PluginDiagnostic> diagnostics;
                    callback(public_context, node, diagnostics);
                    for (auto& diagnostic : diagnostics)
                    {
                        sink.Emit({ diagnostic.range.offset, diagnostic.range.length },
                                  std::move(diagnostic.message));
                    }
                };
            auto registered = scheduler.Register(std::move(scheduled));
            if (!registered)
            {
                return registered;
            }
        }

        m_scheduler = std::move(scheduler);
        m_names.push_back(std::move(plugin.name));
        return {};
    }

    ScheduledResult PluginHost::Analyze(const AnalysisContext& context,
                                        const RuleEngine&      engine,
                                        bool                   profiling,
                                        std::stop_token        stop) const
    {
        return m_scheduler.Analyze(context, engine, profiling, stop);
    }

    std::expected<void, std::string> PluginHost::RegisterNative(ScheduledRule rule)
    {
        rule.external = false;
        return m_scheduler.Register(std::move(rule));
    }
} // namespace heimdall
