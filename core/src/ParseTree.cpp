#include <Heimdall/ParseTree.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <Heimdall/Lexer.hpp>
#include <Heimdall/MacroTable.hpp>
#include <Heimdall/Preprocessor.hpp>
#include <Heimdall/Trace.hpp>

#include "detail/GrammarParser.hpp"

namespace heimdall
{
	using ParseClock = std::chrono::steady_clock;

	std::string ParseTree::ObjectReplacement(std::string_view name, std::size_t offset) const
	{
		if (m_macro_resolver)
		{
			const auto* macro = m_macro_resolver->Find(name, offset);
			return macro && !macro->functionLike ? macro->body : std::string {};
		}

		std::string replacement(PredefinedReplacement(name));
		for (const auto& event : mPreprocessing.macro_events)
		{
			if (event.offset > offset)
			{
				break;
			}

			if (!event.define)
			{
				if (event.text == name)
				{
					replacement.clear();
				}

				continue;
			}

			const auto macro = MacroTable::ParseDefinition(event.text);
			if (macro && macro->name == name)
			{
				replacement = macro->functionLike ? std::string {}: macro->body;
			}
		}

		return replacement;
	}

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
		if (source.size() > kMaxSourceBytes)
		{
			tree.m_diagnostics.emplace_back(0, "source exceeds the 32-bit token offset limit");
			return tree;
		}

		if (stop.stop_requested())
		{
			tree.m_cancelled = true;
			return tree;
		}

		const auto lexStart = ParseClock::now();
		{
			HEIMDALL_TRACE_SCOPE(Parse, "Lex");
			tree.m_tokens = lexed != nullptr ? *lexed : Lexer(source).Lex(stop);
		}

