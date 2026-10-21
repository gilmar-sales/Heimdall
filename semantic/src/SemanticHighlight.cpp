#include <Heimdall/SemanticHighlight.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <Heimdall/TypeModel.hpp>

namespace heimdall
{
	namespace
	{
		struct ScopeRange
		{
			std::size_t begin = 0;
			std::size_t end   = 0;
			ScopeId scope     = 0;
		};

		std::vector<std::string> ScopePaths(const SemanticModel& model)
		{
			const auto& scopes  = model.Scopes();
			const auto& symbols = model.Symbols();
			std::vector<std::string> paths(scopes.Size());
			for (ScopeId scope = 1; scope < scopes.Size(); ++scope)
			{
				const auto parent = scopes.parent[scope];
				if (parent < paths.size())
				{
					paths[scope] = paths[parent];
				}

				if (scopes.owner[scope] < symbols.Size() &&
					(scopes.kind[scope] == ScopeKind::Namespace ||
					scopes.kind[scope] == ScopeKind::Class))
				{
					if (!paths[scope].empty())
					{
						paths[scope] += "::";
					}

					paths[scope] += model.Names().Text(symbols.name[scopes.owner[scope]]);
				}
			}

			return paths;
		}

		HighlightKind KindOf(const SemanticModel& model, SymbolId symbol)
		{
			const auto& symbols = model.Symbols();
			const auto scope    = symbols.scope[symbol];
			const bool member = scope < model.Scopes().Size() &&
				model.Scopes().kind[scope] == ScopeKind::Class;
			switch (symbols.kind[symbol])
			{
			case SymbolKind::Namespace:
				return HighlightKind::Namespace;
			case SymbolKind::Class:
				return HighlightKind::Class;
			case SymbolKind::Enum:
				return HighlightKind::Enum;
			case SymbolKind::Enumerator:
				return HighlightKind::EnumMember;
			case SymbolKind::Function:
				return member && !(symbols.flags[symbol] & SymbolFlag::Static)
				? HighlightKind::Method : HighlightKind::Function;
			case SymbolKind::Variable:
				return member && !(symbols.flags[symbol] & SymbolFlag::Static)
				? HighlightKind::Property : HighlightKind::Variable;
			case SymbolKind::Parameter:
				return HighlightKind::Parameter;
			case SymbolKind::TemplateParameter:
			{
				const auto parameter = model.ParameterOf(symbol);
				return parameter < model.TemplateParameters().kind.size() &&
					model.TemplateParameters().kind[parameter] == TemplateParameterKind::Value
				? HighlightKind::Parameter : HighlightKind::TypeParameter;
			}
			case SymbolKind::Concept:
				return HighlightKind::Concept;
			case SymbolKind::Macro:
				return HighlightKind::Macro;
			default:
				return HighlightKind::Type;
			}
		}

		std::uint32_t ModifiersOf(
			const SemanticModel& model,
			SymbolId symbol,
			const TypeModel& typed)
		{
			const auto& symbols = model.Symbols();
			const auto type     = typed.SymbolType(symbol);
			const bool object = symbols.kind[symbol] == SymbolKind::Variable ||
				symbols.kind[symbol] == SymbolKind::Parameter;
			auto modifiers = symbols.kind[symbol] == SymbolKind::Enumerator ||
				(object && typed.Types().IsKnown(type) && typed.Types().IsConstQualified(type))
			? HighlightMask(HighlightModifier::Readonly) : 0u;
			if (symbols.flags[symbol] & SymbolFlag::Static)
			{
				modifiers |= HighlightMask(HighlightModifier::Static);
			}

			auto scope = symbols.scope[symbol];
			while (scope < model.Scopes().Size())
			{
				const auto kind = model.Scopes().kind[scope];
				if (kind == ScopeKind::Function || kind == ScopeKind::Block)
				{
					return modifiers | HighlightMask(HighlightModifier::FunctionScope);
				}

				if (kind == ScopeKind::Class)
				{
					return modifiers | HighlightMask(HighlightModifier::ClassScope);
				}

				scope = model.Scopes().parent[scope];
			}

			return modifiers | HighlightMask(HighlightModifier::GlobalScope);
		}

		bool Defines(const SemanticModel& model, SymbolId symbol)
		{
			const auto& symbols = model.Symbols();
			const auto kind     = symbols.kind[symbol];
			if (kind == SymbolKind::Parameter || kind == SymbolKind::Namespace ||
				kind == SymbolKind::TemplateParameter)
			{
				return false;
			}

			if (kind == SymbolKind::Function)
			{
				return (symbols.flags[symbol] & SymbolFlag::Definition) != 0;
			}

			const auto& tree = model.Tree();
			const auto node  = symbols.decl_node[symbol];
			if (node >= tree.NodesSoA().size())
			{
				return false;
			}

			const auto begin = tree.NodesSoA().FirstToken(node);
			const auto end = std::min<std::size_t>(begin + tree.NodesSoA().TokenCount(node),
				tree.Tokens().size());
			for (auto token = begin; token < end; ++token)
			{
				if (tree.Text(tree.Tokens()[token]) == "extern")
				{
					return false;
				}

				if ((kind == SymbolKind::Class || kind == SymbolKind::Enum) &&
					tree.Text(tree.Tokens()[token]) == "{")
				{
					return true;
				}
			}

			return kind != SymbolKind::Class && kind != SymbolKind::Enum;
		}

