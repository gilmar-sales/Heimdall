#pragma once

// Experimental source API v1, deliberately independent of internal tables and
// C++ ABI. This is not an isolation boundary: compiled-in plugins are trusted.
#include <Heimdall/Ids.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace heimdall
{
    inline constexpr std::uint32_t PluginApiVersion = 1;
    inline constexpr std::uint32_t InvalidHandle    = ~0u;

    enum class PluginNodeKind : std::uint8_t
    {
        Other,
        Declaration,
        Function,
        Record,
        Namespace,
        Call,
        Identifier,
        Literal,
        Return
    };

    enum class PluginSeverity : std::uint8_t
    {
        Warning,
        Error
    };

    struct PluginRange
    {
        std::size_t offset = 0;
        std::size_t length = 0;
    };

    struct PluginDiagnostic
    {
        PluginRange range;
        std::string message;
    };

    class PluginContext
    {
      public:
        struct Impl;

        explicit PluginContext(std::shared_ptr<const Impl> impl);

        DocumentId Document() const noexcept;

        PluginNodeKind NodeKind(NodeId node) const;

        PluginRange NodeRange(NodeId node) const;

        std::vector<NodeId> Children(NodeId node) const;

        std::vector<NodeId> Descendants(NodeId node) const;

        SymbolId ResolveSymbol(std::size_t offset) const;

        TypeId SymbolType(SymbolId symbol) const;

        StringId SymbolName(SymbolId symbol) const;

        std::string_view String(StringId string) const;

        std::vector<SymbolId> SymbolsInScope(std::size_t offset) const;

        std::vector<PluginRange> References(SymbolId symbol) const;

      private:
        std::shared_ptr<const Impl> m_impl;
    };

    struct PluginRule
    {
        std::string                 code;
        std::string                 summary;
        PluginSeverity              severity = PluginSeverity::Warning;
        bool                        enabled  = true;
        std::vector<PluginNodeKind> interests;
        // Output is buffered and validated by the host. No internal Diagnostic,
        // ParseTree, SemanticModel or table reference crosses this boundary.
        std::function<void(const PluginContext&, NodeId, std::vector<PluginDiagnostic>&)> on_node;
    };

    struct Plugin
    {
        std::string             name;
        std::uint32_t           required_api = PluginApiVersion;
        std::vector<PluginRule> rules;
    };
} // namespace heimdall
