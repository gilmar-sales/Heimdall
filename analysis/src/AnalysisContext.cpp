#include <Heimdall/AnalysisContext.hpp>

#include <algorithm>
#include <cassert>

namespace heimdall
{
    AnalysisContext::AnalysisContext(AnalysisSnapshot snapshot, DocumentId document)
        : m_snapshot(std::move(snapshot)), m_document(document), m_syntax(m_snapshot.Syntax(document)) {}

    const ParseTree &AnalysisContext::Syntax() const
    {
        assert(Valid());
        return *m_syntax;
    }

    const SemanticModel &AnalysisContext::Semantic() const
    {
        assert(Valid());
        return *m_snapshot.Semantic(m_document);
    }

    const TypeModel &AnalysisContext::Types() const
    {
        assert(Valid());
        return *m_snapshot.Types(m_document);
    }

    SourceRange AnalysisContext::NodeRange(NodeId node) const
    {
        const auto &tree = Syntax();
        const auto &soa = tree.NodesSoA();
        if (node >= soa.size() || soa.TokenCount(node) == 0) return {};
        auto first = soa.FirstToken(node);
        auto count = soa.TokenCount(node);
        const auto &tokens = tree.Tokens();
        if (first >= tokens.size() || count > tokens.size() - first) return {};
        const auto &last = tokens[first + count - 1];
        return {tokens[first].offset, last.offset + last.length - tokens[first].offset};
    }

    ScopeId AnalysisContext::ScopeAt(std::size_t offset) const
    {
        const auto &model = Semantic();
        const auto &soa = Syntax().NodesSoA();
        NodeId best = 0;
        for (NodeId node = 0; node < soa.size(); ++node)
        {
            const auto range = NodeRange(node);
            if (range.length && offset >= range.offset && offset < range.offset + range.length) best = node;
        }
        return model.ScopeOfNode(best);
    }

    SymbolId AnalysisContext::ResolveSymbol(std::size_t offset) const
    {
        const auto &tokens = Syntax().Tokens();
        auto found = std::upper_bound(tokens.begin(), tokens.end(), offset,
            [](std::size_t value, const Token &token) { return value < token.offset; });
        if (found == tokens.begin()) return kNone;
        --found;
        if (offset >= found->offset + found->length) return kNone;
        const auto token = static_cast<std::uint32_t>(found - tokens.begin());
        const auto &model = Semantic();
        auto resolved = model.ResolveToken(token);
        if (resolved != kNone) return resolved;
        const auto &symbols = model.Symbols();
        for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
            if (symbols.decl_token[symbol] == token) return symbol;
        return kNone;
    }

    std::vector<SourceRange> AnalysisContext::References(SymbolId symbol) const
    {
        std::vector<SourceRange> ranges;
        const auto &model = Semantic();
        if (symbol >= model.Symbols().Size()) return ranges;
        const auto &refs = model.Refs();
        const auto &tokens = Syntax().Tokens();
        for (std::size_t i = 0; i < refs.target.size(); ++i)
        {
            if (refs.target[i] != symbol || refs.token[i] >= tokens.size()) continue;
            const auto &token = tokens[refs.token[i]];
            ranges.push_back({token.offset, token.length});
        }
        return ranges;
    }

    std::vector<SymbolId> AnalysisContext::SymbolsInScope(std::size_t offset) const
    {
        if (offset > Syntax().Source().size()) return {};
        const auto &model = Semantic();
        const auto &symbols = model.Symbols();
        const auto &scopes = model.Scopes();
        const auto &tokens = Syntax().Tokens();
        std::vector<SymbolId> result;
        for (auto scope = ScopeAt(offset); scope != kNone && scope < scopes.Size(); scope = scopes.parent[scope])
        {
            for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
            {
                if (symbols.scope[symbol] != scope) continue;
                auto token = symbols.decl_token[symbol];
                const bool local = scopes.kind[scope] == ScopeKind::Block || scopes.kind[scope] == ScopeKind::Function;
                if (!local || (token < tokens.size() && tokens[token].offset <= offset)) result.push_back(symbol);
            }
        }
        return result;
    }
}