		bool NamedScope(const SemanticModel& model, ScopeId scope)
		{
			const auto& scopes = model.Scopes();
			while (scope != 0 && scope < scopes.Size())
			{
				if (scopes.kind[scope] != ScopeKind::Template &&
					((scopes.kind[scope] != ScopeKind::Namespace && scopes.kind[scope] != ScopeKind::Class) ||
					scopes.owner[scope] == kNone))
				{
					return false;
				}

				scope = scopes.parent[scope];
			}

			return scope == 0;
		}

		std::string AliasTarget(const SemanticModel& model, SymbolId symbol)
		{
			if (model.Symbols().kind[symbol] != SymbolKind::TypeAlias)
			{
				return {};
			}

			const auto& tree = model.Tree();
			const auto node  = model.Symbols().decl_node[symbol];
			if (node >= tree.NodesSoA().size())
			{
				return {};
			}

			const auto end = std::min<std::size_t>(tree.NodesSoA().FirstToken(node) +
				tree.NodesSoA().TokenCount(node), tree.Tokens().size());
			std::string target;
			bool afterEquals = false;
			for (auto token = tree.NodesSoA().FirstToken(node); token < end; ++token)
			{
				const auto& part = tree.Tokens()[token];
				const auto text  = tree.Text(part);
				if (text == "=")
				{
					afterEquals = true;
				}
				else if (afterEquals && (part.kind == TokenKind::Identifier || text == "::"))
				{
					target += text;
				}
				else if (afterEquals && part.kind != TokenKind::Whitespace)
				{
					return text == ";" ? target : std::string {};
				}
			}

			return target;
		}

		std::string DeclaredHead(const SemanticModel& model, SymbolId symbol)
		{
			const auto& tree = model.Tree();
			const auto node  = model.Symbols().decl_node[symbol];
			if (node >= tree.NodesSoA().size())
			{
				return {};
			}

			std::string head;
			bool component = true;
			for (auto token = tree.NodesSoA().FirstToken(node);
				token < model.Symbols().decl_token[symbol] && token < tree.Tokens().size(); ++token)
			{
				const auto& part = tree.Tokens()[token];
				if (part.kind == TokenKind::Whitespace || part.kind == TokenKind::LineComment ||
					part.kind == TokenKind::BlockComment)
				{
					continue;
				}

				const auto text = tree.Text(part);
				if (part.tok == Tok::ColonColon)
				{
					head += text;
					component = true;
				}
				else if (part.kind == TokenKind::Identifier && part.tok == Tok::None && component)
				{
					head += text;
					component = false;
				}
				else if (!head.empty())
				{
					break;
				}
			}

			return head;
		}
	}

	SemanticHighlightIndex SemanticHighlightIndex::FromModel(const SemanticModel& model)
	{
		SemanticHighlightIndex index;
		const auto paths    = ScopePaths(model);
		const auto typed    = Typer::Type(model);
		const auto& symbols = model.Symbols();
		for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
		{
			const auto scope = symbols.scope[symbol];
			if (scope >= paths.size() ||!NamedScope(model, scope) ||
				symbols.kind[symbol] == SymbolKind::TemplateParameter ||
				symbols.kind[symbol] == SymbolKind::Parameter)
			{
				continue;
			}

			auto name = paths[scope];
			if (!name.empty())
			{
				name += "::";
			}

			name += model.Names().Text(symbols.name[symbol]);
			const HighlightSymbol info
			{
				KindOf(model, symbol), ModifiersOf(model, symbol, typed),
					AliasTarget(model, symbol)
			};
			auto[entry, inserted] = index.mSymbols.try_emplace(name, info);
			if (!inserted && entry->second != info)
			{
				entry->second.reset();
			}
		}

		return index;
	}

	void SemanticHighlightIndex::Merge(const SemanticHighlightIndex& other)
	{
		for (const auto& [name, info] : other.mSymbols)
		{
			auto[entry, inserted] = mSymbols.try_emplace(name, info);
			if (!inserted && entry->second != info)
			{
				entry->second.reset();
			}
		}
	}

