#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <Heimdall/LineTable.hpp>
#include <Heimdall/SemanticRules.hpp>
#include <string>
#include <utility>
#include <vector>

#include "detail/ImplicitConversion.hpp"
#include "detail/TokenView.hpp"

namespace heimdall
{
	namespace
	{
		struct ReturnInfo
		{
			TypeId type  = TypeTable::Unknown;
			bool deduced = false;
		};

		ReturnInfo GetReturnInfo(const FlowModel& flow, FunctionId function)
		{
			const auto& model     = flow.Model();
			const auto& types     = flow.Types();
			const auto& functions = flow.Functions();
			const auto symbol     = functions.symbol[function];
			const detail::TokenView view(model);
			ReturnInfo result;
			result.type = symbol == kNone ? types.LambdaReturnType(functions.node[function])
			: types.SymbolType(symbol);
			const auto[begin, end] = view.Range(functions.node[function]);
			const auto body        = view.Range(functions.body[function]).first;
			bool hasAuto           = symbol == kNone;
			bool trailing          = false;
			for (const auto child : model.ChildrenOf(functions.node[function]))
			{
				if (model.Tree().NodesSoA().Kind(child) == GrammarKind::TypeSpecifier)
				{
					const auto[typeBegin, typeEnd] = view.Range(child);
					for (auto position = typeBegin; position < typeEnd; ++position)
					{
						hasAuto |= view.At(position) == Tok::KwAuto;
					}
				}
			}

			for (auto position = begin; position < std::min(end, body); ++position)
			{
				if (view.At(position) == Tok::LParen || view.At(position) == Tok::LBracket)
				{
					position = view.Match(position, body);
					continue;
				}

				trailing |= view.At(position) == Tok::Arrow;
			}

			result.deduced = hasAuto && !trailing;
			if (symbol != kNone &&
				(model.Symbols().flags[symbol] &
				(SymbolFlag::Template | SymbolFlag::Constructor | SymbolFlag::Destructor)) != 0)
			{
				return {};
			}

			if (result.deduced && result.type == TypeTable::Unknown)
			{
				const auto& nodes = model.Tree().NodesSoA();
				for (std::uint32_t node = 0; node < nodes.size(); ++node)
				{
					const auto block = flow.StatementBlock(node);
					if (nodes.Kind(node) != GrammarKind::ReturnStatement || block == kNone ||
						flow.Blocks().function[block] != function)
					{
						continue;
					}

					for (const auto child : model.ChildrenOf(node))
					{
						if (types.NodeType(child) != TypeTable::Unknown)
						{
							result.type = types.Types().Strip(types.NodeType(child));
							return result;
						}
					}
				}
			}

			return result;
		}

		class Reporter
		{
		public:
			explicit Reporter(const ParseTree& tree) : mTree(tree) {}

			void Add(
				RuleId rule,
				Severity severity,
				const std::string& code,
				std::string message,
				std::size_t offset,
				std::size_t length)
			{
				if (mDiagnostics.empty())
				{
					mLines.Build(mTree.Source());
				}

				const auto position = mLines.Lookup(offset);
				mDiagnostics.push_back(Diagnostic {rule, severity, code, std::move(message),
						offset, length, position.line, position.column, false, {}});
			}

			std::vector<Diagnostic> Take()
			{
				return std::move(mDiagnostics);
			}

		private:
			const ParseTree& mTree;
			LineTable mLines;
			std::vector<Diagnostic> mDiagnostics;
		};
	}

	std::vector<Diagnostic> SemanticRules::AnalyzeUnreachable(const FlowModel& flow)
	{
		const auto& tree = flow.Model().Tree();
		if (!tree.Diagnostics().empty())
		{
			return {};
		}

		const auto& nodes     = tree.NodesSoA();
		const auto& functions = flow.Functions();
		const detail::TokenView view(flow.Model());
		Reporter reporter(tree);
		std::vector<std::vector<std::uint8_t>> reachable;
		for (FunctionId function = 0; function < functions.Size(); ++function)
		{
			reachable.push_back(flow.ReachableSet(function));
		}

		for (std::uint32_t node = 0; node < nodes.size(); ++node)
		{
			const auto block = flow.StatementBlock(node);
			if (block == kNone || nodes.Kind(node) == GrammarKind::CompoundStatement ||
				nodes.Kind(node) == GrammarKind::EmptyStatement)
			{
				continue;
			}

			const auto function = flow.Blocks().function[block];
			if (functions.complete[function] == 0 ||
				reachable[function][block - functions.first_block[function]] != 0)
			{
				continue;
			}

			const auto[begin, end] = view.Range(node);
			if (begin < end)
			{
				reporter.Add(RuleId::UnreachableCode, Severity::Warning, "cpp/unreachable-code",
					"statement is unreachable", view.Offset(begin),
					view.End(end - 1) - view.Offset(begin));
			}
		}

		return reporter.Take();
	}

