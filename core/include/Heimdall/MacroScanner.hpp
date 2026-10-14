#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <Heimdall/MacroTable.hpp>
#include <Heimdall/Preprocessor.hpp>

namespace heimdall
{

	/// An `#include` / `#include_next` / `#import` whose operand has been resolved to a spelling.
	struct IncludeRequest
	{
		/// Header name without delimiters.
		std::string header;
		/// `<...>` rather than `"..."`.
		bool angled = false;
		/// `#include_next`.
		bool next = false;
		/// Byte offset of the directive in the scanned text.
		std::size_t offset = 0;
	};

	/// How the scanner reaches the world outside the text it scans. Every member is optional.
	struct MacroScanCallbacks
	{
		/// Scans the named header into the same table. Returns false when it cannot be found.
		std::function<bool(const IncludeRequest&) > include;
		/// `__has_include`: true when the header exists.
		std::function<bool(const IncludeRequest&) > hasInclude;
		/// `__has_cpp_attribute`, `__has_attribute`, `__has_builtin`, `__has_feature`,
		/// `__has_extension`; the result replaces the built-in table.
		std::function<std::optional<long long>(std::string_view feature, std::string_view operand) >
		feature;
		/// Fill `MacroScanResult::directives` and `events`; only the file the parser reads needs them.
		bool recordDirectives = false;
		/// Called after each active `#define`, `#undef` and `#include`, with the offset of the
		/// end of the directive; the table already holds the directive's effect.
		std::function<void(std::size_t offset) > checkpoint;
	};

	/// What scanning one text produced.
	struct MacroScanResult
	{
		/// Source ranges outside inactive conditional groups, in order, merged when adjacent.
		std::vector<ActiveSourceRange> activeRanges;
		std::vector<PreprocessorDiagnostic> diagnostics;
		/// `#pragma once` in an active region.
		bool pragmaOnce = false;
		/// Macro of a classic include guard that wraps the whole text, or empty.
		std::string guard;
		bool cancelled = false;
		/// Every directive line in source order, with whether its conditional group was live
		/// (`callbacks.recordDirectives`).
		std::vector<PreprocessorDirective> directives;
		/// Active `#define` / `#undef` in source order (`callbacks.recordDirectives`).
		std::vector<MacroEvent> events;
	};

	/// Walks the directives of a text in order and applies them to a macro table: conditional
	/// groups are evaluated against the live table (function-like macros, `defined`,
	/// `__has_include`), `#define`/`#undef` update it, and `#include` re-enters through a
	/// callback so macros defined by one header are visible to what follows it.
	class MacroScanner
	{
	public:
		/// Scans `text` (the file named `file`, used only to label definitions).
		[[nodiscard]] static MacroScanResult Scan(
			std::string_view text,
			std::string_view file,
			MacroTable& table,
			const MacroScanCallbacks& callbacks = {},
			std::stop_token stop = {});

		/// Evaluates a `#if` expression against `table`. False for an error or a zero value.
		[[nodiscard]] static bool Evaluate(
			std::string_view expression,
			const MacroTable& table,
			const MacroScanCallbacks& callbacks = {},
			std::vector<PreprocessorDiagnostic>* diagnostics = nullptr);
	};

} // namespace heimdall
