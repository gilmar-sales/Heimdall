#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <Heimdall/LineTable.hpp>
#include <Heimdall/Lexer.hpp>
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
			if (sourceKind == TypeKind::Unknown || targetKind == TypeKind::Unknown ||
				table.IsDependent(source) || table.IsDependent(target))
			{
				return Conversion::Unknown;
			}

			if (sourceKind == TypeKind::Class || targetKind == TypeKind::Class ||
				sourceKind == TypeKind::TemplateId || targetKind == TypeKind::TemplateId ||
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

		struct ClassInstance
		{
			SymbolId symbol = kNone;
			std::string spelling;
			std::uint32_t instance = kNone;
		};

		[[nodiscard]] std::string_view Trim(std::string_view text)
		{
			while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
			{
				text.remove_prefix(1);
			}

			while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
			{
				text.remove_suffix(1);
			}

			return text;
		}

		[[nodiscard]] ClassInstance ClassOfExpression(
			const TypeModel& types, std::uint32_t expression)
		{
			const auto& model   = types.Model();
			const auto& tree    = model.Tree();
			const auto& nodes   = tree.NodesSoA();
			const auto& symbols = model.Symbols();
			const auto& table   = types.Types();
			TypeId type         = table.Strip(types.NodeType(expression));
			if (table.IsPointer(type))
			{
				type = table.Strip(table.Element(type));
			}

			if (table.Kind(type) == TypeKind::Class)
			{
				return {table.Arg(type), std::string(model.Names().Text(symbols.name[table.Arg(type)]))};
			}

			if (table.Kind(type) == TypeKind::TemplateId)
			{
				const auto instance = table.Arg(type);
				if (types.Instantiations().state[instance] != TemplateResolution::Resolved)
				{
					return {};
				}

				const auto owner = model.Templates().owner[types.Instantiations().selected[instance]];
				return {owner, types.Spell(type), instance};
			}

			if (expression >= nodes.size() || nodes.Kind(expression) != GrammarKind::IdentifierExpression)
			{
				return {};
			}

			const auto variable = model.ResolveToken(nodes.FirstToken(expression));
			if (variable == kNone || symbols.kind[variable] != SymbolKind::Variable)
			{
				return {};
			}

			const auto declaration = symbols.decl_node[variable];
			if (declaration >= nodes.size())
			{
				return {};
			}

			const auto nameToken = symbols.decl_token[variable];
			const auto scope     = model.ScopeOfNode(declaration);
			const auto& tokens   = tree.Tokens();
			for (auto token = nodes.FirstToken(declaration); token < nameToken && token < tokens.size(); ++token)
			{
				if (tokens[token].kind != TokenKind::Identifier)
				{
					continue;
				}

				const auto name      = model.Names().Find(tree.Text(tokens[token]));
				const auto candidate = name == kNone ? kNone : model.Lookup(scope, name, token);
				if (candidate != kNone && symbols.kind[candidate] == SymbolKind::Class)
				{
					const auto typeBegin = tokens[token].offset;
					const auto typeEnd   = tokens[nameToken].offset;
					return {candidate, std::string(Trim(tree.Source().substr(typeBegin, typeEnd - typeBegin)))};
				}
			}

			const auto end = std::min<std::size_t>(
				nodes.FirstToken(declaration) + nodes.TokenCount(declaration), tokens.size());
			bool initializer = false;
			for (auto token = nameToken + 1; token < end; ++token)
			{
				if (!initializer)
				{
					initializer = tokens[token].tok == Tok::Eq;
					continue;
				}

				if (tokens[token].kind != TokenKind::Identifier)
				{
					continue;
				}

				const auto name      = model.Names().Find(tree.Text(tokens[token]));
				const auto candidate = name == kNone ? kNone : model.Lookup(scope, name, token);
				if (candidate != kNone && symbols.kind[candidate] == SymbolKind::Class)
				{
					auto typeEnd = token;
					int depth    = 0;
					bool opened  = false;
					for (auto i = token + 1; i < end; ++i)
					{
						if (tokens[i].kind == TokenKind::Whitespace)
						{
							continue;
						}

						if (tokens[i].tok == Tok::Lt)
						{
							opened = true;
							++depth;
						}
						else if (tokens[i].tok == Tok::Gt && --depth == 0)
						{
							typeEnd = i;
							break;
						}
						else if (!opened)
						{
							break;
						}
					}

					const auto start  = tokens[token].offset;
					const auto finish = tokens[typeEnd].offset + tokens[typeEnd].length;
					return {candidate, std::string(tree.Source().substr(start, finish - start))};
				}

				const auto function = model.ResolveToken(token);
				if (function == kNone || symbols.kind[function] != SymbolKind::Function)
				{
					continue;
				}

				const auto functionNode = symbols.decl_node[function];
				const auto functionName = symbols.decl_token[function];
				if (functionNode >= nodes.size() || functionName >= tokens.size())
				{
					continue;
				}

				for (auto typeToken = nodes.FirstToken(functionNode);
					typeToken < functionName && typeToken < tokens.size(); ++typeToken)
				{
					if (tokens[typeToken].kind != TokenKind::Identifier)
					{
						continue;
					}

					const auto typeName = model.Names().Find(tree.Text(tokens[typeToken]));
					const auto typeSymbol = typeName == kNone ? kNone :
					model.Lookup(model.ScopeOfNode(functionNode), typeName, typeToken);
					if (typeSymbol == kNone || symbols.kind[typeSymbol] != SymbolKind::Class)
					{
						continue;
					}

					const auto start  = tokens[typeToken].offset;
					const auto finish = tokens[functionName].offset;
					return {typeSymbol,
						std::string(Trim(tree.Source().substr(start, finish - start)))};
				}
			}

			return {};
		}

		[[nodiscard]] std::string InstantiateParameterType(
			const SemanticModel& model,
			SymbolId owner,
			std::string_view instanceType,
			std::uint32_t parameter)
		{
			const auto& tree          = model.Tree();
			const auto& nodes         = tree.NodesSoA();
			const auto& tokens        = tree.Tokens();
			const auto parameterBegin = nodes.FirstToken(parameter);
			const auto parameterEnd = std::min<std::size_t>(
				parameterBegin + nodes.TokenCount(parameter), tokens.size());
			std::uint32_t parameterName = kNone;
			for (auto token = parameterBegin; token < parameterEnd; ++token)
			{
				if (tokens[token].kind == TokenKind::Identifier)
				{
					parameterName = token;
				}
			}

			if (parameterName == kNone)
			{
				return {};
			}

			auto type = Trim(tree.Source().substr(tokens[parameterBegin].offset,
				tokens[parameterName].offset - tokens[parameterBegin].offset));
			const auto& symbols = model.Symbols();
			const auto record   = symbols.decl_node[owner];
			if (record >= nodes.size())
			{
				return std::string(type);
			}

			const auto parent = nodes.Parent(record);
			if (parent >= nodes.size() || nodes.Kind(parent) != GrammarKind::TemplateDeclaration)
			{
				return std::string(type);
			}

			const auto recordStart = nodes.FirstToken(record);
			std::string_view templateParameter;
			for (auto token = nodes.FirstToken(parent); token < recordStart; ++token)
			{
				if (tokens[token].tok == Tok::KwClass || tokens[token].tok == Tok::KwTypename)
				{
					for (++token; token < recordStart; ++token)
					{
						if (tokens[token].kind == TokenKind::Identifier)
						{
							templateParameter = tree.Text(tokens[token]);
							break;
						}
					}

					break;
				}
			}

			const auto open  = instanceType.find('<');
			const auto close = instanceType.rfind('>');
			if (templateParameter.empty() || open == std::string_view::npos ||
				close == std::string_view::npos || close <= open)
			{
				return std::string(type);
			}

			const auto argument = Trim(instanceType.substr(open + 1, close - open - 1));
			const auto lexed    = Lexer(type).Lex();
			std::string substituted;
			std::size_t cursor = 0;
			for (const auto& token : lexed)
			{
				if (token.kind != TokenKind::Identifier ||
					type.substr(token.offset, token.length) != templateParameter)
				{
					continue;
				}

				substituted.append(type.substr(cursor, token.offset - cursor));
				substituted.append(argument);
				cursor = token.offset + token.length;
			}

			if (cursor == 0)
			{
				return std::string(type);
			}

			substituted.append(type.substr(cursor));
			return substituted;
		}
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
			// Member function bodies see the whole class, including members
			// declared later; free functions need a prior declaration.
			const auto overloadScope = symbols.scope[target];
			const bool allowForward = overloadScope < model.Scopes().kind.size() &&
				model.Scopes().kind[overloadScope] == ScopeKind::Class;
			for (auto overload = model.LookupLocal(symbols.scope[target], symbols.name[target]);
				overload != kNone; overload = symbols.next_same_name[overload])
			{
				if (!allowForward && symbols.decl_token[overload] > view.TokenAt(begin))
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
			if (children.empty())
			{
				continue;
			}

			const auto callee      = children.front();
			const auto[begin, end] = view.Range(callee);
			SymbolId target        = kNone;
			std::string callName;
			ClassInstance instance;
			bool memberCall = false;
			if (nodes.Kind(callee) == GrammarKind::IdentifierExpression)
			{
				if (end != begin + 1 ||!view.IsWord(begin))
				{
					continue;
				}

				target   = model.ResolveToken(view.TokenAt(begin));
				callName = std::string(view.Text(begin));
			}
			else if (nodes.Kind(callee) == GrammarKind::MemberExpression)
			{
				const auto memberChildren = model.ChildrenOf(callee);
				if (memberChildren.size() != 2)
				{
					continue;
				}

				instance               = ClassOfExpression(types, memberChildren[0]);
				const auto memberToken = nodes.FirstToken(memberChildren[1]);
				if (instance.symbol == kNone || memberToken >= model.Tree().Tokens().size())
				{
					continue;
				}

				const auto& memberNameToken = model.Tree().Tokens()[memberToken];
				const auto memberName       = model.Names().Find(model.Tree().Text(memberNameToken));
				target                      = memberName == kNone ? kNone : model.LookupMember(instance.symbol, memberName);
				callName                    = std::string(model.Tree().Text(memberNameToken));
				memberCall                  = true;
			}
			else
			{
				continue;
			}

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
			std::string expectedSpelling;
			const auto overloadScope = symbols.scope[target];
			const bool allowForward = overloadScope < model.Scopes().kind.size() &&
				model.Scopes().kind[overloadScope] == ScopeKind::Class;
			for (auto overload = model.LookupLocal(symbols.scope[target], symbols.name[target]);
				overload != kNone; overload = symbols.next_same_name[overload])
			{
				if (!allowForward && symbols.decl_token[overload] > view.TokenAt(begin))
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
					const auto parameterNode = parameters[range.first + index];
					auto parameter           = types.ParameterType(parameterNode);
					if (memberCall && instance.instance != kNone)
					{
						parameter = types.SubstitutedType(parameter, instance.instance);
					}

					Conversion conversion = ImplicitConversion(
						types, view, children[index + 1], parameter);
					std::string spelling;
					if (memberCall && instance.instance == kNone && parameter == TypeTable::Unknown)
					{
						spelling = InstantiateParameterType(
							model, instance.symbol, instance.spelling, parameterNode);
						std::string_view base = Trim(spelling);
						if (base.starts_with("const "))
						{
							base.remove_prefix(6);
						}

						while (!base.empty() &&
							(base.back() == '&' || base.back() == '*' || base.back() == ' '))
						{
							base.remove_suffix(1);
						}

						TypeId instantiated = TypeTable::Unknown;
						for (TypeId candidate = 1; candidate < types.Types().Size(); ++candidate)
						{
							if (types.Spell(candidate) == base)
							{
								instantiated = candidate;
								break;
							}
						}

						if (instantiated != TypeTable::Unknown)
						{
							conversion = ImplicitConversion(
								types, view, children[index + 1], instantiated);
						}
						else if (!spelling.empty())
						{
							const auto source = types.Types().Strip(types.NodeType(children[index + 1]));
							const bool expectedNumeric = base == "bool" || base == "char" ||
								base == "short" || base == "int" || base == "long" ||
								base == "float" || base == "double";
							if (expectedNumeric && (types.Types().IsPointer(source) ||
								types.Types().IsArray(source)))
							{
								conversion = Conversion::Invalid;
							}
							else
							{
								conversion = Conversion::Unknown;
							}
						}
					}

					if (conversion == Conversion::Invalid)
					{
						rejected = true;
						if (expected == TypeTable::Unknown)
						{
							invalidIndex     = index;
							expected         = parameter;
							expectedSpelling = std::move(spelling);
						}

						break;
					}
				}

				viable |=!rejected;
			}

			if (viable || unknown ||
				(expected == TypeTable::Unknown && expectedSpelling.empty()))
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
					"no matching overload for '" + callName + "': argument " +
					std::to_string(invalidIndex + 1) + " of type '" +
					types.Spell(types.NodeType(argument)) + "' cannot implicitly convert to '" +
					(expectedSpelling.empty() ? types.Spell(expected) : expectedSpelling) + "'",
					offset, view.End(argumentEnd - 1) - offset,
					position.line, position.column, false, {}
			});
		}

		return diagnostics;
	}

	std::vector<Diagnostic> SemanticRules::AnalyzeIndexedCallArgumentTypes(
		const TypeModel& types, const ScopeIndex& external)
	{
		const auto& tree   = types.Model().Tree();
		const auto& nodes  = tree.NodesSoA();
		const auto& tokens = tree.Tokens();
		std::vector<Diagnostic> diagnostics;
		LineTable lines;
		lines.Build(tree.Source());
		for (std::uint32_t node = 0; node < nodes.size(); ++node)
		{
			if (nodes.Kind(node) != GrammarKind::CallExpression)
			{
				continue;
			}

			const auto children = types.Model().ChildrenOf(node);
			if (children.size() != 2 || nodes.Kind(children[0]) != GrammarKind::IdentifierExpression ||
				nodes.Kind(children[1]) != GrammarKind::IdentifierExpression)
			{
				continue;
			}

			const auto callableToken = nodes.FirstToken(children[0]);
			const auto argumentToken = nodes.FirstToken(children[1]);
			const auto callable      = CompletionEngine::VariableType(tree, callableToken, &external);
			const auto open          = callable.find('<');
			const auto parameterOpen = callable.find('(', open);
			const auto parameterEnd  = callable.rfind(')');
			if (open == std::string::npos || parameterOpen == std::string::npos ||
				parameterEnd == std::string::npos || parameterEnd <= parameterOpen ||
				(callable.substr(0, open) != "std::function" && callable.substr(0, open) != "function"))
			{
				continue;
			}

			auto parameter = callable.substr(parameterOpen + 1, parameterEnd - parameterOpen - 1);
			parameter.erase(std::remove_if(parameter.begin(), parameter.end(),
				[](char value)
				{
					return value == ' ' || value == '\t' || value == '\n';
				}), parameter.end());
			if (!parameter.ends_with('&') || parameter.ends_with("&&") ||
				parameter.starts_with("const") || parameter.find_first_of("<,()*") != std::string::npos)
			{
				continue;
			}

			parameter.pop_back();
			const auto actual     = CompletionEngine::VariableType(tree, argumentToken, &external);
			const auto actualOpen = actual.find('<');
			const auto actualEnd  = actual.rfind('>');
			if (actualOpen == std::string::npos || actualEnd == std::string::npos ||
				actual.substr(actualOpen + 1, actualEnd - actualOpen - 1) != parameter)
			{
				continue;
			}

			auto wrapper = actual.substr(0, actualOpen);
			if (wrapper.starts_with("const "))
			{
				wrapper.erase(0, 6);
			}

			bool proven = false;
			for (const auto& scope : external)
			{
				std::string path;
				for (const auto& part : scope.path)
				{
					path += path.empty() ? part : "::" + part;
				}

				if (path != wrapper || scope.kind != CompletionKind::Type ||!scope.bases.empty() ||
					scope.hasReferenceConversion)
				{
					continue;
				}

				for (const auto& owner : external)
				{
					if (owner.path.size() + 1 != scope.path.size() ||
						!std::equal(owner.path.begin(), owner.path.end(), scope.path.begin()))
					{
						continue;
					}

					proven = std::ranges::any_of(owner.members,[&](const CompletionItem& member)
						{
							return (member.kind == CompletionKind::Type || member.kind == CompletionKind::Namespace) &&
							member.label == scope.path.back() && member.is_definition;
					});
				}

				for (const auto& member : scope.members)
				{
					if (!member.label.starts_with("operator "))
					{
						continue;
					}

					if (member.label.find(parameter) != std::string::npos ||
						std::ranges::any_of(scope.template_params,[&](const std::string& name)
						{
							return member.label.find(name) != std::string::npos;
					}))
					{
						proven = false;
					}
				}

				break;
			}

			if (proven && argumentToken < tokens.size())
			{
				const auto& token   = tokens[argumentToken];
				const auto position = lines.Lookup(token.offset);
				diagnostics.push_back({RuleId::CallArgumentType, Severity::Error,
						"cpp/call-argument-type", "cannot bind '" + actual + "' to '" + parameter + "&'",
						token.offset, token.length, position.line, position.column, false, {}});
			}
		}

		return diagnostics;
	}
}
