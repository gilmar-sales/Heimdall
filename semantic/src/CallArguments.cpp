#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <Heimdall/LineTable.hpp>
#include <Heimdall/SemanticRules.hpp>
#include <string>
#include <vector>

#include "detail/TokenView.hpp"
#include "detail/ImplicitConversion.hpp"

namespace heimdall
{
	namespace
	{
		struct ParameterCount
		{
			std::size_t required = 0;
			std::size_t total    = 0;
			bool variadic        = false;
			bool known           = false;
			std::uint32_t suffix = kNone;
		};

		[[nodiscard]] ParameterCount CountParameters(const SemanticModel& model,
			const detail::TokenView& view,
			SymbolId symbol)
		{
			const auto& symbols = model.Symbols();
			const auto& nodes   = model.Tree().NodesSoA();
			ParameterCount result;
			if (symbols.kind[symbol] != SymbolKind::Function ||
				(symbols.flags[symbol] & SymbolFlag::Template) != 0)
			{
				return result;
			}

			const auto open = view.PositionOf(symbols.decl_token[symbol]) + 1;
			if (view.At(open) != Tok::LParen)
			{
				return result;
			}

			std::uint32_t suffix = kNone;
			for (const auto child : model.ChildrenOf(symbols.decl_node[symbol]))
			{
				if (nodes.Kind(child) != GrammarKind::Declarator)
				{
					continue;
				}

				for (const auto part : model.ChildrenOf(child))
				{
					if (nodes.Kind(part) == GrammarKind::FunctionSuffix &&
						nodes.FirstToken(part) == view.TokenAt(open))
					{
						suffix = part;
						break;
					}
				}
			}

			if (suffix == kNone)
			{
				return result;
			}

			const auto close = view.Match(open, view.Size());
			if (close == view.Size())
			{
				return result;
			}

			result.variadic = close > open + 1 && view.At(close - 1) == Tok::Ellipsis;
			for (const auto parameter : model.ChildrenOf(suffix))
			{
				if (nodes.Kind(parameter) != GrammarKind::ParameterDeclaration)
				{
					continue;
				}

				const auto[begin, end] = view.Range(parameter);
				bool hasDefault        = false;
				for (auto position = begin; position < end; ++position)
				{
					const auto tok = view.At(position);
					if (tok == Tok::Eq)
					{
						hasDefault = true;
						break;
					}

					if (tok == Tok::Ellipsis)
					{
						return {};
					}

					if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace)
					{
						position = view.Match(position, end);
					}
				}

				++result.total;
				result.required += hasDefault ? 0 : 1;
			}

			result.known  = true;
			result.suffix = suffix;
			return result;
		}

		enum class Conversion
		{
			Valid,
			Invalid,
			Unknown
		};

		enum class ArgumentCategory
		{
			Unknown,
			LValue,
			RValue
		};

		[[nodiscard]] ArgumentCategory CategoryOf(const TypeModel& types,
			const detail::TokenView& view,
			std::uint32_t node)
		{
			const auto& model = types.Model();
			const auto kind   = model.Tree().NodesSoA().Kind(node);
			if (kind == GrammarKind::LiteralExpression)
			{
				return types.Types().IsArray(types.NodeType(node))
				? ArgumentCategory::LValue : ArgumentCategory::RValue;
			}

			const auto children = model.ChildrenOf(node);
			if (kind == GrammarKind::ParenthesizedExpression && children.size() == 1)
			{
				return CategoryOf(types, view, children.front());
			}

			if (kind == GrammarKind::IdentifierExpression)
			{
				const auto[begin, end] = view.Range(node);
				if (end == begin + 1)
				{
					const auto symbol = model.ResolveToken(view.TokenAt(begin));
					if (symbol != kNone &&
						(model.Symbols().kind[symbol] == SymbolKind::Variable ||
						model.Symbols().kind[symbol] == SymbolKind::Parameter))
					{
						return ArgumentCategory::LValue;
					}
				}
			}

			return ArgumentCategory::Unknown;
		}

