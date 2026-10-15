#pragma once

#include <cstdint>
#include <string_view>

namespace heimdall
{

	/**
	 * @brief Provides type-name information to the grammar parser.
	 *
	 * The parser uses this information to distinguish declarations from
	 * expressions and casts from parenthesized expressions. Names declared in
	 * the current file are tracked by the parser; this interface provides names
	 * declared in included headers. Names are matched by their last component
	 * (for example, `string` for `std::string`).
	 *
	 * Implementations are immutable and may be shared between threads. Unknown
	 * names are reported as non-types, allowing the parser to use its token-shape
	 * heuristic instead.
	 */
	class TypeNameOracle
	{
	public:
		/** @brief Destroys the oracle. */
		virtual~TypeNameOracle() = default;

		/**
		 * @brief Determines whether a name denotes a type.
		 * @param name Name to query; matching uses its last component.
		 * @return True if the name denotes a type; otherwise, false.
		 */
		virtual bool IsType(std::string_view name) const noexcept = 0;

		/**
		 * @brief Returns the underlying spelling of a known alias.
		 * @param name Alias name to query.
		 * @return Underlying type spelling, or an empty view if unknown or not an alias.
		 */
		virtual std::string_view AliasTarget(std::string_view name) const noexcept
		{
			return {};
		}

		/**
		 * @brief Returns an identifier for the oracle's current answers.
		 *
		 * The value changes whenever query answers may change. Incremental parse
		 * reuse is valid only when the fingerprint is unchanged.
		 * @return Fingerprint of the current oracle state.
		 */
		virtual std::uint64_t Fingerprint() const noexcept = 0;
	};

} // namespace heimdall
