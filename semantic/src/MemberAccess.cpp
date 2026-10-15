#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <Heimdall/LineTable.hpp>
#include <Heimdall/SemanticRules.hpp>

#include "detail/TokenView.hpp"

namespace heimdall
{
	std::vector<Diagnostic> SemanticRules::AnalyzeDotOnPointer(const TypeModel& types)
	{
		const auto& model = types.Model();
		const auto& nodes = model.Tree().NodesSoA();
		const detail::TokenView view(model);
		const auto& table = types.Types();

		std::vector<Diagnostic> diagnostics;
		std::vector<TypeId> resolved(nodes.size(), TypeTable::Unknown);
		std::vector<std::uint8_t> state(nodes.size(), 0);
		std::function<TypeId(std::uint32_t) > resolve = [&](std::uint32_t node) -> TypeId
		{
			if (node >= nodes.size())
			{
				return TypeTable::Unknown;
			}

			if (state[node] == 2)
			{
				return resolved[node];
			}

			if (state[node] == 1)
			{
				return TypeTable::Unknown;
			}

			state[node] = 1;
			TypeId type = types.NodeType(node);
			if (!table.IsKnown(type) && nodes.Kind(node) == GrammarKind::MemberExpression)
			{
				const auto children = model.ChildrenOf(node);
				if (children.size() == 2)
				{
					const auto[objectBegin, objectEnd] = view.Range(children[0]);
					const auto[nameBegin, nameEnd]     = view.Range(children[1]);
					const Tok op                       = view.At(objectEnd);
					TypeId object                      = table.Strip(resolve(children[0]));
					if ((op == Tok::Arrow || op == Tok::Dot) && table.IsPointer(object))
					{
						object = table.Strip(table.Element(object));
					}

					if ((op == Tok::Dot || op == Tok::Arrow) && nameEnd == nameBegin + 1 &&
						table.Kind(object) == TypeKind::Class)
					{
						const auto name = model.Names().Find(view.Text(nameBegin));
						const auto member = name == kNone
						? kNone : model.LookupMember(table.Arg(object), name);
						if (member != kNone)
						{
							type = types.SymbolType(member);
						}
					}
				}
			}

			resolved[node] = type;
			state[node]    = 2;
			return type;
		};
		LineTable lines;
		bool linesBuilt = false;
		for (std::uint32_t node = 0; node < nodes.size(); ++node)
		{
			if (nodes.Kind(node) != GrammarKind::MemberExpression)
			{
				continue;
			}

			const auto children = model.ChildrenOf(node);
			if (children.size() != 2)
			{
				continue;
			}

			const auto[objectBegin, objectEnd] = view.Range(children[0]);
			if (view.At(objectEnd) != Tok::Dot)
			{
				continue;
			}

			// Only a receiver the typer knows is a raw pointer. Class types with
			// an overloaded operator->, unknown receivers and non-pointer
			// scalars stay silent.
			const auto receiver = table.Strip(resolve(children[0]));
			if (table.Kind(receiver) != TypeKind::Pointer)
			{
				continue;
			}

			const auto[nameBegin, nameEnd] = view.Range(children[1]);
			if (nameEnd != nameBegin + 1 ||!view.IsWord(nameBegin))
			{
				continue;
			}

			if (!linesBuilt)
			{
				lines.Build(model.Tree().Source());
				linesBuilt = true;
			}

			const auto source = model.Tree().Source();
			const auto object = source.substr(
				view.Offset(objectBegin), view.End(objectEnd - 1) - view.Offset(objectBegin));
			const auto offset     = view.Offset(nameBegin);
			const auto position   = lines.Lookup(offset);
			const auto dot_offset = view.Offset(objectEnd);
			diagnostics.push_back(Diagnostic {
					RuleId::DotOnPointer, Severity::Error, "cpp/dot-on-pointer",
					"request for member '" + std::string(view.Text(nameBegin)) + "' in '" +
					std::string(object) + "', which is of pointer type '" +
					types.Spell(receiver) + "' (maybe you meant to use '->'?)",
					offset, view.End(nameBegin) - offset, position.line, position.column,
					true, TextEdit {dot_offset, 1, "->"}, true, "Replace '.' with '->'"
			});
		}

		return diagnostics;
	}
} // namespace heimdall
