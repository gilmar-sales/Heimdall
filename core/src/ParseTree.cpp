#include <Heimdall/Lexer.hpp>
#include <Heimdall/ParseTree.hpp>
#include <Heimdall/Preprocessor.hpp>

#include "detail/GrammarParser.hpp"

#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

namespace heimdall
{

    ParseTree ParseTree::Parse(std::string_view source, CppStandard standard)
    {
        ParserOptions options;
        options.standard = standard;
        return Parse(source, options);
    }

    ParseTree ParseTree::Parse(std::string_view source, const ParserOptions &options)
    {
        return Parse(source, options, std::stop_token{});
    }

    ParseTree ParseTree::Parse(std::string_view source, const ParserOptions &options,
        std::stop_token stop, const std::vector<Token> * lexed, const ParseReuse * reuse)
    {
        ParseTree tree;
        tree.m_source = source;
        tree.m_standard = options.standard;
        tree.m_tokens = lexed != nullptr ? *lexed : Lexer(source).Lex();
        auto preprocessing = Preprocessor(options.Macros()).Process(source);
        detail::ParseWithGrammar(tree, preprocessing, stop, &options.Macros(), reuse);
        tree.m_directives = std::move(preprocessing.directives);

        // Build subtree_end for each node (pre-order traversal property)
        const std::size_t node_count = tree.m_nodes_soa.size();
        for (std::size_t n = 0; n < node_count; ++n)
        {
            tree.m_nodes_soa.subtree_end[n] = static_cast<std::uint32_t>(n + 1);
        }

        for (std::size_t n = node_count; n > 1; --n)
        {
            const std::uint32_t parent = tree.m_nodes_soa.parent[n - 1];
            if (parent < n - 1)
            {
                const std::uint32_t child_end = tree.m_nodes_soa.subtree_end[n - 1];
                if (tree.m_nodes_soa.subtree_end[parent] < child_end)
                {
                    tree.m_nodes_soa.subtree_end[parent] = child_end;
                }
            }
        }

        // Build auxiliary structures for fast queries
        tree.BuildAuxiliary();

        return tree;
    }

    void ParseTree::HoldSource(std::shared_ptr<const std::string> owned)
    {
        m_owned_source = std::move(owned);
        if (m_owned_source && m_source.empty())
        {
            m_source = *m_owned_source;
        }
    }

    bool ParseTree::IsDescendant(std::size_t node_index, std::size_t candidate) const noexcept
    {
        if (node_index >= m_nodes_soa.size() || candidate >= m_nodes_soa.size())
        {
            return false;
        }

        return node_index < candidate && candidate < m_nodes_soa.subtree_end[node_index];
    }

    std::vector<std::size_t> ParseTree::Children(std::size_t node_index) const
    {
        std::vector<std::size_t> children;
        if (node_index >= m_nodes_soa.size())
        {
            return children;
        }

        const std::size_t end = m_nodes_soa.subtree_end[node_index] < m_nodes_soa.size()
            ? m_nodes_soa.subtree_end[node_index]
            : m_nodes_soa.size();
        for (std::size_t i = node_index + 1; i < end; ++i)
        {
            if (m_nodes_soa.parent[i] == node_index)
            {
                children.push_back(i);
            }
        }

        return children;
    }

    void ParseTree::BuildAuxiliary()
    {
        // Token kind mask: 1 = trivia (whitespace/comment), 0 = significant
        m_token_kind_mask.resize(m_tokens.size());
        m_identifier_tokens.clear();
        m_directive_tokens.clear();
        m_identifier_tokens.reserve(m_tokens.size() / 8);
        m_directive_tokens.reserve(m_directives.size() * 4);

        for (std::size_t i = 0; i < m_tokens.size(); ++i)
        {
            const TokenKind kind = m_tokens[i].kind;
            const bool is_trivia = (kind == TokenKind::Whitespace ||
                                   kind == TokenKind::LineComment ||
                                   kind == TokenKind::BlockComment);
            m_token_kind_mask[i] = is_trivia ? 1 : 0;

            if (kind == TokenKind::Identifier)
            {
                m_identifier_tokens.push_back(static_cast<std::uint32_t>(i));
            }
        }

        // Directive token indices
        for (const auto &dir : m_directives)
        {
            // Find tokens within directive range
            for (std::size_t i = 0; i < m_tokens.size(); ++i)
            {
                const std::uint32_t tok_offset = m_tokens[i].offset;
                const std::uint32_t tok_end = tok_offset + m_tokens[i].length;
                if (tok_offset >= dir.offset && tok_end <= dir.offset + dir.length)
                {
                    m_directive_tokens.push_back(static_cast<std::uint32_t>(i));
                }
            }
        }
    }

    void ParseTree::EnsureNodesAoS() const
    {
        if (!m_nodes_aos_dirty)
        {
            return;
        }

        const std::size_t n = m_nodes_soa.size();
        m_nodes_aos.resize(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            m_nodes_aos[i] = {
                static_cast<GrammarKind>(m_nodes_soa.kind[i]),
                m_nodes_soa.first_token[i],
                m_nodes_soa.token_count[i],
                m_nodes_soa.parent[i],
                m_nodes_soa.subtree_end[i]
            };
        }
        m_nodes_aos_dirty = false;
    }

} // namespace heimdall