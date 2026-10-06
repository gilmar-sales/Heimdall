#pragma once

#include <Heimdall/Workspace.hpp>

namespace heimdall
{
    struct SourceRange
    {
        std::size_t offset = 0;
        std::size_t length = 0;
    };

    // Native API: direct immutable SoA views, no plugin crossings or second AST.
    // Owns the snapshot and cache objects underlying all returned references.
    class AnalysisContext
    {
    public:
        AnalysisContext(AnalysisSnapshot snapshot, DocumentId document);

        bool Valid() const noexcept
        {
            return m_snapshot.Contains(m_document);
        }

        DocumentId Document() const noexcept
        {
            return m_document;
        }

        const AnalysisSnapshot& Snapshot() const noexcept
        {
            return m_snapshot;
        }

        const ParseTree& Syntax() const;

        const SemanticModel& Semantic() const;

        const TypeModel& Types() const;

        const SymbolTable& Symbols() const
        {
            return Semantic().Symbols();
        }

        SourceRange NodeRange(NodeId node) const;

        ScopeId ScopeAt(std::size_t offset) const;

        SymbolId ResolveSymbol(std::size_t offset) const;

        std::vector<SourceRange> References(SymbolId symbol) const;

        std::vector<SymbolId> SymbolsInScope(std::size_t offset) const;

    private:
        AnalysisSnapshot m_snapshot;
        DocumentId m_document;
        std::shared_ptr<const ParseTree> m_syntax;
    };
}