		[[nodiscard]] bool IsNullLiteral(const SemanticModel& model,
			const detail::TokenView& view,
			std::uint32_t node)
		{
			const auto[begin, end] = view.Range(node);
			if (end == begin + 1)
			{
				return view.IsZeroLiteral(begin);
			}

			const auto children = model.ChildrenOf(node);
			return model.Tree().NodesSoA().Kind(node) == GrammarKind::ParenthesizedExpression &&
				children.size() == 1 && IsNullLiteral(model, view, children.front());
		}

		[[nodiscard]] Conversion ImplicitConversion(
			const TypeModel& types,
			const detail::TokenView& view,
			std::uint32_t argument,
			TypeId parameter)
		{
			const auto& table     = types.Types();
			const auto source     = table.Strip(types.NodeType(argument));
			const auto target     = table.Strip(parameter);
			const auto sourceKind = table.Kind(source);
			const auto targetKind = table.Kind(target);
			if (sourceKind == TypeKind::Unknown || targetKind == TypeKind::Unknown)
			{
				return Conversion::Unknown;
			}

			if (sourceKind == TypeKind::Class || targetKind == TypeKind::Class ||
				sourceKind == TypeKind::Enum || targetKind == TypeKind::Enum)
			{
				return Conversion::Unknown;
			}

			const auto referenceKind = table.Kind(parameter);
			if (referenceKind == TypeKind::LRef || referenceKind == TypeKind::RRef)
			{
				const auto referee  = table.Arg(parameter);
				const auto category = CategoryOf(types, view, argument);
				if (source == target)
				{
					if (!table.IsConstQualified(referee) &&
						table.IsConstQualified(types.NodeType(argument)))
					{
						return Conversion::Invalid;
					}

					if ((referenceKind == TypeKind::LRef && !table.IsConstQualified(referee) &&
						category == ArgumentCategory::RValue) ||
						(referenceKind == TypeKind::RRef && category == ArgumentCategory::LValue))
					{
						return Conversion::Invalid;
					}
				}
				else if (referenceKind == TypeKind::LRef && !table.IsConstQualified(referee) &&
					table.IsArithmetic(source) && table.IsArithmetic(target))
				{
					return Conversion::Invalid;
				}
			}

			if (source == target ||(table.IsArithmetic(source) && table.IsArithmetic(target)))
			{
				return Conversion::Valid;
			}

			const bool sourcePointer =
				sourceKind == TypeKind::Pointer || sourceKind == TypeKind::Array;
			if (table.IsBool(target) && sourcePointer)
			{
				return Conversion::Valid;
			}

			if (targetKind == TypeKind::Pointer)
			{
				if (table.IsBuiltin(source, BuiltinType::NullptrT) ||
					(table.IsInteger(source) && IsNullLiteral(types.Model(), view, argument)))
				{
					return Conversion::Valid;
				}

				if (sourcePointer)
				{
					const auto from = table.Element(source);
					const auto to   = table.Element(target);
					if (table.IsConstQualified(from) && !table.IsConstQualified(to))
					{
						return Conversion::Invalid;
					}

					if (table.Strip(from) == table.Strip(to) ||
						table.IsBuiltin(table.Strip(to), BuiltinType::Void))
					{
						return Conversion::Valid;
					}

					if (table.IsArithmetic(table.Strip(from)) &&
						table.IsArithmetic(table.Strip(to)))
					{
						return Conversion::Invalid;
					}

					return Conversion::Unknown;
				}

				return sourceKind == TypeKind::Builtin ||
					types.ExternalHead(source) == "std::string"
				? Conversion::Invalid : Conversion::Unknown;
			}

			const bool sourceString = types.ExternalHead(source) == "std::string";
			const bool targetString = types.ExternalHead(target) == "std::string";
			if (targetString && table.IsInteger(source) &&
				IsNullLiteral(types.Model(), view, argument))
			{
				return Conversion::Unknown;
			}

			if (targetString && sourcePointer)
			{
				return table.IsBuiltin(table.Strip(table.Element(source)), BuiltinType::Char)
				? Conversion::Valid : Conversion::Invalid;
			}

			if ((targetString && sourceKind == TypeKind::Builtin) ||
				(sourceString && targetKind == TypeKind::Builtin) ||
				(sourcePointer && targetKind == TypeKind::Builtin))
			{
				return Conversion::Invalid;
			}

			if (sourceKind == TypeKind::Builtin && targetKind == TypeKind::Builtin)
			{
				return Conversion::Invalid;
			}

			return Conversion::Unknown;
		}

