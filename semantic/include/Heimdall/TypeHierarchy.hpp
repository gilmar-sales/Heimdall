#pragma once

#include <Heimdall/SemanticModel.hpp>
#include <Heimdall/TypeModel.hpp>

#include <cstdint>
#include <vector>

namespace heimdall
{
	/** Inheritance queries over one translation unit's semantic model. */
	class TypeHierarchy
	{
	public:
		/**
		 * Class named at `token`: the class itself, an alias of it, a variable or
		 * parameter declared with it, or a method's owning class. Fields and
		 * anything else do not identify a single class and yield kNone.
		 */
		[[nodiscard]] static SymbolId FindClassAt(
			const SemanticModel& model, const TypeModel& types, std::uint32_t token);

		/**
		 * Direct bases in declaration order. A dependent `Base<T>` falls back to
		 * the primary template of that name; unresolvable bases are skipped.
		 */
		[[nodiscard]] static std::vector<SymbolId> Parents(
			const SemanticModel& model, SymbolId klass);

		/** Classes in this translation unit that list `klass` as a direct base. */
		[[nodiscard]] static std::vector<SymbolId> Children(
			const SemanticModel& model, SymbolId klass);

		/** Token index of the identifier covering `offset`, or kNone. */
		[[nodiscard]] static std::uint32_t TokenAt(const SemanticModel& model, std::size_t offset);
	};
} // namespace heimdall
