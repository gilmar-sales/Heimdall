#include <Heimdall/Lexer.hpp>
#include <Heimdall/ParseTree.hpp>
#include <Heimdall/Preprocessor.hpp>
#include <algorithm>

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

	ParseTree ParseTree::Parse(std::string_view source, const ParserOptions& options)
	{
		return Parse(source, options, std::stop_token {});
	}

	ParseTree ParseTree::Parse(
		std::string_view source,
		const ParserOptions& options,
		std::stop_token stop,
		const std::vector<Token>* lexed,
		const ParseReuse* reuse)
	{
		ParseTree tree;
		tree.m_source   = source;
		tree.m_standard = options.standard;
		tree.m_tokens   = lexed != nullptr ? *lexed : Lexer(source).Lex();
		tree.Build(options, stop, reuse);
		return tree;
	}

	ParseTree ParseTree::ParseSnapshot(
		std::shared_ptr<const std::string> source,
		const ParserOptions& options,
		std::stop_token stop,
		std::shared_ptr<const std::vector<Token>>
		lexed,
		const ParseReuse* reuse)
	{
		ParseTree tree;
		tree.m_owned_source = std::move(source);
		tree.m_source =
			tree.m_owned_source ? std::string_view(*tree.m_owned_source) : std::string_view {};
		tree.m_standard      = options.standard;
		tree.m_shared_tokens = std::move(lexed);
		if (!tree.m_shared_tokens)
		{
			tree.m_tokens = Lexer(tree.m_source).Lex();
		}

		tree.Build(options, stop, reuse);
		return tree;
	}

	void ParseTree::Build(const ParserOptions& options, std::stop_token stop,
		const ParseReuse* reuse)
	{
		auto& tree = *this;
		if (stop.stop_requested())
		{
			m_cancelled = true;
			return;
		}

		if (reuse && reuse->previous)
		{
			// Reusing a similarly sized document avoids five independent growth
			// chains. Cap the hint when a large portion of the source was deleted.
			tree.m_nodes_soa.reserve(
				std::min(reuse->previous->NodesSoA().size(), tree.Tokens().size() * 2 + 1));
		}

		auto preprocessing = Preprocessor(options.Macros()).Process(tree.m_source);
		if (stop.stop_requested())
		{
			m_cancelled = true;
			return;
		}

		detail::ParseWithGrammar(
			tree, preprocessing, stop, &options.Macros(), reuse, options.type_names.get());
		tree.m_directives = std::move(preprocessing.directives);
		if (m_cancelled || stop.stop_requested())
		{
			m_cancelled = true;
			return;
		}

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
	}

	std::size_t ParseTree::StorageBytes() const noexcept
	{
		std::size_t bytes =
			m_nodes_soa.kind.capacity() +
			sizeof(std::uint32_t) *
			(m_nodes_soa.first_token.capacity() + m_nodes_soa.token_count.capacity() +
			m_nodes_soa.parent.capacity() + m_nodes_soa.subtree_end.capacity());
		bytes +=
			Tokens().capacity() * sizeof(Token) + m_items.capacity() * sizeof(TopLevelItem) +
			m_directives.capacity() * sizeof(PreprocessorDirective) +
			m_diagnostics.capacity() * sizeof(GrammarDiagnostic) + m_token_kind_mask.capacity() +
			sizeof(std::uint32_t) *
			(m_identifier_tokens.capacity() + m_directive_tokens.capacity()) +
			(m_decoration.capacity() + 7) / 8;
		for (const auto& diagnostic : m_diagnostics)
		{
			bytes += diagnostic.message.capacity();
		}

		return bytes;
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
		for (const auto child : DirectChildren(node_index))
		{
			children.push_back(child);
		}

		return children;
	}

	ParseTree::ChildRange ParseTree::DirectChildren(std::size_t node_index) const noexcept
	{
		if (node_index >= m_nodes_soa.size())
			return {&m_nodes_soa, 0, 0, node_index};
		const auto end =
			std::min<std::size_t>(m_nodes_soa.SubtreeEnd(node_index), m_nodes_soa.size());
		return {&m_nodes_soa, std::min(node_index + 1, end), end, node_index};
	}

	void ParseTree::ChildRange::Iterator::Seek() noexcept
	{
		// Expression parsing can reparent an earlier operand to a later node.
		// Such nodes are not strict pre-order: retain the parent filter instead
		// of treating every index at a subtree boundary as a direct child.
		while (m_index < m_end && m_soa->Parent(m_index) != m_parent)
		{
			++m_index;
		}
	}

	ParseTree::ChildRange::Iterator & ParseTree::ChildRange::Iterator::operator++() noexcept
	{
		m_index = std::min<std::size_t>(m_soa->SubtreeEnd(m_index), m_end);
		Seek();
		return *this;
	}

	void ParseTree::BuildAuxiliary()
	{
		const auto& m_tokens = Tokens();
		// Token kind mask: 1 = trivia (whitespace/comment), 0 = significant
		m_token_kind_mask.resize(m_tokens.size());
		m_identifier_tokens.clear();
		m_directive_tokens.clear();
		m_identifier_tokens.reserve(m_tokens.size() / 8);
		m_directive_tokens.reserve(m_directives.size() * 4);

		for (std::size_t i = 0; i < m_tokens.size(); ++i)
		{
			const TokenKind kind = m_tokens[i].kind;
			const bool is_trivia =
				(kind == TokenKind::Whitespace || kind == TokenKind::LineComment ||
				kind == TokenKind::BlockComment);
			m_token_kind_mask[i] = is_trivia ? 1 : 0;

			if (kind == TokenKind::Identifier)
			{
				m_identifier_tokens.push_back(static_cast<std::uint32_t>(i));
			}
		}

		// Directive token indices. Tokens are ordered by offset, so each
		// directive's range is found by binary search instead of a full scan.
		for (const auto& dir : m_directives)
		{
			const auto first = std::lower_bound(
				m_tokens.begin(), m_tokens.end(), dir.offset,
				[](const Token& token, std::size_t offset)
				{
					return token.offset < offset;
			});
			for (auto it = first; it != m_tokens.end(); ++it)
			{
				const std::size_t tok_end = static_cast<std::size_t>(it->offset) + it->length;
				if (tok_end > dir.offset + dir.length)
				{
					break;
				}

				m_directive_tokens.push_back(static_cast<std::uint32_t>(it - m_tokens.begin()));
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
			m_nodes_aos[i] = {static_cast<GrammarKind>(m_nodes_soa.kind[i]),
				m_nodes_soa.first_token[i], m_nodes_soa.token_count[i],
				m_nodes_soa.parent[i], m_nodes_soa.subtree_end[i]};
		}

		m_nodes_aos_dirty = false;
	}

} // namespace heimdall