	const HighlightSymbol* SemanticHighlightIndex::Find(std::string_view qualifiedName) const
	{
		std::string name(qualifiedName);
		if (name.starts_with("::"))
		{
			name.erase(0, 2);
		}

		std::vector<std::string_view> visited;
		while (true)
		{
			const auto entry = mSymbols.find(name);
			if (entry != mSymbols.end())
			{
				return entry->second ? & *entry->second : nullptr;
			}

			auto split    = name.rfind("::");
			bool replaced = false;
			while (split != std::string::npos)
			{
				const auto alias = mSymbols.find(name.substr(0, split));
				if (alias != mSymbols.end() && alias->second && !alias->second->aliasTarget.empty())
				{
					if (std::ranges::find(visited, std::string_view(alias->first)) != visited.end())
					{
						return nullptr;
					}

					visited.push_back(alias->first);
					auto target      = alias->second->aliasTarget;
					const auto owner = name.rfind("::", split == 0 ? 0 : split - 1);
					if (!target.starts_with("::") && target.find("::") == std::string::npos &&
						owner != std::string::npos)
					{
						target = name.substr(0, owner + 2) + target;
					}

					if (target.starts_with("::"))
					{
						target.erase(0, 2);
					}

					name     = target + name.substr(split);
					replaced = true;
					break;
				}

				split = split == 0 ? std::string::npos : name.rfind("::", split - 1);
			}

			if (!replaced)
			{
				return nullptr;
			}
		}

		return nullptr;
	}