		struct ParameterRange
		{
			ParameterCount count;
			std::size_t first = 0;
		};
	}

	detail::ConversionResult detail::CheckImplicitConversion(const TypeModel& types,
		std::uint32_t expression,
		TypeId target)
	{
		const detail::TokenView view(types.Model());
		switch (ImplicitConversion(types, view, expression, target))
		{
		case Conversion::Valid:
			return detail::ConversionResult::Valid;
		case Conversion::Invalid:
			return detail::ConversionResult::Invalid;
		default:
			return detail::ConversionResult::Unknown;
		}
	}

	std::vector<Diagnostic> SemanticRules::AnalyzeCallArguments(const SemanticModel& model)
	{
		std::vector<Diagnostic> diagnostics;
		const auto& nodes   = model.Tree().NodesSoA();
		const auto& symbols = model.Symbols();
		const detail::TokenView view(model);
		std::vector<ParameterCount> counts;
		counts.reserve(symbols.Size());
		for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
		{
			counts.push_back(CountParameters(model, view, symbol));
		}

		LineTable lines;
		bool linesBuilt = false;
		for (std::uint32_t node = 0; node < nodes.size(); ++node)
		{
			if (nodes.Kind(node) != GrammarKind::CallExpression)
			{
				continue;
			}

			const auto children = model.ChildrenOf(node);
			if (children.empty() ||
				nodes.Kind(children.front()) != GrammarKind::IdentifierExpression)
			{
				continue;
			}

			const auto[begin, end] = view.Range(children.front());
			if (end != begin + 1 ||!view.IsWord(begin))
			{
				continue;
			}

			const auto target = model.ResolveToken(view.TokenAt(begin));
			if (target == kNone || symbols.kind[target] != SymbolKind::Function)
			{
				continue;
			}

			const auto[callBegin, callEnd] = view.Range(node);
			if (callBegin != begin || callEnd <= end || view.At(callEnd - 1) != Tok::RParen)
			{
				continue;
			}

			bool dependent = false;
			for (auto position = end; position < callEnd; ++position)
			{
				dependent |= view.At(position) == Tok::Ellipsis;
			}

			if (dependent)
			{
				continue;
			}

			const auto supplied = children.size() - 1;
			bool viable         = false;
			bool unknown        = false;
			bool variadic       = false;
			std::size_t minimum = view.Size();
			std::size_t maximum = 0;
			for (auto overload = model.LookupLocal(symbols.scope[target], symbols.name[target]);
				overload != kNone; overload = symbols.next_same_name[overload])
			{
				if (symbols.decl_token[overload] > view.TokenAt(begin))
				{
					continue;
				}

				const auto& count = counts[overload];
				unknown |=!count.known;
				variadic |= count.variadic;
				viable |= count.known && supplied >= count.required &&
					(count.variadic || supplied <= count.total);
				minimum = std::min(minimum, count.required);
				maximum = std::max(maximum, count.total);
			}

			if (viable || unknown || minimum == view.Size())
			{
				continue;
			}

			if (!linesBuilt)
			{
				lines.Build(model.Tree().Source());
				linesBuilt = true;
			}

			const auto offset   = view.Offset(begin);
			const auto position = lines.Lookup(offset);
			const auto expected = variadic ? "at least " + std::to_string(minimum)
			: minimum == maximum ? std::to_string(minimum)
			: std::to_string(minimum) + ".." + std::to_string(maximum);
			diagnostics.push_back(Diagnostic {
					RuleId::CallArgumentCount, Severity::Error, "cpp/call-argument-count",
					"no matching argument count for '" + std::string(view.Text(begin)) +
					"': expected " + expected + ", received " + std::to_string(supplied),
					offset, view.End(begin) - offset, position.line, position.column, false, {}
			});
		}

		return diagnostics;
	}

	std::vector<Diagnostic> SemanticRules::AnalyzeCallArgumentTypes(const TypeModel& types)
	{
		const auto& model   = types.Model();
		const auto& nodes   = model.Tree().NodesSoA();
		const auto& symbols = model.Symbols();
		const detail::TokenView view(model);
		std::vector<ParameterRange> ranges;
		std::vector<std::uint32_t> parameters;
		ranges.reserve(symbols.Size());
		for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
		{
			const auto count = CountParameters(model, view, symbol);
			ranges.push_back({count, parameters.size()});
			if (!count.known)
			{
				continue;
			}

			for (const auto child : model.ChildrenOf(count.suffix))
			{
				if (nodes.Kind(child) == GrammarKind::ParameterDeclaration)
				{
					parameters.push_back(child);
				}
			}
		}

		std::vector<Diagnostic> diagnostics;
		LineTable lines;
		bool linesBuilt = false;
		for (std::uint32_t node = 0; node < nodes.size(); ++node)
		{
			if (nodes.Kind(node) != GrammarKind::CallExpression)
			{
				continue;
			}

			const auto children = model.ChildrenOf(node);
			if (children.empty() ||
				nodes.Kind(children.front()) != GrammarKind::IdentifierExpression)
			{
				continue;
			}

			const auto[begin, end] = view.Range(children.front());
			if (end != begin + 1 ||!view.IsWord(begin))
			{
				continue;
			}

			const auto target = model.ResolveToken(view.TokenAt(begin));
			if (target == kNone || symbols.kind[target] != SymbolKind::Function)
			{
				continue;
			}

			const auto[callBegin, callEnd] = view.Range(node);
			if (callBegin != begin || callEnd <= end || view.At(callEnd - 1) != Tok::RParen)
			{
				continue;
			}

			bool dependent = false;
			for (auto position = end; position < callEnd; ++position)
			{
				dependent |= view.At(position) == Tok::Ellipsis;
			}

			if (dependent)
			{
				continue;
			}

			const auto supplied      = children.size() - 1;
			bool viable              = false;
			bool unknown             = false;
			std::size_t invalidIndex = 0;
			TypeId expected          = TypeTable::Unknown;
			for (auto overload = model.LookupLocal(symbols.scope[target], symbols.name[target]);
				overload != kNone; overload = symbols.next_same_name[overload])
			{
				if (symbols.decl_token[overload] > view.TokenAt(begin))
				{
					continue;
				}

				const auto& range = ranges[overload];
				const auto& count = range.count;
				if (!count.known)
				{
					unknown = true;
					continue;
				}

				if (supplied < count.required ||(!count.variadic && supplied > count.total))
				{
					continue;
				}

				bool rejected = false;
				for (std::size_t index = 0; index < std::min(supplied, count.total); ++index)
				{
					const auto parameter = types.ParameterType(parameters[range.first + index]);
					if (ImplicitConversion(types, view, children[index + 1], parameter) ==
						Conversion::Invalid)
					{
						rejected = true;
						if (expected == TypeTable::Unknown)
						{
							invalidIndex = index;
							expected     = parameter;
						}

						break;
					}
				}

				viable |=!rejected;
			}

			if (viable || unknown || expected == TypeTable::Unknown)
			{
				continue;
			}

			if (!linesBuilt)
			{
				lines.Build(model.Tree().Source());
				linesBuilt = true;
			}

			const auto argument                    = children[invalidIndex + 1];
			const auto[argumentBegin, argumentEnd] = view.Range(argument);
			if (argumentBegin == argumentEnd)
			{
				continue;
			}

			const auto offset   = view.Offset(argumentBegin);
			const auto position = lines.Lookup(offset);
			diagnostics.push_back(Diagnostic {
					RuleId::CallArgumentType, Severity::Error, "cpp/call-argument-type",
					"no matching overload for '" + std::string(view.Text(begin)) + "': argument " +
					std::to_string(invalidIndex + 1) + " of type '" +
					types.Spell(types.NodeType(argument)) + "' cannot implicitly convert to '" +
					types.Spell(expected) + "'",
					offset, view.End(argumentEnd - 1) - offset,
					position.line, position.column, false, {}
			});
		}

		return diagnostics;
	}
}
