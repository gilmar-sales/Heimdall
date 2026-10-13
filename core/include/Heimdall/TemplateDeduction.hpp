#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace heimdall
{

	class ParseTree;
	struct IndexedScope;

	/// A condition controlling a deduced return expression.
	struct ReturnCondition
	{
		std::string expression;
		bool negated = false;
	};

	/// An owned return expression and its enclosing constexpr conditions.
	struct ReturnRecipe
	{
		std::string expression;
		std::vector<ReturnCondition> conditions;
	};

	/// Cold, shared metadata for conservative return-type deduction.
	struct FunctionDeduction
	{
		std::vector<std::string> parameterNames;
		std::vector<std::string> parameterTypes;
		std::vector<ReturnRecipe> returns;
	};

	/// Bounded structural deduction; unsupported expressions leave the type unknown.
	class TemplateDeduction
	{
	public:
		/// Captures return expressions without retaining views into the parse tree.
		[[nodiscard]] static std::shared_ptr<const FunctionDeduction> Capture(
			const ParseTree& tree, std::size_t nameNode);

		/// Substitutes explicit and structurally deduced template arguments.
		[[nodiscard]] static std::string Instantiate(
			const FunctionDeduction& function,
			const std::vector<std::string>& templateParameters,
			const std::vector<std::string>& explicitArguments,
			const std::vector<std::string>& argumentTypes,
			std::span<const IndexedScope * const> scopes,
			const std::vector<std::string>& owner,
			bool preserveReference);
	};
}
