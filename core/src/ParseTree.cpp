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
        ParseTree tree;
        tree.m_source = source;
        tree.m_standard = options.standard;
        tree.m_tokens = Lexer(source).Lex();
        auto preprocessing = Preprocessor(options.Macros()).Process(source);
        detail::ParseWithGrammar(tree, preprocessing);
        tree.m_directives = std::move(preprocessing.directives);

        for (std::size_t n = 0; n < tree.m_nodes.size(); ++n)
        {
            tree.m_nodes[n].subtree_end = static_cast<std::uint32_t>(n + 1);
        }

        for (std::size_t n = tree.m_nodes.size(); n > 1; --n)
        {
            const std::uint32_t parent = tree.m_nodes[n - 1].parent;
            if (parent < n - 1)
            {
                const std::uint32_t child_end = tree.m_nodes[n - 1].subtree_end;
                if (tree.m_nodes[parent].subtree_end < child_end)
                {
                    tree.m_nodes[parent].subtree_end = child_end;
                }
            }
        }

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
        if (node_index >= m_nodes.size() || candidate >= m_nodes.size())
        {
            return false;
        }

        return node_index < candidate && candidate < m_nodes[node_index].subtree_end;
    }

    std::vector<std::size_t> ParseTree::Children(std::size_t node_index) const
    {
        std::vector<std::size_t> children;
        if (node_index >= m_nodes.size())
        {
            return children;
        }

        const std::size_t end = m_nodes[node_index].subtree_end < m_nodes.size()
        ? m_nodes[node_index].subtree_end
        : m_nodes.size();
        for (std::size_t i = node_index + 1; i < end; ++i)
        {
            if (m_nodes[i].parent == node_index)
            {
                children.push_back(i);
        }}

        return children;
    }

} // namespace heimdall
