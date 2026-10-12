#pragma once

#include <Heimdall/ParseTree.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace heimdall
{

	/// Highlight category; the order is the LSP semantic token legend order.
	enum class HighlightKind : std::uint8_t
	{
		Keyword,
		Number,
		String,
		Comment,
		Operator,
		Macro,
		Namespace,
		Type,
		Class,
		Struct,
		Enum,
		EnumMember,
		Function,
		Method,
		Property,
		Variable,
		Count
	};

	/// Bit flags combined in `HighlightSpan::modifiers`.
	inline constexpr std::uint8_t kHighlightDeclaration = 1;

	/// A classified byte range of the source; ranges never overlap.
	struct HighlightSpan
	{
		std::size_t offset     = 0;
		std::size_t length     = 0;
		HighlightKind kind     = HighlightKind::Keyword;
		std::uint8_t modifiers = 0;
	};

	/// Name of a kind in the LSP semantic token legend (`keyword`, `enumMember`, ...).
	[[nodiscard]] std::string_view HighlightKindName(HighlightKind kind) noexcept;

	/// Lexical and syntactic classification of a `ParseTree` (core, STL only). Works on
	/// incomplete code and needs neither a compile database nor semantic analysis.
	class SyntaxHighlight
	{
	public:
		/// Spans in source order. Whitespace and unclassified identifiers produce no span.
		[[nodiscard]] static std::vector<HighlightSpan> Extract(const ParseTree& tree);
	};

} // namespace heimdall