		tree.mMetrics.lexNs = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(ParseClock::now() - lexStart)
			.count());
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
		tree.m_standard = options.standard;
		if (tree.m_source.size() > kMaxSourceBytes)
		{
			tree.m_diagnostics.emplace_back(0, "source exceeds the 32-bit token offset limit");
			return tree;
		}

		tree.m_shared_tokens = std::move(lexed);
		if (!tree.m_shared_tokens)
		{
			const auto lexStart = ParseClock::now();
			{
				HEIMDALL_TRACE_SCOPE(Parse, "Lex");
				tree.m_tokens = Lexer(tree.m_source).Lex(stop);
			}

			tree.mMetrics.lexNs = static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(ParseClock::now() - lexStart)
				.count());
		}

		tree.Build(options, stop, reuse);
		return tree;
	}

	void ParseTree::Build(
		const ParserOptions& options,
		std::stop_token stop,
		const ParseReuse* reuse)
	{
		HEIMDALL_TRACE_SCOPE(Parse, "ParseTree::Build");
		auto& tree             = *this;
		tree.m_type_names      = options.type_names;
		tree.m_macro_resolver  = options.macro_resolver;
		tree.mPredefinedMacros = options.shared_macros;
		if (!tree.mPredefinedMacros && !options.predefined_macros.empty())
		{
			tree.mPredefinedMacros = std::make_shared<const Preprocessor::MacroMap>(options.predefined_macros);
		}

		if (stop.stop_requested())
		{
			m_cancelled = true;
			return;
		}

		if (reuse != nullptr && reuse->previous)
		{
			// Reusing a similarly sized document avoids five independent growth
			// chains. Cap the hint when a large portion of the source was deleted.
			tree.m_nodes_soa.reserve(
				std::min(reuse->previous->NodesSoA().size(), tree.Tokens().size() * 2 + 1));
		}

		const auto preprocessingStart = ParseClock::now();
		PreprocessorResult preprocessing;
		if (options.macro_resolver && options.macro_resolver->Preprocessing() != nullptr)
		{
			// Conditions were evaluated with the macros of the whole translation unit.
			preprocessing = *options.macro_resolver->Preprocessing();
		}
		else if (reuse != nullptr && reuse->previous && reuse->directivesUnchanged && !reuse->previous->Cancelled())
		{
			preprocessing     = reuse->previous->mPreprocessing;
			const auto oldEnd = reuse->offset + reuse->old_length;
			const auto shift = static_cast<std::ptrdiff_t>(reuse->new_length) -
				static_cast<std::ptrdiff_t>(reuse->old_length);
			for (auto& range : preprocessing.active_ranges)
			{
				if (range.offset <= reuse->offset && range.offset + range.length >= oldEnd)
				{
					range.length =
						static_cast<std::size_t>(static_cast<std::ptrdiff_t>(range.length) + shift);
				}
				else if (range.offset >= oldEnd)
				{
					range.offset =
						static_cast<std::size_t>(static_cast<std::ptrdiff_t>(range.offset) + shift);
				}
			}

			for (auto& directive : preprocessing.directives)
			{
				if (directive.offset >= oldEnd)
				{
					directive.offset = static_cast<std::size_t>(
						static_cast<std::ptrdiff_t>(directive.offset) + shift);
				}
			}

			for (auto& event : preprocessing.macro_events)
			{
				if (event.offset >= oldEnd)
				{
					event.offset = static_cast<std::size_t>(
						static_cast<std::ptrdiff_t>(event.offset) + shift);
				}
			}

			for (auto& diagnostic : preprocessing.diagnostics)
			{
				if (diagnostic.offset >= oldEnd)
				{
					diagnostic.offset = static_cast<std::size_t>(
						static_cast<std::ptrdiff_t>(diagnostic.offset) + shift);
				}
			}

			mMetrics.reusedPreprocessing = true;
		}
		else
		{
			preprocessing = Preprocessor(options.Macros()).Process(tree.m_source, false, stop);
		}

		mMetrics.preprocessingNs =
			static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			ParseClock::now() - preprocessingStart)
			.count());
		if (stop.stop_requested())
		{
			m_cancelled = true;
			return;
		}

		const auto grammarStart = ParseClock::now();
		detail::ParseWithGrammar(tree, preprocessing, stop, &options.Macros(), reuse,
			options.type_names.get(), options.macro_resolver.get());
		mMetrics.grammarNs = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(ParseClock::now() - grammarStart)
			.count());
		tree.mPreprocessing = std::move(preprocessing);
		if (m_cancelled || stop.stop_requested())
		{
			m_cancelled = true;
			return;
		}

		// Build subtree_end for each node (pre-order traversal property)
		const auto auxiliaryStart    = ParseClock::now();
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
		mMetrics.auxiliaryNs = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(ParseClock::now() - auxiliaryStart)
			.count());
	}

	std::size_t ParseTree::StorageBytes() const noexcept
	{
		std::size_t bytes =
			m_nodes_soa.kind.capacity() +
			sizeof(std::uint32_t) *
			(m_nodes_soa.first_token.capacity() + m_nodes_soa.token_count.capacity() +
			m_nodes_soa.parent.capacity() + m_nodes_soa.subtree_end.capacity());
		bytes += Tokens().capacity() * sizeof(Token) + m_items.capacity() * sizeof(TopLevelItem) +
			mPreprocessing.directives.capacity() * sizeof(PreprocessorDirective) +
			m_diagnostics.capacity() * sizeof(GrammarDiagnostic) +
			m_token_kind_mask.capacity() +
			sizeof(std::uint32_t) *
			(m_identifier_tokens.capacity() + m_directive_tokens.capacity()) +
			(m_decoration.capacity() + 7) / 8;

		for (const auto& diagnostic : m_diagnostics)
		{
			bytes += diagnostic.message.capacity();
		}

		bytes += mPreprocessing.active_ranges.capacity() * sizeof(ActiveSourceRange) +
			mPreprocessing.diagnostics.capacity() * sizeof(PreprocessorDiagnostic);
		for (const auto& diagnostic : mPreprocessing.diagnostics)
		{
			bytes += diagnostic.message.capacity();
		}

		for (const auto& [name, replacement] : mPreprocessing.local_macros)
		{
			bytes += name.capacity() + replacement.capacity() +
				sizeof(decltype(mPreprocessing.local_macros)::value_type);
		}

		const std::lock_guard lock(*mNodesAoSMutex);
		bytes += m_nodes_aos.capacity() * sizeof(GrammarNode);
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

	RequirementKind ParseTree::RequirementKindOf(std::size_t node) const noexcept
	{
		if (node >= m_nodes_soa.size() || m_nodes_soa.Kind(node) != GrammarKind::Requirement)
		{
			return RequirementKind::Simple;
		}

		const auto first   = m_nodes_soa.FirstToken(node);
		const auto& tokens = Tokens();
		if (first >= tokens.size())
		{
			return RequirementKind::Simple;
		}

		switch (tokens[first].tok)
		{
		case Tok::LBrace:
			return RequirementKind::Compound;
		case Tok::KwTypename:
			return RequirementKind::Type;
		case Tok::KwRequires:
			return RequirementKind::Nested;
		default:
			return RequirementKind::Simple;
		}
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
		m_directive_tokens.reserve(mPreprocessing.directives.size() * 4);

		for (std::size_t i = 0; i < m_tokens.size(); ++i)
		{
			const TokenKind kind = m_tokens[i].kind;
			const bool is_trivia =
				(kind == TokenKind::Whitespace || kind == TokenKind::LineComment ||
				kind == TokenKind::BlockComment);
			m_token_kind_mask[i] = is_trivia ? 1 : 0;

			if (kind == TokenKind::Identifier)
			{
				m_identifier_tokens.emplace_back(i);
			}
		}

		// Directive token indices. Tokens are ordered by offset, so each
		// directive's range is found by binary search instead of a full scan.
		for (const auto& dir : mPreprocessing.directives)
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
		const std::lock_guard lock(*mNodesAoSMutex);
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
