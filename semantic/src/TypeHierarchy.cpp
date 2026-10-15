#include <Heimdall/TypeHierarchy.hpp>

#include <algorithm>

namespace heimdall
{
	namespace
	{
		SymbolId ClassOfType(const TypeModel& types, TypeId type)
		{
			const auto& table = types.Types();
			for (std::size_t steps = 0; steps < table.Size(); ++steps)
			{
				switch (table.Kind(type))
				{
				case TypeKind::Class:
					return table.Arg(type);
				case TypeKind::Const:
				case TypeKind::LRef:
				case TypeKind::RRef:
				case TypeKind::Pointer:
					type = table.Arg(type);
					break;
				default:
					return kNone;
				}
			}

			return kNone;
		}

		SymbolId DeclaredAt(const SymbolTable& symbols, std::uint32_t token)
		{
			for (SymbolId i = 0; i < symbols.Size(); ++i)
			{
				if (symbols.decl_token[i] == token)
				{
					return i;
				}
			}

			return kNone;
		}

		// Innermost scope whose defining node covers `token`; scopes are created
		// outside-in, so the last match is the deepest.
		ScopeId ScopeOfToken(const SemanticModel& model, std::uint32_t token)
		{
			const auto& soa    = model.Tree().NodesSoA();
			const auto& scopes = model.Scopes();
			ScopeId best       = SemanticModel::TranslationUnitScope;
			for (ScopeId scope = 1; scope < scopes.Size(); ++scope)
			{
				const auto node = scopes.node[scope];
				if (node < soa.size() && token >= soa.FirstToken(node) &&
					token < soa.FirstToken(node) + soa.TokenCount(node))
				{
					best = scope;
				}
			}

			return best;
		}

		SymbolId LookupSpelling(const SemanticModel& model, std::uint32_t token)
		{
			const auto name = model.Names().Find(model.Tree().Text(model.Tree().Tokens()[token]));
			return name == kNone ? kNone : model.Lookup(ScopeOfToken(model, token), name, token);
		}

		// Names the binder does not record as references: type uses, and members
		// reached through a receiver (`a.name`, `a->name`).
		SymbolId ResolveByContext(
			const SemanticModel& model,
			const TypeModel& types,
			std::uint32_t token)
		{
			const auto& sig = model.Significant();
			const auto at   = std::ranges::lower_bound(sig, token);
			if (at == sig.end() || *at != token)
			{
				return kNone;
			}

			const auto& tokens = model.Tree().Tokens();
			if (at - sig.begin() < 2 ||
				(tokens[at[-1]].tok != Tok::Dot && tokens[at[-1]].tok != Tok::Arrow))
			{
				return LookupSpelling(model, token);
			}

			const auto receiver = at[-2];
			SymbolId variable   = model.ResolveToken(receiver);
			if (variable == kNone)
			{
				variable = LookupSpelling(model, receiver);
			}

			const auto klass =
				variable == kNone ? kNone : ClassOfType(types, types.SymbolType(variable));
			const auto name = model.Names().Find(model.Tree().Text(tokens[token]));
			return klass == kNone || name == kNone ? kNone : model.LookupMember(klass, name);
		}
	} // namespace

	std::uint32_t TypeHierarchy::TokenAt(const SemanticModel& model, std::size_t offset)
	{
		const auto& tree   = model.Tree();
		const auto& tokens = tree.Tokens();
		for (const auto index : model.Significant())
		{
			const Token& token = tokens[index];
			if (token.kind == TokenKind::Identifier && offset >= token.offset &&
				offset <= static_cast<std::size_t>(token.offset) + token.length)
			{
				return index;
			}
		}

		return kNone;
	}

	SymbolId TypeHierarchy::FindClassAt(
		const SemanticModel& model,
		const TypeModel& types,
		std::uint32_t token)
	{
		const auto& symbols = model.Symbols();
		SymbolId symbol     = model.ResolveToken(token);
		if (symbol == kNone)
		{
			symbol = DeclaredAt(symbols, token);
		}

		if (symbol == kNone)
		{
			symbol = ResolveByContext(model, types, token);
		}

		if (symbol == kNone)
		{
			return kNone;
		}

		const auto ownerClass = [&](SymbolId member)
		{
			const auto scope = symbols.scope[member];
			return scope < model.Scopes().Size() && model.Scopes().kind[scope] == ScopeKind::Class
			? model.Scopes().owner[scope]
			: kNone;
		};

		switch (symbols.kind[symbol])
		{
		case SymbolKind::Class:
			return symbol;
		case SymbolKind::Function:
			return ownerClass(symbol);
		case SymbolKind::TypeAlias:
		case SymbolKind::Parameter:
			return ClassOfType(types, types.SymbolType(symbol));
		case SymbolKind::Variable:
			return ownerClass(symbol) != kNone ? kNone
			: ClassOfType(types, types.SymbolType(symbol));
		default:
			return kNone;
		}
	}

	std::vector<SymbolId> TypeHierarchy::Parents(const SemanticModel& model, SymbolId klass)
	{
		std::vector<SymbolId> result;
		const auto& symbols = model.Symbols();
		if (klass >= symbols.Size())
		{
			return result;
		}

		const auto& bases = model.Bases();
		const auto first  = symbols.first_base[klass];
		const auto count  = symbols.base_count[klass];
		for (std::uint32_t i = 0; i < count; ++i)
		{
			SymbolId target = bases.target[first + i];
			if (target == kNone && bases.name[first + i] != kNone)
			{
				const auto candidate =
					model.Lookup(symbols.scope[klass], bases.name[first + i], symbols.decl_token[klass]);
				if (candidate != kNone && symbols.kind[candidate] == SymbolKind::Class &&
					(symbols.flags[candidate] & SymbolFlag::Template) != 0)
				{
					target = candidate;
				}
			}

			if (target != kNone && target != klass &&
				std::ranges::find(result, target) == result.end())
			{
				result.push_back(target);
			}
		}

		return result;
	}

	std::vector<SymbolId> TypeHierarchy::Children(const SemanticModel& model, SymbolId klass)
	{
		std::vector<SymbolId> result;
		const auto& symbols = model.Symbols();
		for (SymbolId i = 0; i < symbols.Size(); ++i)
		{
			if (symbols.kind[i] != SymbolKind::Class || symbols.base_count[i] == 0)
			{
				continue;
			}

			const auto parents = Parents(model, i);
			if (std::ranges::find(parents, klass) != parents.end())
			{
				result.push_back(i);
			}
		}

		return result;
	}
} // namespace heimdall
