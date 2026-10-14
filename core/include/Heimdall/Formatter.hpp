#pragma once

#include <cstddef>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <Heimdall/ParseTree.hpp>
#include <Heimdall/RuleEngine.hpp>

namespace heimdall
{

	enum class PointerAlignment
	{
		Left,
		Right,
	};

	enum class ReferenceAlignment
	{
		Left,
		Right,
	};

	enum class BraceStyle
	{
		Attach,
		Allman,
	};

	// How single-statement control blocks (`if`/`else`/`for`/`while`/`do`
	// with one statement and no braces) are treated.
	enum class SingleLineStyle
	{
		Keep,
		SingleLine,
		Indent,
		IndentWithBraces,
	};

	struct FormatOptions
	{
		bool operator==(const FormatOptions&) const noexcept = default;
		static constexpr std::size_t kDefaultIndentWidth     = 4;
		static constexpr std::size_t kDefaultColumnLimit     = 100;

		std::size_t indent_width    = kDefaultIndentWidth;
		bool use_tabs               = false;
		std::size_t max_empty_lines = 1;
		// Maximum line width before comma-driven breaks; 0 disables breaking.
		std::size_t column_limit               = kDefaultColumnLimit;
		PointerAlignment pointer_alignment     = PointerAlignment::Left;
		ReferenceAlignment reference_alignment = ReferenceAlignment::Left;
		BraceStyle brace_style                 = BraceStyle::Allman;
		// Single-statement control blocks: Keep (default), SingleLine, Indent or
		// IndentWithBraces. Only `if`/`else`/`for`/`while`/`do` bodies are touched;
		// function, record, namespace, lambda and switch/case blocks never are.
		SingleLineStyle single_line_style = SingleLineStyle::IndentWithBraces;
		// Sort contiguous `#include` runs by header name; other directives stay
		// verbatim. Off by default: sorting reorders source lines.
		bool sort_includes = false;
		/** Include groups in their configured formatting order. */
		std::vector<IncludeGroup> includeOrder = {IncludeGroup::Angle, IncludeGroup::Quote};
		/** Compare include names without distinguishing letter case. */
		bool includeCaseInsensitive = true;
		/** File name being formatted ("Reporting.cpp"); its homonymous header sorts first. */
		std::string mainFileName;
		/** Adds a missing final newline when the corresponding lint rule is enabled. */
		bool requireFinalNewline = false;
		// Insert an empty line after the closing `}` of an `if`/`else`/`for`/
		// `while`/`do`/`switch`/`try`/`catch` body when more code follows in the
		// same scope (never before `}`, `else`, `catch` or a label).
		bool blank_line_after_control_block = true;
		// Insert an empty line after a `class`/`struct`/`union`/`enum` definition
		// (`};`) when more code follows in the same scope (never before `}` or a
		// label such as `public:`).
		bool blank_line_after_type_definition = true;
		// Space before the colon of a base clause or enum underlying type:
		// `enum class E : std::uint8_t`, `class A : public B` (false: `E: T`).
		bool space_before_inheritance_colon = true;
		// Pad trailing `//` comments of adjacent lines to one column.
		bool align_trailing_comments        = true;
		bool align_consecutive_macros       = false;
		bool align_consecutive_assignments  = false;
		bool space_after_c_style_cast       = true;
		bool space_after_logical_not        = false;
		bool space_before_cpp11_braced_list = true;
		// Separate function/method declarations and definitions; in records,
		// also separate methods from fields and access sections. Fields stay grouped.
		bool blank_line_between_methods = true;
	};

	// One replacement hunk over the source buffer, line-oriented: lines
	// [start_line, end_line) are replaced by `replacement` (which carries its own
	// newlines). Offsets are provided so hosts can map to positions without
	// re-scanning; line numbers are 0-based.
	struct FormatEdit
	{
		std::size_t start_line   = 0;
		std::size_t end_line     = 0;
		std::size_t start_offset = 0;
		std::size_t end_offset   = 0;
		std::string replacement;
	};

	/** Immutable checkpoint after declaration and control-flow shaping passes. */
	class FormattingSnapshot
	{
	public:
		/** Formatted output owned by this snapshot, independent of its parse tree. */
		[[nodiscard]] const std::string& Formatted() const noexcept;

		/** Whether the remaining passes converged to a previous snapshot's input. */
		[[nodiscard]] bool ReusedShapedOutput() const noexcept
		{
			return mReusedShapedOutput;
		}

		/** Whether all passes finished without cancellation. */
		[[nodiscard]] bool Complete() const noexcept
		{
			return static_cast<bool>(mFormatted);
		}

		/** Retained string capacities; shared payload may be counted by another snapshot. */
		[[nodiscard]] std::size_t StorageBytes() const noexcept;

	private:
		friend class Formatter;
		FormatOptions mOptions;
		CppStandard mStandard = CppStandard::Cpp20;
		std::shared_ptr<const std::string> mPrepared;
		std::shared_ptr<const std::string> mFormatted;
		bool mReusedShapedOutput = false;
	};

	class Formatter
	{
	public:
		explicit Formatter(FormatOptions options = {}) : m_options(options) {}

		std::string Format(std::string_view source) const;

		std::string Format(const ParseTree& tree) const;

		/** Formats a pinned tree, returning no output when cancellation is requested. */
		[[nodiscard]] std::string Format(const ParseTree& tree, std::stop_token stop) const;

		/** Reuses remaining passes after exact convergence, invalidating on options or dialect. */
		[[nodiscard]] FormattingSnapshot FormatIncremental(
			const ParseTree& tree,
			const FormattingSnapshot* previous = nullptr,
			std::stop_token stop = {}) const;

		// Minimal line-diff between `source` and `Format(source)`: empty when the
		// file is already formatted. Edits are disjoint and ascending.
		std::vector<FormatEdit> FormatEdits(std::string_view source) const;

		/** Computes edits using the pinned tree's tokens and parser dialect. */
		[[nodiscard]] std::vector<FormatEdit> FormatEdits(
			const ParseTree& tree,
			std::stop_token stop = {}) const;

		/** Diffs a previously formatted buffer without running formatting passes again. */
		[[nodiscard]] static std::vector<FormatEdit> ComputeEdits(
			std::string_view source,
			std::string_view formatted,
			std::stop_token stop = {});

		// Format the whole buffer but only apply edits overlapping
		// [start_line, end_line); lines outside are byte-identical.
		std::string FormatRange(
			std::string_view source,
			std::size_t start_line,
			std::size_t end_line) const;

	private:
		Formatter(FormatOptions options, std::stop_token stop) :
		m_options(options), mStop(std::move(stop)) {}

		std::vector<FormatEdit> Diff(std::string_view source, std::string_view formatted) const;

		std::string DeclarationLayout(const ParseTree& tree) const;

		std::string FormatImpl(
			std::string_view source,
			const std::vector<Token>& tokens,
			const std::vector<PreprocessorDirective>& directives) const;

		std::string FormatShaped(
			std::string_view source,
			const std::vector<Token>& tokens,
			const std::vector<PreprocessorDirective>& directives) const;

		FormatOptions m_options;
		std::stop_token mStop;
	};

} // namespace heimdall
