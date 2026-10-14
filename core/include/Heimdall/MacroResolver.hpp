#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <Heimdall/MacroTable.hpp>
#include <Heimdall/Preprocessor.hpp>

namespace heimdall
{

	/// Answers "which macro does this identifier name here?" for the parser. Implemented by
	/// whoever knows the translation unit (headers, `-D`, builtins); the core only asks.
	class MacroResolver
	{
	public:
		MacroResolver() = default;

		MacroResolver(const MacroResolver&) = delete;

		MacroResolver& operator= (const MacroResolver&) = delete;

		virtual~MacroResolver() = default;

		/// The definition of `name` live at byte `offset`, or null. Must stay valid for as long
		/// as the resolver lives and be safe to call from the parsing thread.
		[[nodiscard]] virtual const MacroDef* Find(
			std::string_view name,
			std::size_t offset) const noexcept = 0;

		/// Summary of every definition the resolver can return. A parse that was reused under a
		/// different fingerprint is read again, because its macros may have changed meaning.
		[[nodiscard]] virtual std::uint64_t Fingerprint() const noexcept = 0;

		/// The directives, active ranges and macro events of the text the resolver was built
		/// for, computed with every macro of the translation unit (a condition on a macro a
		/// header defines is evaluated for real). When non-null the parser uses it instead of
		/// preprocessing the text on its own, so it must describe exactly the text that is
		/// parsed. Null leaves the parser's own preprocessing in charge.
		[[nodiscard]] virtual const PreprocessorResult* Preprocessing() const noexcept
		{
			return nullptr;
		}

		/// Every macro live at `offset`, for expanding a use of one of them; empty when the
		/// resolver cannot provide a table.
		[[nodiscard]] virtual std::optional<MacroTable> TableAt(std::size_t) const
		{
			return std::nullopt;
		}
	};

} // namespace heimdall
