#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <Heimdall/Lexer.hpp>
#include <Heimdall/LineTable.hpp>
#include <Heimdall/SemanticRules.hpp>

namespace heimdall
{
	namespace
	{
		std::string_view Trim(std::string_view text)
		{
			while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
				text.front() == '\r' || text.front() == '\n'))
			{
				text.remove_prefix(1);
			}

			while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
				text.back() == '\r' || text.back() == '\n'))
			{
				text.remove_suffix(1);
			}

			return text;
		}

		SymbolId ClassOfExpression(
			const TypeModel& types,
			std::uint32_t node,
			std::string_view& instanceType)
		{
			const auto& model = types.Model();
			const auto& tree  = model.Tree();
			const auto& nodes = tree.NodesSoA();
			const auto& table = types.Types();
			TypeId type       = table.Strip(types.NodeType(node));
			if (table.IsPointer(type))
			{
				type = table.Strip(table.Element(type));
			}

			if (table.Kind(type) == TypeKind::Class)
			{
				return table.Arg(type);
			}

			if (node >= nodes.size() || nodes.Kind(node) != GrammarKind::IdentifierExpression)
			{
				return kNone;
			}

			const auto variable = model.ResolveToken(nodes.FirstToken(node));
			const auto& symbols = model.Symbols();
			if (variable != kNone && symbols.kind[variable] == SymbolKind::Class)
			{
				instanceType = model.Names().Text(symbols.name[variable]);
				return variable;
			}

			if (variable == kNone || symbols.decl_node[variable] >= nodes.size())
			{
				return kNone;
			}

			const auto declaration = symbols.decl_node[variable];
			const auto name_token  = symbols.decl_token[variable];
			const auto scope       = model.ScopeOfNode(declaration);
			const auto& tokens     = tree.Tokens();
			for (auto token = nodes.FirstToken(declaration); token < name_token && token < tokens.size(); ++token)
			{
				if (tokens[token].kind != TokenKind::Identifier)
				{
					continue;
				}

				const auto name      = model.Names().Find(tree.Text(tokens[token]));
				const auto candidate = name == kNone ? kNone : model.Lookup(scope, name, token);
				if (candidate != kNone && symbols.kind[candidate] == SymbolKind::Class)
				{
					const auto& source   = tree.Source();
					const auto typeStart = tokens[token].offset;
					const auto typeEnd   = tokens[name_token].offset;
					if (typeEnd >= typeStart && typeEnd <= source.size())
					{
						instanceType = Trim(source.substr(typeStart, typeEnd - typeStart));
						while (!instanceType.empty() &&
							(instanceType.back() == '*' || instanceType.back() == '&'))
						{
							instanceType = Trim(instanceType.substr(0, instanceType.size() - 1));
						}
					}

					return candidate;
				}
			}

			bool inInitializer = false;
			const auto declarationEnd = std::min<std::size_t>(
				nodes.FirstToken(declaration) + nodes.TokenCount(declaration), tokens.size());
			for (auto token = name_token + 1; token < declarationEnd; ++token)
			{
				if (!inInitializer)
				{
					inInitializer = tokens[token].tok == Tok::Eq;
					continue;
				}

				if (tokens[token].kind != TokenKind::Identifier)
				{
					continue;
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

				const auto functionScope = model.ScopeOfNode(functionNode);
				for (auto typeToken = nodes.FirstToken(functionNode);
					typeToken < functionName && typeToken < tokens.size(); ++typeToken)
				{
					if (tokens[typeToken].kind != TokenKind::Identifier)
					{
						continue;
					}

					const auto typeName = model.Names().Find(tree.Text(tokens[typeToken]));
					const auto typeSymbol = typeName == kNone
					? kNone : model.Lookup(functionScope, typeName, typeToken);
					if (typeSymbol == kNone || symbols.kind[typeSymbol] != SymbolKind::Class)
					{
						continue;
					}

					const auto typeStart  = tokens[typeToken].offset;
					const auto typeFinish = tokens[functionName].offset;
					instanceType          = Trim(tree.Source().substr(typeStart, typeFinish - typeStart));
					return typeSymbol;
				}
			}

			bool initializer = false;
			const auto declarationEndWithType = std::min<std::size_t>(
				nodes.FirstToken(declaration) + nodes.TokenCount(declaration), tokens.size());
			for (auto token = name_token + 1; token < declarationEndWithType; ++token)
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
				if (candidate == kNone || symbols.kind[candidate] != SymbolKind::Class)
				{
					continue;
				}

				auto typeEnd = token;
				if (token + 1 < declarationEndWithType && tokens[token + 1].tok == Tok::Lt)
				{
					int depth = 0;
					for (auto i = token + 1; i < declarationEndWithType; ++i)
					{
						if (tokens[i].tok == Tok::Lt)
						{
							++depth;
						}
						else if (tokens[i].tok == Tok::Gt && --depth == 0)
						{
							typeEnd = i;
							break;
						}
					}
				}

				const auto& source    = tree.Source();
				const auto typeStart  = tokens[token].offset;
				const auto typeFinish = tokens[typeEnd].offset + tokens[typeEnd].length;
				if (typeFinish <= source.size())
				{
					instanceType = Trim(source.substr(typeStart, typeFinish - typeStart));
				}

				return candidate;
			}

			return kNone;
		}

		std::string SpelledMemberType(
			const SemanticModel& model,
			SymbolId owner,
			SymbolId member,
			std::string_view instanceType)
		{
			const auto& tree       = model.Tree();
			const auto& nodes      = tree.NodesSoA();
			const auto& tokens     = tree.Tokens();
			const auto& symbols    = model.Symbols();
			const auto declaration = symbols.decl_node[member];
			if (declaration >= nodes.size())
			{
				return {};
			}

			const auto first     = nodes.FirstToken(declaration);
			const auto nameToken = symbols.decl_token[member];
			if (first >= tokens.size() || nameToken >= tokens.size() || first > nameToken)
			{
				return {};
			}

			auto type = Trim(tree.Source().substr(tokens[first].offset,
				tokens[nameToken].offset - tokens[first].offset));
			const auto templateOpen  = instanceType.find('<');
			const auto templateClose = instanceType.rfind('>');
			if (templateOpen != std::string_view::npos &&
				templateClose != std::string_view::npos && templateClose > templateOpen)
			{
				const auto record      = symbols.decl_node[owner];
				const auto recordStart = nodes.FirstToken(record);
				std::vector<std::string_view> parameters;
				for (auto token = recordStart; token > 0;)
				{
					--token;
					if (tokens[token].tok == Tok::KwTemplate)
					{
						for (auto i = token + 1; i < recordStart; ++i)
						{
							if (tokens[i].tok == Tok::KwClass || tokens[i].tok == Tok::KwTypename)
							{
								for (++i; i < recordStart && tokens[i].kind != TokenKind::Identifier; ++i) {}

								if (i < recordStart)
								{
									parameters.push_back(tree.Text(tokens[i]));
								}
							}
						}

						break;
					}

					if (tokens[token].tok == Tok::Semi || tokens[token].tok == Tok::RBrace)
					{
						break;
					}
				}

				std::vector<std::string_view> arguments;
				std::size_t argumentStart = templateOpen + 1;
				int depth                 = 0;
				for (auto i = argumentStart; i <= templateClose; ++i)
				{
					if (i == templateClose ||(instanceType[i] == ',' && depth == 0))
					{
						arguments.push_back(Trim(instanceType.substr(argumentStart, i - argumentStart)));
						argumentStart = i + 1;
					}
					else if (instanceType[i] == '<')
					{
						++depth;
					}
					else if (instanceType[i] == '>' && depth > 0)
					{
						--depth;
					}
				}

				if (!parameters.empty() && parameters.size() == arguments.size())
				{
					const auto lexed = Lexer(type).Lex();
					std::string substituted;
					std::size_t cursor = 0;
					for (const auto& token : lexed)
					{
						if (token.kind != TokenKind::Identifier)
						{
							continue;
						}

						const auto text = type.substr(token.offset, token.length);
						for (std::size_t i = 0; i < parameters.size(); ++i)
						{
							if (text == parameters[i])
							{
								substituted.append(type.substr(cursor, token.offset - cursor));
								substituted.append(arguments[i]);
								cursor = token.offset + token.length;
								break;
							}
						}
					}

					if (cursor != 0)
					{
						substituted.append(type.substr(cursor));
						return substituted;
					}
				}
			}

			return std::string(type);
		}

		SymbolId EnclosingClass(const SemanticModel& model, std::uint32_t node)
		{
			const auto& scopes = model.Scopes();
			for (auto scope = model.ScopeOfNode(node); scope != kNone && scope < scopes.Size();
				scope = scopes.parent[scope])
			{
				if (scopes.kind[scope] == ScopeKind::Class && scopes.owner[scope] != kNone)
				{
					return scopes.owner[scope];
				}
			}

			return kNone;
		}

		bool DerivesFrom(const SemanticModel& model, SymbolId derived, SymbolId base)
		{
			if (derived == base)
			{
				return true;
			}

			const auto& bases = model.Bases();
			std::vector<SymbolId> pending {derived};
			std::vector<SymbolId> seen;
			while (!pending.empty())
			{
				const auto current = pending.back();
				pending.pop_back();
				if (std::ranges::find(seen, current) != seen.end())
				{
					continue;
				}

				seen.push_back(current);
				for (std::size_t i = 0; i < bases.derived.size(); ++i)
				{
					if (bases.derived[i] != current)
					{
						continue;
					}

					if (bases.target[i] == base)
					{
						return true;
					}

					if (bases.target[i] != kNone)
					{
						pending.push_back(bases.target[i]);
					}
				}
			}

			return false;
		}

		bool IsNestedWithin(const SemanticModel& model, SymbolId nested, SymbolId enclosing)
		{
			const auto& symbols = model.Symbols();
			const auto& scopes  = model.Scopes();
			if (nested >= symbols.Size() || enclosing >= symbols.Size())
			{
				return false;
			}

			const auto target_scope = symbols.member_scope[enclosing];
			for (auto scope = symbols.member_scope[nested]; scope != kNone && scope < scopes.Size();
				scope = scopes.parent[scope])
			{
				if (scope == target_scope)
				{
					return true;
				}
			}

			return false;
		}

		bool HasFriendAccess(
			const SemanticModel& model,
			SymbolId owner,
			SymbolId contextClass,
			std::uint32_t accessNode)
		{
			if (owner >= model.Symbols().Size())
			{
				return false;
			}

			const auto& tree  = model.Tree();
			const auto& nodes = tree.NodesSoA();
			const auto record = model.Symbols().decl_node[owner];
			if (record >= nodes.size())
			{
				return false;
			}

			const auto begin    = nodes.FirstToken(record);
			const auto end      = begin + nodes.TokenCount(record);
			const auto& tokens  = tree.Tokens();
			const auto& symbols = model.Symbols();
			std::string_view callerFunction;
			for (auto scope = model.ScopeOfNode(accessNode); scope != kNone && scope < model.Scopes().Size();
				scope = model.Scopes().parent[scope])
			{
				if (model.Scopes().kind[scope] != ScopeKind::Function)
				{
					continue;
				}

				const auto functionNode = model.Scopes().node[scope];
				for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
				{
					if (symbols.kind[symbol] == SymbolKind::Function &&
						symbols.decl_node[symbol] == functionNode)
					{
						callerFunction = model.Names().Text(symbols.name[symbol]);
						break;
					}
				}

				break;
			}

			std::size_t body = begin;
			while (body < end && tokens[body].tok != Tok::LBrace)
			{
				++body;
			}

			if (body == end)
			{
				return false;
			}

			for (auto i = body + 1; i < end; ++i)
			{
				if (tokens[i].tok == Tok::LBrace)
				{
					std::size_t depth = 1;
					while (++i < end && depth != 0)
					{
						depth += tokens[i].tok == Tok::LBrace ? 1 : 0;
						depth -= tokens[i].tok == Tok::RBrace ? 1 : 0;
					}

					continue;
				}

				if (tokens[i].tok != Tok::KwFriend)
				{
					continue;
				}

				std::vector<std::size_t> friendTokens;
				for (auto j = i + 1; j < end && tokens[j].tok != Tok::Semi; ++j)
				{
					if (tokens[j].kind == TokenKind::Identifier ||
						tokens[j].tok == Tok::ColonColon || tokens[j].tok == Tok::KwClass ||
						tokens[j].tok == Tok::KwStruct || tokens[j].tok == Tok::LParen)
					{
						friendTokens.push_back(j);
					}
				}

				if (friendTokens.empty())
				{
					continue;
				}

				if (contextClass != kNone)
				{
					const auto contextName = model.Names().Text(symbols.name[contextClass]);
					for (std::size_t j = 0; j < friendTokens.size(); ++j)
					{
						const auto tokenIndex = friendTokens[j];
						if ((tokens[tokenIndex].tok == Tok::KwClass ||
							tokens[tokenIndex].tok == Tok::KwStruct) && j + 1 < friendTokens.size() &&
							tree.Text(tokens[friendTokens[j + 1]]) == contextName)
						{
							return true;
						}

						if (j + 2 < friendTokens.size() &&
							tree.Text(tokens[tokenIndex]) == contextName &&
							tokens[friendTokens[j + 1]].tok == Tok::ColonColon &&
							tree.Text(tokens[friendTokens[j + 2]]) == callerFunction)
						{
							return true;
						}
					}
				}
				else if (!callerFunction.empty())
				{
					for (std::size_t j = 0; j + 1 < friendTokens.size(); ++j)
					{
						if (tree.Text(tokens[friendTokens[j]]) == callerFunction &&
							tokens[friendTokens[j + 1]].tok == Tok::LParen)
						{
							return true;
						}
					}
				}
			}

			return false;
		}
	}

	std::vector<Diagnostic> SemanticRules::AnalyzeAccessControl(const TypeModel& types)
	{
		const auto& model   = types.Model();
		const auto& tree    = model.Tree();
		const auto& nodes   = tree.NodesSoA();
		const auto& symbols = model.Symbols();
		std::vector<Diagnostic> diagnostics;
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

			std::string_view instanceType;
			const auto objectClass = ClassOfExpression(types, children[0], instanceType);
			if (objectClass == kNone)
			{
				continue;
			}

			const auto memberToken = nodes.FirstToken(children[1]);
			if (memberToken >= tree.Tokens().size())
			{
				continue;
			}

			const auto memberName = model.Names().Find(tree.Text(tree.Tokens()[memberToken]));
			const auto member     = memberName == kNone ? kNone : model.LookupMember(objectClass, memberName);
			if (member == kNone || member >= symbols.access.size())
			{
				continue;
			}

			const auto access = symbols.access[member];
			if (access == AccessLevel::Public)
			{
				continue;
			}

			const auto memberScope = symbols.scope[member];
			const auto owner = memberScope < model.Scopes().Size()
			? model.Scopes().owner[memberScope] : kNone;
			if (owner == kNone || owner >= symbols.Size())
			{
				continue;
			}

			const auto contextClass = EnclosingClass(model, node);
			bool permitted = contextClass == owner || IsNestedWithin(model, contextClass, owner) ||
				HasFriendAccess(model, owner, contextClass, node);
			if (access == AccessLevel::Protected &&
				contextClass != kNone && DerivesFrom(model, contextClass, owner) &&
				DerivesFrom(model, objectClass, contextClass))
			{
				permitted = true;
			}

			if (permitted)
			{
				continue;
			}

			if (!linesBuilt)
			{
				lines.Build(tree.Source());
				linesBuilt = true;
			}

			const auto offset     = tree.Tokens()[memberToken].offset;
			const auto position   = lines.Lookup(offset);
			const auto memberText = tree.Text(tree.Tokens()[memberToken]);
			const auto accessName = access == AccessLevel::Private ? "private" : "protected";
			const auto ownerName = instanceType.empty()
			? model.Names().Text(symbols.name[owner]) : instanceType;
			const auto memberType = owner == kNone ? std::string {}:
			SpelledMemberType(model, owner, member, instanceType);
			const auto qualifiedName = std::string(ownerName) + "::" + std::string(memberText);
			const auto subject       = memberType.empty() ? qualifiedName : memberType + " " + qualifiedName;
			const auto message       = "'" + subject + "' is " + accessName + " within this context";
			diagnostics.push_back(Diagnostic {
					RuleId::InaccessibleMember, Severity::Error, "cpp/access-control", message,
					offset, tree.Tokens()[memberToken].length, position.line, position.column,
					false, {}
			});
		}

		return diagnostics;
	}

	std::vector<Diagnostic> SemanticRules::AnalyzeConflictingDeclarations(const TypeModel& types)
	{
		const auto& model   = types.Model();
		const auto& tree    = model.Tree();
		const auto& nodes   = tree.NodesSoA();
		const auto& symbols = model.Symbols();
		const auto& tokens  = tree.Tokens();
		const auto isAuto = [&](SymbolId symbol)
		{
			const auto declaration = symbols.decl_node[symbol];
			if (declaration >= nodes.size())
			{
				return false;
			}

			const auto begin = nodes.FirstToken(declaration);
			const auto end   = std::min<std::size_t>(begin + nodes.TokenCount(declaration), tokens.size());
			for (auto token = begin; token < end && token < symbols.decl_token[symbol]; ++token)
			{
				if (tokens[token].tok == Tok::KwAuto)
				{
					return true;
				}
			}

			return false;
		};
		const auto spelledAutoType = [&](SymbolId symbol)
		{
			const auto declaration = symbols.decl_node[symbol];
			if (declaration >= nodes.size())
			{
				return std::string {};
			}

			const auto begin     = nodes.FirstToken(declaration);
			const auto end       = std::min<std::size_t>(begin + nodes.TokenCount(declaration), tokens.size());
			const auto nameToken = symbols.decl_token[symbol];
			auto initializer     = end;
			for (auto token = nameToken + 1; token < end; ++token)
			{
				if (tokens[token].tok == Tok::Eq)
				{
					initializer = token + 1;
					break;
				}
			}

			while (initializer < end && tokens[initializer].kind == TokenKind::Whitespace)
			{
				++initializer;
			}

			if (initializer >= end || tokens[initializer].kind != TokenKind::Identifier)
			{
				return std::string {};
			}

			const auto candidateName = model.Names().Find(tree.Text(tokens[initializer]));
			const auto candidate = candidateName == kNone
			? kNone : model.Lookup(model.ScopeOfNode(declaration), candidateName, initializer);
			if (candidate != kNone && symbols.kind[candidate] == SymbolKind::Class)
			{
				auto typeEnd = initializer;
				auto next    = initializer + 1;
				while (next < end && tokens[next].kind == TokenKind::Whitespace)
				{
					++next;
				}

				if (next < end && tokens[next].tok == Tok::Lt)
				{
					int depth = 0;
					for (auto token = next; token < end; ++token)
					{
						if (tokens[token].tok == Tok::Lt)
						{
							++depth;
						}
						else if (tokens[token].tok == Tok::Gt && --depth == 0)
						{
							typeEnd = token;
							break;
						}
					}
				}

				const auto typeStart  = tokens[initializer].offset;
				const auto typeFinish = tokens[typeEnd].offset + tokens[typeEnd].length;
				return std::string(tree.Source().substr(typeStart, typeFinish - typeStart));
			}

			const auto function = model.ResolveToken(static_cast<std::uint32_t>(initializer));
			if (function == kNone || symbols.kind[function] != SymbolKind::Function)
			{
				return std::string {};
			}

			const auto functionNode = symbols.decl_node[function];
			const auto functionName = symbols.decl_token[function];
			if (functionNode >= nodes.size() || functionName >= tokens.size())
			{
				return std::string {};
			}

			auto returnBegin = nodes.FirstToken(functionNode);
			while (returnBegin < functionName && tokens[returnBegin].tok == Tok::KwTemplate)
			{
				while (returnBegin < functionName && tokens[returnBegin].tok != Tok::Gt)
				{
					++returnBegin;
				}

				++returnBegin;
			}

			if (returnBegin >= functionName)
			{
				return std::string {};
			}

			const auto typeStart = tokens[returnBegin].offset;
			const auto typeEnd   = tokens[functionName].offset;
			return std::string(Trim(tree.Source().substr(typeStart, typeEnd - typeStart)));
		};

		std::vector<Diagnostic> diagnostics;
		LineTable lines;
		bool linesBuilt = false;
		for (SymbolId current = 0; current < symbols.Size(); ++current)
		{
			if (symbols.kind[current] != SymbolKind::Variable ||!isAuto(current))
			{
				continue;
			}

			SymbolId previous = kNone;
			for (SymbolId candidate = 0; candidate < current; ++candidate)
			{
				if (symbols.kind[candidate] == SymbolKind::Variable &&
					symbols.scope[candidate] == symbols.scope[current] &&
					symbols.name[candidate] == symbols.name[current] && isAuto(candidate))
				{
					previous = candidate;
				}
			}

			if (previous == kNone)
			{
				continue;
			}

			const auto token         = symbols.decl_token[current];
			const auto previousToken = symbols.decl_token[previous];
			if (token >= tokens.size() || previousToken >= tokens.size())
			{
				continue;
			}

			if (!linesBuilt)
			{
				lines.Build(tree.Source());
				linesBuilt = true;
			}

			const auto offset       = tokens[token].offset;
			const auto position     = lines.Lookup(offset);
			const auto name         = model.Names().Text(symbols.name[current]);
			const auto previousType = spelledAutoType(previous);
			const auto relatedMessage = previousType.empty()
			? "previous declaration of '" + std::string(name) + "' is here"
			: "previous declaration as '" + previousType + " " + std::string(name) + "' is here";
			diagnostics.push_back(Diagnostic {
					RuleId::ConflictingDeclaration, Severity::Error,
					"cpp/conflicting-declaration", "conflicting declaration 'auto " + std::string(name) + "'",
					offset, tokens[token].length, position.line, position.column, false, {}, true, {},
					true, tokens[previousToken].offset, tokens[previousToken].length,
					relatedMessage
			});
		}

		return diagnostics;
	}
} // namespace heimdall