	std::vector<Diagnostic> SemanticRules::AnalyzeMissingReturn(const FlowModel& flow)
	{
		const auto& model = flow.Model();
		if (!model.Tree().Diagnostics().empty())
		{
			return {};
		}

		const auto& functions = flow.Functions();
		const detail::TokenView view(model);
		Reporter reporter(model.Tree());
		for (FunctionId function = 0; function < functions.Size(); ++function)
		{
			const auto info   = GetReturnInfo(flow, function);
			const auto symbol = functions.symbol[function];
			if (functions.complete[function] == 0 || info.type == TypeTable::Unknown ||
				flow.Types().Types().IsBuiltin(flow.Types().Types().Strip(info.type),
				BuiltinType::Void) ||
				(symbol != kNone && model.Names().Text(model.Symbols().name[symbol]) == "main" &&
				model.Symbols().scope[symbol] == SemanticModel::TranslationUnitScope))
			{
				continue;
			}

			const auto endBlock = functions.fallthrough[function];
			if (endBlock == kNone ||!flow.IsReachable(function, endBlock))
			{
				continue;
			}

			const auto[begin, end] = view.Range(functions.body[function]);
			if (begin < end)
			{
				reporter.Add(RuleId::MissingReturn, Severity::Error, "cpp/missing-return",
					"function returning '" + flow.Types().Spell(info.type) +
					"' can reach the end without returning a value",
					view.Offset(end - 1), view.End(end - 1) - view.Offset(end - 1));
			}
		}

		return reporter.Take();
	}

	std::vector<Diagnostic> SemanticRules::AnalyzeReturnTypes(const FlowModel& flow)
	{
		const auto& model = flow.Model();
		if (!model.Tree().Diagnostics().empty())
		{
			return {};
		}

		const auto& types = flow.Types();
		const auto& nodes = model.Tree().NodesSoA();
		const detail::TokenView view(model);
		Reporter reporter(model.Tree());
		std::vector<ReturnInfo> returns;
		for (FunctionId function = 0; function < flow.Functions().Size(); ++function)
		{
			returns.push_back(GetReturnInfo(flow, function));
		}

		for (std::uint32_t node = 0; node < nodes.size(); ++node)
		{
			const auto block = flow.StatementBlock(node);
			if (nodes.Kind(node) != GrammarKind::ReturnStatement || block == kNone)
			{
				continue;
			}

			const auto function = flow.Blocks().function[block];
			const auto info     = returns[function];
			if (info.type == TypeTable::Unknown || flow.Functions().complete[function] == 0)
			{
				continue;
			}

			const auto[begin, end] = view.Range(node);
			const bool bare        = begin + 1 < end && view.At(begin + 1) == Tok::Semi;
			const bool isVoid = types.Types().IsBuiltin(types.Types().Strip(info.type),
				BuiltinType::Void);
			bool invalid = bare && !isVoid;
			for (const auto child : model.ChildrenOf(node))
			{
				const auto actual = types.NodeType(child);
				if (actual == TypeTable::Unknown)
				{
					continue;
				}

				invalid |= isVoid ? !types.Types().IsBuiltin(actual, BuiltinType::Void)
				: info.deduced ? types.Types().Strip(actual) != info.type
				: detail::CheckImplicitConversion(types, child, info.type) ==
					detail::ConversionResult::Invalid;
			}

			if (invalid && begin < end)
			{
				reporter.Add(RuleId::ReturnType, Severity::Error, "cpp/return-type",
					"return is incompatible with return type '" +
					types.Spell(info.type) + "'",
					view.Offset(begin), view.End(end - 1) - view.Offset(begin));
			}
		}

		return reporter.Take();
	}
}
