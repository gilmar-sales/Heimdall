#include <Heimdall/SyntaxTree.hpp>

#include <string_view>

namespace heimdall
{

    namespace
    {

        char ExpectedCloser(char opener)
        {
            switch (opener)
            {
            case '(':
                return ')';
            case '[':
                return ']';
            case '{':
                return '}';
            default:
                return '\0';
            }
        }

        SyntaxKind GroupKind(char opener)
        {
            switch (opener)
            {
            case '(':
                return SyntaxKind::ParenthesizedGroup;
            case '[':
                return SyntaxKind::BracketedGroup;
            default:
                return SyntaxKind::BracedGroup;
            }
        }

    } // namespace

    SyntaxTree SyntaxTree::Parse(std::string_view source, std::size_t max_nesting_depth)
    {
        return Parse(source, CppStandard::Cpp20, max_nesting_depth);
    }

    SyntaxTree SyntaxTree::Parse(std::string_view source, CppStandard standard,
        std::size_t max_nesting_depth)
    {
        SyntaxTree tree;
        tree.m_source = source;
        tree.m_standard = standard;
        tree.m_tokens = Lexer(source).Lex();
        tree.m_nodes.push_back({SyntaxKind::TranslationUnit, 0,
                static_cast<std::uint32_t>(tree.m_tokens.size()), RootNode, 1});
        tree.m_token_parents.resize(tree.m_tokens.size(), RootNode);

        std::vector<std::size_t> group_stack;
        constexpr std::size_t kMaxReserveDepth = 4096;
        group_stack.reserve(max_nesting_depth < kMaxReserveDepth ? max_nesting_depth : kMaxReserveDepth);
        std::size_t current_parent = RootNode;

        for (std::size_t i = 0; i < tree.m_tokens.size(); ++i)
        {
            const Token &token = tree.m_tokens[i];
            const std::string_view text = tree.Text(token);
            if (token.kind != TokenKind::Punctuation || text.size() != 1)
            {
                tree.m_token_parents[i] = current_parent;
                continue;
            }

            const char c = text.front();
            if (ExpectedCloser(c) != '\0')
            {
                tree.m_token_parents[i] = current_parent;
                if (group_stack.size() >= max_nesting_depth)
                {
                    tree.m_diagnostics.push_back({token.offset, "maximum delimiter nesting depth exceeded"});
                    continue;
                }

                const std::size_t node = tree.m_nodes.size();
                tree.m_nodes.push_back({GroupKind(c), static_cast<std::uint32_t>(i), 1,
                        static_cast<std::uint32_t>(current_parent),
                        static_cast<std::uint32_t>(node + 1)});
                tree.m_token_parents[i] = node;
                group_stack.push_back(node);
                current_parent = node;
                continue;
            }

            if (c == ')' || c == ']' || c == '}')
            {
                if (!group_stack.empty())
                {
                    const std::size_t node = group_stack.back();
                    const Token &opener = tree.m_tokens[tree.m_nodes[node].first_token];
                    const char expected = ExpectedCloser(tree.Text(opener).front());
                    if (c == expected)
                    {
                        tree.m_token_parents[i] = node;
                        tree.m_nodes[node].token_count =
                            static_cast<std::uint32_t>(i - tree.m_nodes[node].first_token + 1);
                        group_stack.pop_back();
                        current_parent = tree.m_nodes[node].parent;
                        continue;
                    }
                }

                tree.m_token_parents[i] = current_parent;
                tree.m_nodes.push_back({SyntaxKind::Error, static_cast<std::uint32_t>(i), 1,
                        static_cast<std::uint32_t>(current_parent),
                        static_cast<std::uint32_t>(tree.m_nodes.size() + 1)});
                tree.m_diagnostics.push_back({token.offset, "unmatched closing delimiter"});
                continue;
            }

            tree.m_token_parents[i] = current_parent;
        }

        for (const std::size_t node: group_stack)
        {
            const std::size_t first = tree.m_nodes[node].first_token;
            tree.m_nodes[node].token_count =
                static_cast<std::uint32_t>(tree.m_tokens.size() - first);
            tree.m_diagnostics.push_back({tree.m_tokens[first].offset, "unclosed delimiter"});
        }

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

    void SyntaxTree::HoldSource(std::shared_ptr<const std::string> owned)
    {
        m_owned_source = std::move(owned);
        if (m_owned_source && m_source.empty())
        {
            m_source = *m_owned_source;
        }
    }

    bool SyntaxTree::IsDescendant(std::size_t node_index, std::size_t candidate) const noexcept
    {
        if (node_index >= m_nodes.size() || candidate >= m_nodes.size())
        {
            return false;
        }

        return node_index < candidate && candidate < m_nodes[node_index].subtree_end;
    }

    std::vector<std::size_t> SyntaxTree::Children(std::size_t node_index) const
    {
        std::vector<std::size_t> result;
        if (node_index >= m_nodes.size())
        {
            return result;
        }

        const std::size_t end = m_nodes[node_index].subtree_end < m_nodes.size()
        ? m_nodes[node_index].subtree_end
        : m_nodes.size();
        for (std::size_t i = node_index + 1; i < end; ++i)
        {
            if (m_nodes[i].parent == node_index)
            {
                result.push_back(i);
            }
        }

        return result;
    }

} // namespace heimdall
