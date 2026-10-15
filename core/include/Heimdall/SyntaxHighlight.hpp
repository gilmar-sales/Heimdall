#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include <Heimdall/MacroTable.hpp>
#include <Heimdall/ParseTree.hpp>
#include <Heimdall/Preprocessor.hpp>

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
		Parameter,
		Count
	};

	/// Bit flags combined in `HighlightSpan::modifiers`.
	inline constexpr std::uint8_t kHighlightDeclaration = 1;
	/// The span lies in a group excluded by conditional compilation.
	inline constexpr std::uint8_t kHighlightInactive = 2;

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

	/// Knowledge from outside the file: which macros are live, and which lines are compiled.
	struct HighlightContext
	{
		/// The macro `name` names at byte `offset`, or null; may be empty.
		std::function<const MacroDef * (std::string_view name, std::size_t offset) > findMacro;
		/// Source ranges outside inactive conditional groups; empty means everything is active.
		std::span<const ActiveSourceRange> activeRanges;
	};

	/// Lexical and syntactic classification of a `ParseTree` (core, STL only). Works on
	/// incomplete code and needs neither a compile database nor semantic analysis.
	class SyntaxHighlight
	{
	public:
		/// Spans in source order. Whitespace and unclassified identifiers produce no span.
		[[nodiscard]] static std::vector<HighlightSpan> Extract(const ParseTree& tree);

		/// As above, with macros resolved through `context` (any header, any `-D`) and spans
		/// in inactive groups marked `kHighlightInactive`. Identifiers keep their
		/// normal kind plus the inactive flag; names without a reliable kind are
		/// emitted as `variable` only inside inactive groups so the client can dim
		/// the whole region.
		[[nodiscard]] static std::vector<HighlightSpan> Extract(
			const ParseTree& tree,
			const HighlightContext& context);
	};

} // namespace heimdall