	std::vector<HighlightSpan> SemanticHighlight::Extract(
		const SemanticModel& model,
		std::span<const HighlightSpan> lexical,
		const SemanticHighlightIndex* headers)
	{
		const auto& tree        = model.Tree();
		const auto& tokens      = tree.Tokens();
		const auto& nodes       = tree.NodesSoA();
		const auto& symbols     = model.Symbols();
		const auto& scopes      = model.Scopes();
		const auto& significant = model.Significant();
		const auto paths        = ScopePaths(model);
		const auto typed        = Typer::Type(model);
		std::vector<std::uint32_t> symbolModifiers(symbols.Size());
		std::vector<HighlightKind> symbolKinds(symbols.Size());
		for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
		{
			symbolModifiers[symbol] = ModifiersOf(model, symbol, typed);
			symbolKinds[symbol]     = KindOf(model, symbol);
		}

		std::vector<SymbolId> targets(tokens.size(), kNone);
		std::vector<bool> declarations(tokens.size(), false);
		std::vector<ScopeRange> ranges;
		ranges.reserve(scopes.Size());
		for (ScopeId scope = 0; scope < scopes.Size(); ++scope)
		{
			const auto node = scopes.node[scope];
			if (node < nodes.size())
			{
				ranges.push_back({nodes.FirstToken(node),
						nodes.FirstToken(node) + nodes.TokenCount(node), scope});
			}
		}

		std::ranges::sort(ranges,[](const ScopeRange& left, const ScopeRange& right)
			{
				if (left.begin != right.begin)
				{
					return left.begin < right.begin;
				}

				return left.end != right.end ? left.end > right.end : left.scope < right.scope;
			});
		for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
		{
			const auto token = symbols.decl_token[symbol];
			if (token < tokens.size())
			{
				targets[token]      = symbol;
				declarations[token] = true;
			}
		}

		const auto& references = model.Refs();
		for (std::size_t reference = 0; reference < references.token.size(); ++reference)
		{
			const auto token = references.token[reference];
			if (token < tokens.size() && !declarations[token] &&
				references.state[reference] == RefState::Resolved)
			{
				targets[token] = references.target[reference];
			}
		}

		for (std::uint32_t node = 0; node < nodes.size(); ++node)
		{
			const auto symbol = typed.MemberSymbol(node);
			if (symbol >= symbols.Size())
			{
				continue;
			}

			const auto children = model.ChildrenOf(node);
			if (!children.empty())
			{
				const auto child = children.back();
				if (nodes.Kind(child) == GrammarKind::IdentifierExpression)
				{
					const auto token = nodes.FirstToken(child);
					if (token < tokens.size())
					{
						targets[token] = symbol;
					}
				}
			}
		}

		std::vector<HighlightSpan> enriched;
		enriched.reserve(significant.size());
		std::vector<ScopeRange> active;
		std::size_t nextRange = 0;
		for (std::size_t position = 0; position < significant.size(); ++position)
		{
			const auto token = significant[position];
			while (!active.empty() && active.back().end <= token)
			{
				active.pop_back();
			}

			while (nextRange < ranges.size() && ranges[nextRange].begin <= token)
			{
				if (ranges[nextRange].end > token)
				{
					active.push_back(ranges[nextRange]);
				}

				++nextRange;
			}

			if (tokens[token].kind != TokenKind::Identifier)
			{
				continue;
			}

			const auto scope    = active.empty() ? ScopeId {0}: active.back().scope;
			const auto text     = tree.Text(tokens[token]);
			const auto previous = position == 0 ? std::string_view {}: tree.Text(tokens[significant[position - 1]]);
			const bool member   = previous == "." || previous == "->";
			auto symbol         = targets[token];
			if (symbol >= symbols.Size() && !member && previous != "::")
			{
				symbol = model.Lookup(scope, model.Names().Find(text), token);
			}

			std::string qualified(text);
			auto first = position;
			while (first >= 2 && tree.Text(tokens[significant[first - 1]]) == "::" &&
				tokens[significant[first - 2]].kind == TokenKind::Identifier &&
				tokens[significant[first - 2]].tok == Tok::None)
			{
				first -= 2;
				qualified = std::string(tree.Text(tokens[significant[first]])) + "::" + qualified;
			}

			const bool absolute = first > 0 && tree.Text(tokens[significant[first - 1]]) == "::";
			if (previous == "::" && first == position && position >= 2)
			{
				const auto qualifier = tokens[significant[position - 2]].tok;
				if (qualifier == Tok::Gt || qualifier == Tok::Shr || qualifier == Tok::RParen ||
					qualifier == Tok::RBracket)
				{
					continue;
				}
			}

			if (previous == "::" && first < position)
			{
				auto owner = model.Lookup(absolute ? ScopeId {0}: scope,
					model.Names().Find(tree.Text(tokens[significant[first]])), token);
				if (owner < symbols.Size() && symbols.kind[owner] == SymbolKind::TypeAlias)
				{
					const auto type = typed.Types().Strip(typed.SymbolType(owner));
					auto target     = AliasTarget(model, owner);
					if (target.empty())
					{
						target = typed.ExternalHead(typed.SymbolType(owner));
					}

					if (!target.empty())
					{
						qualified = std::string(target) + qualified.substr(qualified.find("::"));
					}

					if (typed.Types().Kind(type) == TypeKind::Class ||
						typed.Types().Kind(type) == TypeKind::Enum)
					{
						owner = typed.Types().Arg(type);
					}
				}

				for (auto part = first + 2; owner < symbols.Size() && part <= position; part += 2)
				{
					owner = model.LookupMember(owner, model.Names().Find(tree.Text(tokens[significant[part]])));
				}

				if (owner < symbols.Size())
				{
					symbol = owner;
				}
			}

			if (symbol < symbols.Size())
			{
				targets[token]  = symbol;
				auto modifiers  = symbolModifiers[symbol];
				const auto kind = symbolKinds[symbol];
				if (declarations[token])
				{
					modifiers |= kHighlightDeclaration;
					if (Defines(model, symbol))
					{
						modifiers |= HighlightMask(HighlightModifier::Definition);
					}
				}

				enriched.push_back({tokens[token].offset, tokens[token].length, kind, modifiers});
			}
			else if (headers)
			{
				const HighlightSymbol* external = nullptr;
				if (member)
				{
					if (position < 2 || targets[significant[position - 2]] >= symbols.Size())
					{
						continue;
					}

					auto type = typed.Types().Strip(typed.SymbolType(targets[significant[position - 2]]));
					if (previous == "->")
					{
						if (!typed.Types().IsPointer(type))
						{
							continue;
						}

						type = typed.Types().Strip(typed.Types().Element(type));
					}

					auto head = std::string(typed.ExternalHead(type));
					if (head.empty())
					{
						head = DeclaredHead(model, targets[significant[position - 2]]);
					}

					if (head.empty())
					{
						continue;
					}

					qualified = std::string(head) + "::" + std::string(text);
				}

				auto parent = absolute ? ScopeId {0}: scope;
				while (parent < scopes.Size())
				{
					const auto name = paths[parent].empty() ? qualified : paths[parent] + "::" + qualified;
					external        = headers->Find(name);
					if (external)
					{
						break;
					}

					parent = scopes.parent[parent];
				}

				if (external)
				{
					enriched.push_back({tokens[token].offset, tokens[token].length,
							external->kind, external->modifiers});
				}
			}
		}

		std::vector<HighlightSpan> result;
		result.reserve(lexical.size() + enriched.size());
		std::size_t original = 0;
		for (const auto& span : enriched)
		{
			while (original < lexical.size() && lexical[original].offset < span.offset)
			{
				result.push_back(lexical[original++]);
			}

			auto replacement = span;
			if (original < lexical.size() && lexical[original].offset == span.offset)
			{
				const auto& old = lexical[original++];
				if (old.modifiers & kHighlightInactive || old.kind == HighlightKind::Macro ||
					old.length != span.length)
				{
					result.push_back(old);
					continue;
				}

				replacement.modifiers |= old.modifiers & kHighlightDeclaration;
			}

			result.push_back(replacement);
		}

		result.insert(result.end(), lexical.begin() + original, lexical.end());
		return result;
	}
}
