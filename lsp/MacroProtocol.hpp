#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <Heimdall/MacroEnvironment.hpp>
#include <Heimdall/MacroTable.hpp>

namespace heimdall::lsp
{

	/// Byte range of a word in a text.
	struct WordRange
	{
		std::size_t begin = 0;
		std::size_t end   = 0;
	};

	/// The identifier that contains or ends at `offset`, if any.
	[[nodiscard]] std::optional<WordRange> IdentifierAt(std::string_view text, std::size_t offset);

	/// The macro named at `offset`, as live there. On a directive line (`#define X`, `#ifdef X`)
	/// the state after the directive is used, so the defining name resolves to itself.
	[[nodiscard]] const MacroDef* MacroAt(
		const MacroEnvironment& env,
		std::string_view text,
		WordRange word);

	/// `#define NAME(a, b) body`, with a long body cut short.
	[[nodiscard]] std::string MacroSignature(const MacroDef& macro);

	/// What the invocation that starts at `word` expands to, cut to `maxChars`; empty when
	/// the expansion is the same text (a function-like name used without arguments).
	[[nodiscard]] std::string MacroExpansionPreview(
		const MacroEnvironment& env,
		std::string_view text,
		WordRange word,
		const MacroDef& macro,
		std::size_t maxChars = 240);

	/// A text replacement for an invocation, in bytes of the document.
	struct MacroExpansionEdit
	{
		std::size_t begin = 0;
		std::size_t end   = 0;
		std::string text;
	};

	/// The invocation at `word` replaced by exactly what the preprocessor produces, or nothing
	/// when that is not safe to write back: an incomplete or unterminated call, a diagnostic,
	/// a limit hit, a multi-line result, or a use on a directive line.
	[[nodiscard]] std::optional<MacroExpansionEdit> MacroExpansionForEdit(
		const MacroEnvironment& env,
		std::string_view text,
		WordRange word,
		const MacroDef& macro);

	/// Markdown for a hover: signature, expansion and where the macro comes from.
	[[nodiscard]] std::string MacroHoverMarkdown(const MacroDef& macro, std::string_view expansion);

	/// The macro's name inside its `#define` line of `fileText`; `directiveOffset` is the start
	/// of that line.
	[[nodiscard]] std::optional<WordRange> DefinitionNameRange(
		std::string_view fileText,
		std::size_t directiveOffset,
		std::string_view name);

} // namespace heimdall::lsp
