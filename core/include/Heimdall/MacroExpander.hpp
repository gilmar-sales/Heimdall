#pragma once

#include <Heimdall/Lexer.hpp>
#include <Heimdall/MacroTable.hpp>
#include <Heimdall/Preprocessor.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stop_token>
#include <string_view>
#include <vector>

namespace heimdall
{

	/// Marks `ExpandedToken::spelled` / `invocation` of a token with no source counterpart.
	inline constexpr std::uint32_t kNoToken = std::numeric_limits<std::uint32_t>::max();

	/// Bounds that keep pathological expansions (`#define A B B`) finite.
	struct ExpansionLimits
	{
		/// Tokens produced by one `Expand` call.
		std::size_t maxTokens = 1'000'000;
		/// Nesting of argument pre-expansion.
		std::size_t maxDepth = 256;
	};

	/// A token of the expansion with its provenance.
	struct ExpandedToken
	{
		/// Spelling; points into the input text, a macro body or the result's pool.
		std::string_view text;
		TokenKind kind   = TokenKind::Unknown;
		Tok tok          = Tok::None;
		bool spaceBefore = false;
		/// Index of the input token this token was copied from, or `kNoToken` when a macro body
		/// or `#`/`##` created it.
		std::uint32_t spelled = kNoToken;
		/// Index of the input token naming the outermost macro whose expansion produced this
		/// token, or `kNoToken` when no macro was involved.
		std::uint32_t invocation = kNoToken;
	};

	/// Tokens after macro replacement. Spellings stay valid while the result and the input text
	/// live.
	struct ExpansionResult
	{
		std::vector<ExpandedToken> tokens;
		std::vector<PreprocessorDiagnostic> diagnostics;
		/// True when a limit or cancellation stopped the expansion early.
		bool truncated = false;
		/// Keeps pooled spellings and macro definitions alive.
		std::shared_ptr<void> storage;
	};

	/// Function-like and object-like macro replacement with the C++ rules for `#`, `##`,
	/// `__VA_ARGS__`, `__VA_OPT__` and rescanning (hide-set algorithm; no recursion on the
	/// call stack except for argument pre-expansion, which is bounded by `maxDepth`).
	class MacroExpander
	{
	public:
		explicit MacroExpander(const MacroTable& table, ExpansionLimits limits = {}) :
		mTable(table), mLimits(limits) {}

		/// Expands the whole of `text`. With `protectDefined`, the operand of `defined` in a
		/// `#if` expression is left alone.
		[[nodiscard]] ExpansionResult Expand(std::string_view text, bool protectDefined = false,
			std::stop_token stop = {}) const;

		/// Expands lexed `tokens` of `text` (trivia allowed); `spelled` indexes `tokens`.
		[[nodiscard]] ExpansionResult Expand(
			std::string_view text,
			std::span<const Token> tokens,
			bool protectDefined = false,
			std::stop_token stop = {}) const;

	private:
		const MacroTable& mTable;
		ExpansionLimits mLimits;
	};

} // namespace heimdall
