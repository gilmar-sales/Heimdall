#include "Support.hpp"
#include <Heimdall/Refactoring.hpp>
#include <Heimdall/Doxygen.hpp>
#include <Heimdall/Navigation.hpp>

#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace heimdall
{
	namespace
	{
		RefactoringError Error(RefactoringErrorCode code, std::string message)
		{
			return {code, std::move(message)};
		}

		void AppendDocumentationEdits(
			const ParseTree& tree,
			DocumentEdits& edits,
			const std::vector<std::size_t>& declarations,
			std::string_view oldName,
			std::string_view newName,
			bool selfBrief,
			bool uniqueWorkspaceName = false,
			std::string_view qualifiedName = {})
		{
			const auto add =[&](std::size_t offset, std::size_t length)
			{
				if (std::none_of(edits.edits.begin(), edits.edits.end(),[offset](const auto& edit)
					{
						return edit.offset == offset;
				}))
				{
					edits.edits.push_back({offset, length, std::string(oldName), std::string(newName)});
				}
			};
			for (const auto& reference : DoxygenReferences(tree))
			{
				if (tree.Source().substr(reference.nameOffset, reference.nameLength) != oldName)
				{
					continue;
				}

				const auto targets = Navigation::Definition(tree, reference.offset);
				bool matches = targets.size() == 1 && targets.front().file == -1 &&
					std::find(declarations.begin(), declarations.end(), targets.front().offset) !=
					declarations.end();
				if (targets.empty() && uniqueWorkspaceName && !reference.parameter)
				{
					const auto name = reference.target.substr(0, reference.target.find('('));
					matches         = name == qualifiedName || name == oldName;
				}

				if (matches)
				{
					add(reference.nameOffset, reference.nameLength);
				}
			}

			if (!selfBrief)
			{
				return;
			}

			const auto& tokens = tree.Tokens();
			for (const auto declaration : declarations)
			{
				auto selected = std::lower_bound(tokens.begin(), tokens.end(), declaration,
					[](const Token& token, std::size_t offset)
					{
						return token.offset < offset;
				});
				while (selected != tokens.begin())
				{
					--selected;
					if (selected->tok == Tok::Semi || selected->tok == Tok::LBrace ||
						selected->tok == Tok::RBrace)
					{
						break;
					}

					const auto text = tree.Text(*selected);
					if (!(text.starts_with("/**") || text.starts_with("/*!") ||
						text.starts_with("///") || text.starts_with("//!")))
					{
						continue;
					}

					auto begin = text.find("@brief");
					if (begin == std::string_view::npos)
					{
						begin = text.find("\\brief");
					}

					if (begin != std::string_view::npos)
					{
						begin += std::string_view("@brief").size();
						const auto end = text.find_first_of("@\\", begin);
						for (auto position = text.find(oldName, begin);
							position != std::string_view::npos &&
							(end == std::string_view::npos || position < end);
							position = text.find(oldName, position + oldName.size()))
						{
							const auto word =[](char value)
							{
								return (value >= 'a' && value <= 'z') ||
									(value >= 'A' && value <= 'Z') ||
									(value >= '0' && value <= '9') || value == '_';
							};
							if ((position == 0 ||!word(text[position - 1])) &&
								(position + oldName.size() == text.size() ||
								!word(text[position + oldName.size()])))
							{
								add(selected->offset + position, oldName.size());
							}
						}
					}

					if (selected->kind == TokenKind::BlockComment)
					{
						break;
					}
				}
			}
		}

		// Helpers defined below their first use (scope walks shared by the
		// local and field paths).
		NodeId InnermostNode(const GrammarNodeSoA& soa, std::uint32_t token);

		ScopeId InnermostLambda(const SemanticModel& model, ScopeId from, ScopeId to);

		bool Identifier(std::string_view name)
		{
			// Start with portable identifiers. Reserved names are not generated.
			if (name.empty() || name.front() == '_' || name.find("__") != std::string_view::npos)
			{
				return false;
			}

			const auto alpha =[](char c)
			{
				return (c >= 'a' && c <= 'z') ||(c >= 'A' && c <= 'Z');
			};
			if (!alpha(name.front()))
			{
				return false;
			}

			for (char c : name)
			{
				if (!alpha(c) && !(c >= '0' && c <= '9') && c != '_')
				{
					return false;
				}
			}

			const auto tokens = Lexer(name).Lex();
			// Alternative operator spellings are identifier tokens in this lexer.
			constexpr std::string_view alternatives[] = {
				"and", "and_eq", "bitand", "bitor", "compl", "not",
				"not_eq", "or", "or_eq", "xor", "xor_eq"
			};
			return tokens.size() == 1 && tokens[0].kind == TokenKind::Identifier &&
				tokens[0].tok == Tok::None &&
				std::ranges::find(alternatives, name) == std::end(alternatives);
		}

		bool Within(const SemanticModel& model, ScopeId scope, ScopeId ancestor)
		{
			const auto& scopes = model.Scopes();
			for (std::size_t steps = 0; scope < scopes.Size() && steps < scopes.Size(); ++steps)
			{
				if (scope == ancestor)
				{
					return true;
				}

				if (scope == SemanticModel::TranslationUnitScope)
				{
					break;
				}

				scope = scopes.parent[scope];
			}

			return false;
		}

		bool SameDeclarationScope(const SemanticModel& model, ScopeId a, ScopeId b)
		{
			if (a == b)
			{
				return true;
			}

			const auto& scopes = model.Scopes();
			const auto& soa    = model.Tree().NodesSoA();
			const auto outermost_body =[&](ScopeId body, ScopeId owner)
			{
				if (body >= scopes.Size() || owner >= scopes.Size() ||
					scopes.kind[body] != ScopeKind::Block || scopes.parent[body] != owner)
				{
					return false;
				}

				const auto node = scopes.node[body];
				if (node >= soa.size() || soa.Kind(node) != GrammarKind::CompoundStatement ||
					scopes.node[owner] >= soa.size() || soa.Parent(node) != scopes.node[owner])
				{
					return false;
				}

				const auto kind = soa.Kind(scopes.node[owner]);
				return kind == GrammarKind::FunctionDefinition ||
					kind == GrammarKind::IfStatement || kind == GrammarKind::LoopStatement ||
					kind == GrammarKind::SwitchStatement;
			};
			// C++ forbids redeclaring a parameter/condition variable in the
			// outermost body, even though the binder gives that body a scope.
			// Two sibling bodies must remain independent from each other.
			return outermost_body(a, b) || outermost_body(b, a);
		}

		ScopeId ScopeAt(const SemanticModel& model, std::uint32_t token)
		{
			const auto& soa  = model.Tree().NodesSoA();
			NodeId innermost = 0;
			for (NodeId node = 0; node < soa.size(); ++node)
			{
				if (token >= soa.FirstToken(node) &&
					token - soa.FirstToken(node) < soa.TokenCount(node))
				{
					innermost = node;
				}
			}

			return model.ScopeOfNode(innermost);
		}

		// Token range of the function body that owns `scope`. A function-local
		// rename can only be captured by uses inside that same function, so the
		// collision scan stays proportional to the function instead of the file.
		bool FunctionTokenRange(
			const SemanticModel& model,
			ScopeId scope,
			std::uint32_t& begin,
			std::uint32_t& end)
		{
			const auto& scopes = model.Scopes();
			ScopeId function   = scope;
			while (function < scopes.Size() && scopes.kind[function] == ScopeKind::Block)
			{
				function = scopes.parent[function];
			}

			if (function >= scopes.Size() || scopes.kind[function] != ScopeKind::Function)
			{
				return false;
			}

			const auto& soa = model.Tree().NodesSoA();
			const auto node = scopes.node[function];
			if (node >= soa.size())
			{
				return false;
			}

			begin = soa.FirstToken(node);
			end   = begin + soa.TokenCount(node);
			return true;
		}

		std::expected<void, RefactoringError> CheckNameScopes(
			const SemanticModel& model,
			SymbolId target,
			std::string_view name,
			std::stop_token stop)
		{
			const auto& symbols = model.Symbols();
			const auto scope    = symbols.scope[target];
			for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
			{
				if (stop.stop_requested())
				{
					return std::unexpected(
						Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
				}

				if (symbol != target && model.Names().Text(symbols.name[symbol]) == name &&
					SameDeclarationScope(model, scope, symbols.scope[symbol]))
				{
					return std::unexpected(
						Error(RefactoringErrorCode::NameCollision,
						"The new name is already declared in the same scope"));
				}
			}

			const auto& tree        = model.Tree();
			const auto& tokens      = tree.Tokens();
			const auto& significant = model.Significant();

			// Uses outside the renamed function's token range resolve in scopes
			// that can never sit inside the target's scope, so they cannot be
			// captured by the new name. Scanning only the function turns this
			// check from O(file) into O(function).
			std::uint32_t range_begin = symbols.decl_token[target] + 1;
			std::uint32_t range_end   = static_cast<std::uint32_t>(tokens.size());
			{
				std::uint32_t function_begin = 0;
				std::uint32_t function_end   = range_end;
				if (FunctionTokenRange(model, scope, function_begin, function_end))
				{
					range_begin = std::max(range_begin, function_begin);
					range_end   = std::min(range_end, function_end);
				}
			}

			// Declaration membership was a linear find per token, i.e.
			// O(tokens x symbols). A set built once makes it O(1) per token.
			const std::unordered_set<std::uint32_t> declarations(
				symbols.decl_token.begin(), symbols.decl_token.end());
			// Intern the new name once instead of hashing it per candidate.
			const auto wanted = model.Names().Find(name);

			for (std::uint32_t token = range_begin; token < range_end; ++token)
			{
				if (stop.stop_requested())
				{
					return std::unexpected(
						Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
				}

				if (!model.IsCode(token) || tokens[token].kind != TokenKind::Identifier ||
					tree.Text(tokens[token]) != name ||
					declarations.find(token) != declarations.end())
				{
					continue;
				}

				const auto use_scope = ScopeAt(model, token);
				// Captures are not modeled: a use inside a lambda may bind
				// differently after the rename, unless it already binds to a
				// lambda parameter or lambda-local (shadowing is stable).
				if (const auto lambda = InnermostLambda(model, use_scope, scope);
					lambda != kNone)
				{
					const auto bound = model.Lookup(use_scope, wanted, token);
					if (bound == kNone ||!Within(model, symbols.scope[bound], lambda))
					{
						return std::unexpected(
							Error(RefactoringErrorCode::IncompleteAnalysis,
							"An occurrence of the new name in an affected scope is not "
							"modeled"));
					}
				}

				if (std::binary_search(model.Refs().token.begin(), model.Refs().token.end(), token))
				{
					continue;
				}

				const auto at = std::lower_bound(significant.begin(), significant.end(), token);
				if (at != significant.begin())
				{
					const auto previous = tokens[*(at - 1)].tok;
					if (previous == Tok::Dot || previous == Tok::Arrow ||
						previous == Tok::ColonColon)
					{
						continue;
					}
				}

				if (!Within(model, use_scope, scope))
				{
					continue;
				}

				const auto existing = model.Lookup(use_scope, wanted, token);
				if (existing != kNone && Within(model, symbols.scope[existing], scope))
				{
					continue;
				}

				// Rebind cannot certify an occurrence omitted by the binder.
				// Block only when this declaration could actually capture it.
				return std::unexpected(
					Error(RefactoringErrorCode::IncompleteAnalysis,
					"An occurrence of the new name in an affected scope is not modeled"));
			}

			return {};
		}

		NodeId InnermostNode(const GrammarNodeSoA& soa, std::uint32_t token)
		{
			// Smallest covering range wins: node indices are creation order,
			// not nesting depth (a binary operator node is created after its
			// left operand), so the highest index is not always the innermost.
			NodeId innermost       = 0;
			std::uint32_t tightest = soa.size() != 0 ? soa.TokenCount(0) + 1 : 0;
			for (NodeId node = 0; node < soa.size(); ++node)
			{
				if (token >= soa.FirstToken(node) &&
					token - soa.FirstToken(node) < soa.TokenCount(node) &&
					(soa.TokenCount(node) < tightest ||
					(soa.TokenCount(node) == tightest && node > innermost)))
				{
					innermost = node;
					tightest  = soa.TokenCount(node);
				}
			}

			return innermost;
		}

		bool IsFieldTarget(const SemanticModel& model, SymbolId symbol)
		{
			const auto& symbols = model.Symbols();
			const auto& scopes  = model.Scopes();
			return symbol < symbols.Size() && symbols.kind[symbol] == SymbolKind::Variable &&
				symbols.scope[symbol] < scopes.Size() &&
				scopes.kind[symbols.scope[symbol]] == ScopeKind::Class;
		}

		// Whether any identifier token in the node's range spells `name`. Used
		// to tell apart unmodeled constructs that can interact with a rename
		// from unrelated ones sitting nearby.
		bool MentionsName(const ParseTree& tree, NodeId node, std::string_view name)
		{
			const auto& soa    = tree.NodesSoA();
			const auto& tokens = tree.Tokens();
			if (node >= soa.size())
			{
				return false;
			}

			const auto begin = soa.FirstToken(node);
			const auto end =
				std::min<std::uint32_t>(begin + soa.TokenCount(node),
				static_cast<std::uint32_t>(tokens.size()));
			for (auto i = begin; i < end; ++i)
			{
				if (tokens[i].kind == TokenKind::Identifier && tokens[i].tok == Tok::None &&
					tree.Text(tokens[i]) == name)
				{
					return true;
				}
			}

			return false;
		}

		// Whether the lambda's capture clause (`[...]`, before the parameter
		// list) mentions `name`. Explicit captures bind the name without
		// declaring it, so they veto; implicit (`[]`, `[=]`, `[&]`) captures
		// and parameter/body mentions are attributed by the reference scan.
		bool CaptureMentions(const ParseTree& tree, NodeId lambda, std::string_view name)
		{
			const auto& soa    = tree.NodesSoA();
			const auto& tokens = tree.Tokens();
			if (lambda >= soa.size() || soa.Kind(lambda) != GrammarKind::LambdaExpression)
			{
				return false;
			}

			auto i = soa.FirstToken(lambda);
			if (i >= tokens.size() || tokens[i].tok != Tok::LBracket)
			{
				return false;
			}

			std::size_t depth = 0;
			for (; i < tokens.size(); ++i)
			{
				if (tokens[i].tok == Tok::LBracket)
				{
					++depth;
				}
				else if (tokens[i].tok == Tok::RBracket)
				{
					if (--depth == 0)
					{
						break;
					}
				}
				else if (depth == 1 && tokens[i].kind == TokenKind::Identifier &&
					tokens[i].tok == Tok::None && tree.Text(tokens[i]) == name)
				{
					return true;
				}
			}

			return false;
		}

		// Innermost lambda-owned function scope on the path from `from` up to
		// (excluding) `to`, or kNone. Captures are not modeled, so a use that
		// does not already bind inside the lambda may bind differently after
		// a rename; uses bound to lambda parameters or lambda-locals are
		// shadowed and safe.
		ScopeId InnermostLambda(const SemanticModel& model, ScopeId from, ScopeId to)
		{
			const auto& scopes = model.Scopes();
			const auto& soa    = model.Tree().NodesSoA();
			ScopeId found      = kNone;
			for (auto scope = from; scope < scopes.Size() && scope != to;
				scope = scopes.parent[scope])
			{
				if (scopes.kind[scope] == ScopeKind::Function &&
					scopes.node[scope] < soa.size() &&
					soa.Kind(scopes.node[scope]) == GrammarKind::LambdaExpression)
				{
					// Walked inside-out: the first hit is the innermost.
					return scope;
				}

				if (scope == SemanticModel::TranslationUnitScope)
				{
					break;
				}
			}

			return found;
		}

		// Member name token -> resolved field (kNone when unresolvable) for
		// every member access in the unit. Shared by reference collection and
		// new-name capture analysis so both see the same attribution.
		std::expected<std::unordered_map<std::uint32_t, SymbolId>, RefactoringError>
		BuildMemberUses(const SemanticModel& model, const TypeModel& types, std::stop_token stop)
		{
			std::unordered_map<std::uint32_t, SymbolId> member_use;
			const auto& soa = model.Tree().NodesSoA();
			for (NodeId node = 0; node < soa.size(); ++node)
			{
				if (stop.stop_requested())
				{
					return std::unexpected(
						Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
				}

				if (soa.Kind(node) != GrammarKind::MemberExpression)
				{
					continue;
				}

				const auto kids = model.ChildrenOf(node);
				if (kids.size() != 2)
				{
					continue;
				}

				member_use.emplace(soa.FirstToken(kids[1]), types.MemberSymbol(node));
			}

			return member_use;
		}

		// Occurrences of a class field: its declaration plus every member access
		// that the typer resolved to it, in this translation unit. Any use the
		// model cannot attribute vetoes the rename instead of being skipped.
		std::expected<SymbolOccurrences, RefactoringError> ReferencesField(
			const AnalysisSnapshot& snapshot,
			DocumentId document,
			const ParseTree& tree,
			const SemanticModel& model,
			const std::shared_ptr<const TypeModel>& types,
			SymbolId target,
			std::uint32_t selected,
			bool include_declaration,
			std::stop_token stop)
		{
			const auto& symbols = model.Symbols();
			const auto& scopes  = model.Scopes();
			const auto& tokens  = tree.Tokens();
			const auto& soa     = tree.NodesSoA();

			const ScopeId class_scope = symbols.scope[target];
			const SymbolId owner =
				class_scope < scopes.Size() ? scopes.owner[class_scope] : kNone;
			const NodeId class_node =
				class_scope < scopes.Size() ? scopes.node[class_scope] : 0;

			// Template-owned fields have dependent bindings.
			if (class_node < soa.size())
			{
				for (auto ancestor = class_node; ancestor < soa.size();
					ancestor = soa.Parent(ancestor))
				{
					if (stop.stop_requested())
					{
						return std::unexpected(
							Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
					}

					if (soa.Kind(ancestor) == GrammarKind::TemplateDeclaration)
					{
						return std::unexpected(
							Error(RefactoringErrorCode::IncompleteAnalysis,
							"Template-dependent bindings are not supported"));
					}

					if (ancestor == 0 || soa.Parent(ancestor) == ancestor)
					{
						break;
					}
				}
			}

			SymbolOccurrences result {target,
				tokens[selected].offset,
				tokens[selected].length,
				std::string(model.Names().Text(symbols.name[target])), {}};
			if (include_declaration)
			{
				result.tokens.push_back(symbols.decl_token[target]);
			}

			// Member name token -> resolved field (kNone when unresolvable) for
			// every member access in the unit.
			auto member_use = BuildMemberUses(model, *types, stop);
			if (!member_use)
			{
				return std::unexpected(member_use.error());
			}

			const std::unordered_set<std::uint32_t> declarations(
				symbols.decl_token.begin(), symbols.decl_token.end());

			// Functions containing assembly or coroutines veto occurrences
			// inside them; uses elsewhere are unaffected.
			std::vector<std::pair<std::uint32_t, std::uint32_t>> tainted;
			for (std::uint32_t i = 0; i < tokens.size(); ++i)
			{
				if (stop.stop_requested())
				{
					return std::unexpected(
						Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
				}

				if (tokens[i].tok != Tok::KwAsm && tokens[i].tok != Tok::KwCoAwait &&
					tokens[i].tok != Tok::KwCoYield && tokens[i].tok != Tok::KwCoReturn)
				{
					continue;
				}

				auto enclosing = InnermostNode(soa, i);
				while (enclosing < soa.size())
				{
					if (soa.Kind(enclosing) == GrammarKind::FunctionDefinition)
					{
						const auto begin = soa.FirstToken(enclosing);
						tainted.emplace_back(begin, begin + soa.TokenCount(enclosing));
						break;
					}

					if (enclosing == 0 || soa.Parent(enclosing) == enclosing)
					{
						break;
					}

					enclosing = soa.Parent(enclosing);
				}
			}

			for (std::uint32_t i = 0; i < tokens.size(); ++i)
			{
				if (stop.stop_requested())
				{
					return std::unexpected(
						Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
				}

				if (tokens[i].kind != TokenKind::Identifier || tokens[i].tok != Tok::None ||
					tree.Text(tokens[i]) != result.name || i == symbols.decl_token[target])
				{
					continue;
				}

				const SymbolId resolved   = model.ResolveToken(i);
				const auto found          = member_use->find(i);
				const SymbolId via_member = found == member_use->end() ? kNone : found->second;
				if (resolved == target || via_member == target)
				{
					for (const auto& [begin, end] : tainted)
					{
						if (i >= begin && i < end)
						{
							return std::unexpected(
								Error(RefactoringErrorCode::IncompleteAnalysis,
								"Assembly and coroutines are not supported"));
						}
					}

					result.tokens.push_back(i);
				}
				else if (resolved != kNone || via_member != kNone ||
					declarations.find(i) != declarations.end())
				{
					continue;
				}
				else
				{
					return std::unexpected(
						Error(RefactoringErrorCode::IncompleteAnalysis,
						"An occurrence of this name is not resolved"));
				}
			}

			std::ranges::sort(result.tokens);
			if (class_node < soa.size())
			{
				const auto first = soa.FirstToken(class_node);
				const auto last  = first + soa.TokenCount(class_node);
				if (last <= tokens.size() && last > first)
				{
					const auto class_start = tokens[first].offset;
					const auto class_end   = tokens[last - 1].offset + tokens[last - 1].length;
					for (const auto& directive : tree.Directives())
					{
						if (directive.kind == DirectiveKind::Include &&
							directive.offset >= class_start && directive.offset < class_end)
						{
							return std::unexpected(
								Error(RefactoringErrorCode::MacroContext,
								"Includes inside a class are not supported for rename"));
						}
					}
				}
			}

			const auto source = snapshot.Source(document);
			if (!source)
			{
				return std::unexpected(Error(RefactoringErrorCode::IncompleteAnalysis,
					"Document is not available"));
			}

			if (!tree.Directives().empty() ||!snapshot.Options(document).Macros().empty())
			{
				const auto verified =
					refactor_detail::VerifyPreprocessing(snapshot, document, *source, stop);
				if (!verified)
				{
					return std::unexpected(verified.error());
				}
			}

			return result;
		}

		std::expected<void, RefactoringError> CheckFieldName(
			const SemanticModel& model,
			const ParseTree& tree,
			const TypeModel& types,
			SymbolId target,
			std::string_view name,
			std::stop_token stop)
		{
			const auto& symbols       = model.Symbols();
			const auto& scopes        = model.Scopes();
			const ScopeId class_scope = symbols.scope[target];
			const SymbolId owner =
				class_scope < scopes.Size() ? scopes.owner[class_scope] : kNone;
			const auto wanted = model.Names().Find(name);
			if (wanted == kNone)
			{
				return {};
			}

			if (owner != kNone)
			{
				// The class or a base already declares the name; any kind collides.
				if (model.LookupMember(owner, wanted) != kNone)
				{
					return std::unexpected(
						Error(RefactoringErrorCode::NameCollision,
						"The class already declares the new name"));
				}
			}
			else
			{
				// Anonymous owner: same-scope scan instead of member lookup.
				for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
				{
					if (stop.stop_requested())
					{
						return std::unexpected(
							Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
					}

					if (symbol != target && symbols.name[symbol] == wanted &&
						symbols.scope[symbol] == class_scope)
					{
						return std::unexpected(
							Error(RefactoringErrorCode::NameCollision,
							"The class already declares the new name"));
					}
				}
			}

			// Bare uses of the new name inside the class subtree would be
			// captured by the renamed field unless they already resolve inside
			// it. Captures and unattributed uses cannot be proven safe.
			// (Locals that shadow safely are decided per use below; no symbol
			// pre-scan is needed.)
			auto member_use = BuildMemberUses(model, types, stop);
			if (!member_use)
			{
				return std::unexpected(member_use.error());
			}

			const std::unordered_set<std::uint32_t> declarations(
				symbols.decl_token.begin(), symbols.decl_token.end());
			const auto& tokens = tree.Tokens();
			for (std::uint32_t token = 0; token < tokens.size(); ++token)
			{
				if (stop.stop_requested())
				{
					return std::unexpected(
						Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
				}

				if (tokens[token].kind != TokenKind::Identifier ||
					tokens[token].tok != Tok::None || tree.Text(tokens[token]) != name ||
					declarations.find(token) != declarations.end())
				{
					continue;
				}

				const auto found = member_use->find(token);
				if (found != member_use->end() && found->second != kNone)
				{
					// Qualified use attributed to another member; positional.
					continue;
				}

				if (found != member_use->end())
				{
					// Unresolvable member access could bind to the field.
					return std::unexpected(
						Error(RefactoringErrorCode::IncompleteAnalysis,
						"An occurrence of the new name in an affected scope is not modeled"));
				}

				const auto use_scope = ScopeAt(model, token);
				if (!Within(model, use_scope, class_scope))
				{
					continue;
				}

				// Captures are not modeled: veto unless the use already binds
				// inside the lambda itself (parameter or lambda-local).
				if (const auto lambda = InnermostLambda(model, use_scope, class_scope);
					lambda != kNone)
				{
					const auto bound = model.Lookup(use_scope, wanted, token);
					if (bound == kNone ||!Within(model, symbols.scope[bound], lambda))
					{
						return std::unexpected(
							Error(RefactoringErrorCode::IncompleteAnalysis,
							"An occurrence of the new name in an affected scope is not "
							"modeled"));
					}

					continue;
				}

				const auto existing = model.Lookup(use_scope, wanted, token);
				if (existing == kNone ||!Within(model, symbols.scope[existing], class_scope))
				{
					return std::unexpected(
						Error(RefactoringErrorCode::IncompleteAnalysis,
						"An occurrence of the new name in an affected scope is not modeled"));
				}
			}

			return {};
		}

		// Edit hull for incremental revalidation: the span covering every edit,
		// in original coordinates. Reuse is best-effort; unprovable items are
		// parsed normally, so the result never changes.
		ParseReuse MakeReuse(
			const ParseTree* previous,
			const std::vector<SourceEdit>& edits,
			std::size_t before_size,
			std::size_t after_size)
		{
			std::size_t old_start = edits.front().offset;
			std::size_t old_end   = old_start;
			for (const auto& edit : edits)
			{
				old_start = std::min(old_start, edit.offset);
				old_end   = std::max(old_end, edit.offset + edit.length);
			}

			ParseReuse reuse;
			reuse.previous   = previous;
			reuse.offset     = old_start;
			reuse.old_length = old_end - old_start;
			reuse.new_length = after_size - before_size + reuse.old_length;
			return reuse;
		}

		// Certifies that a rename changed nothing but the spelling of the
		// edited declarations: token/symbol/ref/member identity is exact.
		// `edited` holds the source offsets of the applied edits; symbols
		// declared at those offsets must spell `new_name`, all others must be
		// byte-identical. `types` may be null to skip member checking.
		std::expected<void, RefactoringError> CheckRenamedModel(
			const ParseTree& tree,
			const SemanticModel& model,
			const TypeModel* types,
			const ParseTree& after_tree,
			const SemanticModel& after,
			const TypeModel* after_types,
			const std::unordered_set<std::size_t>& edited,
			std::string_view new_name)
		{
			const auto& before_symbols = model.Symbols();
			const auto& after_symbols  = after.Symbols();
			if (after_tree.Tokens().size() != tree.Tokens().size() ||
				after_symbols.Size() != before_symbols.Size() ||
				after.Refs().token != model.Refs().token)
			{
				return std::unexpected(Error(RefactoringErrorCode::SemanticChange,
					"Rename changes symbol bindings"));
			}

			if (after.Refs().target != model.Refs().target)
			{
				return std::unexpected(
					Error(RefactoringErrorCode::NameCollision,
					"The new name changes lookup in an affected scope"));
			}

			for (SymbolId i = 0; i < before_symbols.Size(); ++i)
			{
				const auto at_renamed =
					edited.find(tree.Tokens()[before_symbols.decl_token[i]].offset) !=
					edited.end();
				const auto expected = at_renamed
				? new_name
				: model.Names().Text(before_symbols.name[i]);
				if (after.Names().Text(after_symbols.name[i]) != expected ||
					before_symbols.decl_token[i] != after_symbols.decl_token[i] ||
					before_symbols.scope[i] != after_symbols.scope[i] ||
					before_symbols.kind[i] != after_symbols.kind[i])
				{
					return std::unexpected(Error(RefactoringErrorCode::SemanticChange,
						"Rename changes declarations"));
				}
			}

			if (types != nullptr && after_types != nullptr)
			{
				if (after_tree.NodesSoA().size() != tree.NodesSoA().size())
				{
					return std::unexpected(Error(RefactoringErrorCode::SemanticChange,
						"Rename changes member bindings"));
				}

				for (NodeId n = 0; n < tree.NodesSoA().size(); ++n)
				{
					if (types->MemberSymbol(n) != after_types->MemberSymbol(n))
					{
						return std::unexpected(Error(RefactoringErrorCode::SemanticChange,
							"Rename changes member bindings"));
					}
				}
			}

			return {};
		}

		std::expected<SymbolOccurrences, RefactoringError> References(
			const AnalysisSnapshot& snapshot,
			DocumentId document,
			std::size_t offset,
			bool include_declaration,
			std::stop_token stop)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
			}

			const auto source = snapshot.Source(document);
			const auto tree   = snapshot.Syntax(document);
			if (!source ||!tree || offset >= source->size())
			{
				return std::unexpected(
					Error(RefactoringErrorCode::InvalidSelection, "Select an identifier"));
			}

			if (!tree->Diagnostics().empty())
			{
				return std::unexpected(
					Error(RefactoringErrorCode::IncompleteAnalysis, "Document has parse errors"));
			}

			const auto model       = snapshot.Semantic(document);
			const auto& symbols    = model->Symbols();
			const auto& tokens     = tree->Tokens();
			std::uint32_t selected = refactor_detail::FindSelectedToken(tokens, offset);

			SymbolId target = selected == kNone ? kNone : model->ResolveToken(selected);
			if (target == kNone)
			{
				for (SymbolId i = 0; i < symbols.Size(); ++i)
					if (symbols.decl_token[i] == selected)
				{
					target = i;
					break;
				}
			}

			// Member uses (`s.m`) resolve to kNone in the binder; resolve them
			// through the typer instead so rename can start from a use.
			if (target == kNone && selected != kNone)
			{
				if (const auto field_types = snapshot.Types(document))
				{
					const auto& fsoa = tree->NodesSoA();
					for (NodeId n = 0; n < fsoa.size(); ++n)
					{
						if (stop.stop_requested())
						{
							return std::unexpected(
								Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
						}

						if (fsoa.Kind(n) != GrammarKind::MemberExpression)
						{
							continue;
						}

						const auto kids = model->ChildrenOf(n);
						if (kids.size() != 2)
						{
							continue;
						}

						if (fsoa.FirstToken(kids[1]) == selected)
						{
							target = field_types->MemberSymbol(n);
							break;
						}
					}
				}
			}

			if (target != kNone && symbols.kind[target] == SymbolKind::Class)
			{
				if (!tree->Directives().empty() ||
					(symbols.flags[target] & SymbolFlag::Template) != 0)
				{
					return std::unexpected(Error(RefactoringErrorCode::IncompleteAnalysis,
						"Class rename with includes or templates requires project-wide coverage"));
				}

				for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
				{
					if (symbols.scope[symbol] == symbols.member_scope[target] &&
						(symbols.flags[symbol] & (SymbolFlag::Constructor | SymbolFlag::Destructor)) != 0)
					{
						return std::unexpected(Error(RefactoringErrorCode::IncompleteAnalysis,
							"Class rename with constructors or destructors is not modeled yet"));
					}
				}

				SymbolOccurrences result {target, tokens[selected].offset, tokens[selected].length,
					std::string(model->Names().Text(symbols.name[target])), {}};
				for (const auto token : model->Significant())
				{
					if (tree->Text(tokens[token]) != result.name)
					{
						continue;
					}

					const auto destinations = Navigation::Definition(*tree, tokens[token].offset);
					if (destinations.size() != 1 || destinations.front().offset !=
						tokens[symbols.decl_token[target]].offset)
					{
						return std::unexpected(Error(RefactoringErrorCode::IncompleteAnalysis,
							"A same-named class occurrence is ambiguous or unresolved"));
					}

					if (include_declaration || token != symbols.decl_token[target])
					{
						result.tokens.push_back(token);
					}
				}

				return result;
			}

			if (target == kNone ||(symbols.kind[target] != SymbolKind::Variable &&
				symbols.kind[target] != SymbolKind::Parameter))
			{
				return std::unexpected(
					Error(RefactoringErrorCode::UnsupportedSymbol,
					"Only variables, parameters and class fields are supported"));
			}

			// Two declarations of the same name in one scope (e.g. identical
			// catch parameter names in sibling handlers) resolve
			// first-declared-wins; refuse rather than rename the wrong uses.
			for (SymbolId other = 0; other < symbols.Size(); ++other)
			{
				if (stop.stop_requested())
				{
					return std::unexpected(
						Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
				}

				if (other != target && symbols.name[other] == symbols.name[target] &&
					symbols.scope[other] == symbols.scope[target])
				{
					return std::unexpected(
						Error(RefactoringErrorCode::IncompleteAnalysis,
						"An ambiguous declaration of this name is not modeled"));
				}
			}

			const auto& scopes = model->Scopes();
			if (IsFieldTarget(*model, target))
			{
				auto field_types = snapshot.Types(document);
				if (!field_types)
				{
					return std::unexpected(Error(RefactoringErrorCode::IncompleteAnalysis,
						"Document types are not available"));
				}

				return ReferencesField(snapshot, document, *tree, *model, field_types, target,
					selected, include_declaration, stop);
			}

			ScopeId function = symbols.scope[target];
			while (function < scopes.Size() && scopes.kind[function] == ScopeKind::Block)
			{
				function = scopes.parent[function];
			}

			if (function >= scopes.Size() || scopes.kind[function] != ScopeKind::Function)
			{
				return std::unexpected(
					Error(RefactoringErrorCode::UnsupportedSymbol, "Symbol is not local"));
			}

			const auto& soa = tree->NodesSoA();
			const auto node = scopes.node[function];
			// The scope that owns the target: unmodeled constructs (lambdas,
			// templates, assembly, coroutines) can only affect the rename
			// through uses inside this subtree. Function-scoped targets keep
			// the whole-function scan; block locals get a narrower one.
			const auto scope_node = symbols.scope[target] < scopes.Size()
			? scopes.node[symbols.scope[target]]
			: node;
			const auto scan_node =
				scope_node < soa.size() ? scope_node : node;
			const auto scan_begin = soa.FirstToken(scan_node);
			const auto scan_end   = scan_begin + soa.TokenCount(scan_node);
			// Lambdas, templates and requires-expressions have uses the binder
			// does not model, but only when they can interact. A lambda vetoes
			// on an explicit capture of the name; implicit captures and
			// parameter/body mentions are attributed by the reference scan
			// (lambda parameters are declared). Templates and
			// requires-expressions keep the mention veto: their parameters
			// are still unmodeled.
			// (The template-ancestor walk below still guards dependent contexts.)
			const std::string_view old_name = model->Names().Text(symbols.name[target]);
			for (auto i = scan_node; i < soa.SubtreeEnd(scan_node); ++i)
			{
				const auto kind = soa.Kind(i);
				if (kind == GrammarKind::LambdaExpression)
				{
					if (CaptureMentions(*tree, i, old_name))
					{
						return std::unexpected(
							Error(RefactoringErrorCode::IncompleteAnalysis,
							"Lambda captures are not modeled"));
					}
				}
				else if ((kind == GrammarKind::TemplateDeclaration ||
					kind == GrammarKind::RequiresExpression) &&
					MentionsName(*tree, i, old_name))
				{
					return std::unexpected(
						Error(RefactoringErrorCode::IncompleteAnalysis,
						"Lambda, template or requires-expression coverage is incomplete"));
				}
			}

			for (auto ancestor = node; ancestor < soa.size(); ancestor = soa.Parent(ancestor))
			{
				if (soa.Kind(ancestor) == GrammarKind::TemplateDeclaration)
				{
					return std::unexpected(Error(RefactoringErrorCode::IncompleteAnalysis,
						"Template-dependent bindings are not supported"));
				}

				if (ancestor == 0 || soa.Parent(ancestor) == ancestor)
				{
					break;
				}
			}

			const auto begin          = soa.FirstToken(node);
			const auto end            = begin + soa.TokenCount(node);
			const auto function_start = tokens[begin].offset;
			const auto function_end   = tokens[end - 1].offset + tokens[end - 1].length;
			for (const auto& directive : tree->Directives())
			{
				if (directive.kind == DirectiveKind::Include &&
					directive.offset >= function_start && directive.offset < function_end)
				{
					return std::unexpected(
						Error(RefactoringErrorCode::MacroContext,
						"Includes inside a function are not supported for rename"));
				}
			}

			SymbolOccurrences result {target,
				tokens[selected].offset,
				tokens[selected].length,
				std::string(model->Names().Text(symbols.name[target])), {}};
			if (include_declaration)
			{
				result.tokens.push_back(symbols.decl_token[target]);
			}

			for (std::uint32_t i = begin; i < end && i < tokens.size(); ++i)
			{
				if (stop.stop_requested())
				{
					return std::unexpected(
						Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
				}

				if (i >= scan_begin && i < scan_end &&
					(tokens[i].tok == Tok::KwAsm || tokens[i].tok == Tok::KwCoAwait ||
					tokens[i].tok == Tok::KwCoYield || tokens[i].tok == Tok::KwCoReturn))
				{
					return std::unexpected(Error(RefactoringErrorCode::IncompleteAnalysis,
						"Assembly and coroutines are not supported"));
				}

				if (tokens[i].kind != TokenKind::Identifier || tokens[i].tok != Tok::None ||
					tree->Text(tokens[i]) != result.name || i == symbols.decl_token[target])
				{
					continue;
				}

				const auto resolved = model->ResolveToken(i);
				if (resolved == target)
				{
					result.tokens.push_back(i);
				}
				else if (resolved == kNone)
				{
					const bool declaration =
						std::ranges::find(symbols.decl_token, i) != symbols.decl_token.end();
					if (!declaration)
					{
						return std::unexpected(Error(RefactoringErrorCode::IncompleteAnalysis,
							"An occurrence of this name is not resolved"));
					}
				}
			}

			std::ranges::sort(result.tokens);
			if (!tree->Directives().empty() ||!snapshot.Options(document).Macros().empty())
			{
				const auto verified =
					refactor_detail::VerifyPreprocessing(snapshot, document, *source, stop);
				if (!verified)
				{
					return std::unexpected(verified.error());
				}
			}

			return result;
		}
	} // namespace

	// Shared rename/signature helpers (declared in refactor/src/Support.hpp).
	// Token containing `offset` (binary search; tokens are offset-ordered),
	// or kNone. Shared by the local, field and workspace paths.
	std::uint32_t refactor_detail::FindSelectedToken(const std::vector<Token>& tokens,
		std::size_t offset)
	{
		const auto matches =[&](const Token& token)
		{
			return token.kind == TokenKind::Identifier && token.tok == Tok::None;
		};
		const auto begin = tokens.begin(), end = tokens.end();
		const auto after = std::upper_bound(
			begin, end, offset,
			[](std::size_t value, const Token& token)
			{
				return value < token.offset;
		});
		if (after != begin)
		{
			const auto candidate = std::prev(after);
			if (candidate->offset <= offset &&
				offset - candidate->offset < candidate->length && matches(*candidate))
			{
				return static_cast<std::uint32_t>(candidate - begin);
			}
		}

		// Defensive fallback: identical to the old linear scan.
		for (std::uint32_t i = 0; i < tokens.size(); ++i)
		{
			if (offset >= tokens[i].offset && offset - tokens[i].offset < tokens[i].length &&
				matches(tokens[i]))
			{
				return i;
			}
		}

		return kNone;
	}

	// Cross-file identity resolution for the workspace paths is declared in
	// refactor/src/Support.hpp; the definition below shares it with the
	// signature operations. Only index-supported entities with complete
	// coverage and fully resolved occurrences resolve; overloads, members,
	// templates and partial models stay honestly unsupported.
	// Shared identity resolution for the workspace paths: the cursor must
	// land on a supported free function or global with complete coverage.
	std::expected<refactor_detail::WorkspaceEntity, RefactoringError>
	refactor_detail::ResolveWorkspaceEntity(
		const AnalysisSnapshot& snapshot,
		DocumentId document,
		std::size_t offset,
		std::stop_token stop)
	{
		const auto source = snapshot.Source(document);
		const auto tree   = snapshot.Syntax(document);
		if (!source ||!tree || offset >= source->size())
		{
			return std::unexpected(
				RefactoringError {RefactoringErrorCode::InvalidSelection, "Select an identifier"});
		}

		if (!tree->Diagnostics().empty())
		{
			return std::unexpected(RefactoringError {RefactoringErrorCode::IncompleteAnalysis,
					"Document has parse errors"});
		}

		if (refactor_detail::FindSelectedToken(tree->Tokens(), offset) == kNone)
		{
			return std::unexpected(
				RefactoringError {RefactoringErrorCode::InvalidSelection, "Select an identifier"});
		}

		auto index_result = snapshot.SymbolIndex(stop);
		if (!index_result)
		{
			return std::unexpected(
				index_result.error() == SymbolIndexError::Cancelled
				? RefactoringError {RefactoringErrorCode::Cancelled, "Request cancelled"}
				: RefactoringError
				{
					RefactoringErrorCode::IncompleteAnalysis, "The workspace symbol index is not available"
			});
		}

		const auto& index                   = *index_result;
		const IndexedOccurrence* occurrence = index->At(document, offset);
		if (occurrence == nullptr || occurrence->entity == InvalidEntity ||
			occurrence->entity >= index->Entities().size())
		{
			return std::unexpected(
				RefactoringError {RefactoringErrorCode::UnsupportedSymbol,
					"Only free functions and global variables are supported across files"});
		}

		const auto& entity       = index->Entities()[occurrence->entity];
		const EntityId entity_id = occurrence->entity;
		if ((entity.kind != SymbolKind::Function && entity.kind != SymbolKind::Variable) ||
			entity.linkage == EntityLinkage::Local)
		{
			return std::unexpected(
				RefactoringError {RefactoringErrorCode::UnsupportedSymbol,
					"Only free functions and global variables are supported across files"});
		}

		// Member functions need member identity (overload sets, overrides);
		// a class-qualified function is not a free function.
		if (entity.kind == SymbolKind::Function)
		{
			const auto separator = entity.qualified_name.rfind("::");
			if (separator != std::string::npos)
			{
				bool namespace_owner = false;
				bool class_owner     = false;
				for (const auto owner :
					index->Named(entity.qualified_name.substr(0, separator)))
				{
					if (owner >= index->Entities().size())
					{
						continue;
					}

					const auto owner_kind = index->Entities()[owner].kind;
					namespace_owner       = namespace_owner || owner_kind == SymbolKind::Namespace;
					class_owner           = class_owner || owner_kind == SymbolKind::Class;
				}

				if (class_owner)
				{
					return std::unexpected(
						RefactoringError {RefactoringErrorCode::UnsupportedSymbol,
							"Member functions are not supported across files yet"});
				}

				if (!namespace_owner)
				{
					return std::unexpected(
						RefactoringError {RefactoringErrorCode::IncompleteAnalysis,
							"The owner of this symbol is not indexed"});
				}
			}
		}

		if (!entity.supported)
		{
			return std::unexpected(RefactoringError {RefactoringErrorCode::IncompleteAnalysis,
					"This symbol cannot be renamed across files yet"});
		}

		if (!index->HasCompleteCoverageFor(entity_id))
		{
			return std::unexpected(RefactoringError {RefactoringErrorCode::IncompleteAnalysis,
					"Workspace coverage is incomplete for this symbol"});
		}

		return WorkspaceEntity {*index_result, entity_id};
	}

	std::expected<SymbolOccurrences, RefactoringError> RefactoringService::LocalReferences(
		const AnalysisSnapshot& snapshot,
		DocumentId document,
		std::size_t offset,
		bool include_declaration,
		std::stop_token stop)
	{
		return References(snapshot, document, offset, include_declaration, stop);
	}

	bool refactor_detail::ValidName(std::string_view name)
	{
		return Identifier(name);
	}

	std::expected<RefactoringPlan, RefactoringError> RenameWorkspace(
		const AnalysisSnapshot& snapshot,
		DocumentId document,
		std::size_t offset,
		std::string_view new_name,
		std::stop_token stop)
	{
		auto resolved =
			refactor_detail::ResolveWorkspaceEntity(snapshot, document, offset, stop);
		if (!resolved)
		{
			return std::unexpected(resolved.error());
		}

		const auto& index  = resolved->index;
		const auto& entity = index->Entities()[resolved->entity];
		std::map<DocumentId, std::vector<const IndexedOccurrence * >> by_document;
		for (const auto occurrence_index : entity.occurrences)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(
					Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
			}

			if (occurrence_index >= index->Occurrences().size())
			{
				return std::unexpected(Error(RefactoringErrorCode::IncompleteAnalysis,
					"The symbol index is inconsistent"));
			}

			const auto& indexed = index->Occurrences()[occurrence_index];
			if (indexed.resolution != OccurrenceResolution::Resolved)
			{
				return std::unexpected(Error(RefactoringErrorCode::IncompleteAnalysis,
					"An occurrence of this symbol is not resolved"));
			}

			by_document[indexed.document].push_back(&indexed);
		}

		// Conservative global check: any same-spelled entity collides.
		// Hijacks through shadowing locals are additionally caught per
		// document by the binding comparison below.
		if (!index->Named(std::string(new_name)).empty())
		{
			return std::unexpected(Error(RefactoringErrorCode::NameCollision,
				"The new name is already declared in the workspace"));
		}

		RefactoringPlan plan {"Rename " + entity.qualified_name + " to " +
				std::string(new_name),
			snapshot.Revision(), {}};
		for (const auto& [affected, occurrences] : by_document)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(
					Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
			}

			if (!snapshot.Contains(affected))
			{
				return std::unexpected(
					Error(RefactoringErrorCode::StaleSnapshot,
					"Workspace changed; request the refactoring again"));
			}

			const auto affected_source = snapshot.Source(affected);
			if (!affected_source)
			{
				return std::unexpected(Error(RefactoringErrorCode::IncompleteAnalysis,
					"Document is not available"));
			}

			DocumentEdits edits {affected, snapshot.Path(affected),
				snapshot.Version(affected), affected_source, {},
				snapshot.Options(affected)};
			for (const auto* indexed : occurrences)
			{
				if (indexed->offset + indexed->length > affected_source->size())
				{
					return std::unexpected(
						Error(RefactoringErrorCode::StaleSnapshot,
						"Workspace changed; request the refactoring again"));
				}

				edits.edits.push_back(
					{indexed->offset, indexed->length,
						std::string(affected_source->substr(indexed->offset, indexed->length)),
						std::string(new_name)});
			}

			const auto documentationTree = snapshot.Syntax(affected);
			if (documentationTree)
			{
				std::vector<std::size_t> declarations;
				for (const auto* occurrence : occurrences)
				{
					if (occurrence->role == OccurrenceRole::Declaration ||
						occurrence->role == OccurrenceRole::Definition)
					{
						declarations.push_back(occurrence->offset);
					}
				}

				const auto separator = entity.qualified_name.rfind("::");
				const auto name = separator == std::string::npos ? entity.qualified_name
				: entity.qualified_name.substr(separator + 2);
				AppendDocumentationEdits(*documentationTree, edits, declarations, name, new_name,
					entity.kind == SymbolKind::Function,
					index->Named(entity.qualified_name).size() == 1, entity.qualified_name);
			}

			plan.documents.push_back(std::move(edits));
		}

		auto preview = PreviewRefactoring(snapshot, plan, stop);
		if (!preview)
		{
			return std::unexpected(preview.error());
		}

		const auto preview_for =[&](DocumentId affected)->const RefactoredSource *
		{
			for (const auto& refactored : *preview)
			{
				if (refactored.document == affected)
				{
					return &refactored;
				}
			}

			return nullptr;
		};

		for (const auto& renamed : plan.documents)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(
					Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
			}

			const auto before_tree  = snapshot.Syntax(renamed.document);
			const auto before_model = snapshot.Semantic(renamed.document);
			const auto refactored   = preview_for(renamed.document);
			if (!before_tree ||!before_model || refactored == nullptr)
			{
				return std::unexpected(Error(RefactoringErrorCode::IncompleteAnalysis,
					"Document is not available"));
			}

			if (!before_tree->Diagnostics().empty())
			{
				return std::unexpected(Error(RefactoringErrorCode::IncompleteAnalysis,
					"Document has parse errors"));
			}

			if (!before_tree->Directives().empty() ||
				!snapshot.Options(renamed.document).Macros().empty())
			{
				const auto verified = refactor_detail::VerifyPreprocessing(
					snapshot, renamed.document, refactored->source, stop);
				if (!verified)
				{
					return std::unexpected(verified.error());
				}
			}

			const auto reuse = MakeReuse(before_tree.get(), renamed.edits,
				renamed.original->size(), refactored->source.size());
			auto after_tree =
				ParseTree::Parse(refactored->source, snapshot.Options(renamed.document), stop,
				nullptr, &reuse);
			if (after_tree.Cancelled() || stop.stop_requested())
			{
				return std::unexpected(
					Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
			}

			if (!after_tree.Diagnostics().empty())
			{
				return std::unexpected(Error(RefactoringErrorCode::SemanticChange,
					"Rename introduces parse errors"));
			}

			const auto after        = Binder::Bind(after_tree);
			const auto before_types = snapshot.Types(renamed.document);
			const auto after_types  = Typer::Type(after);
			std::unordered_set<std::size_t> edited;
			for (const auto& edit : renamed.edits)
			{
				edited.insert(edit.offset);
			}

			const auto unchanged = CheckRenamedModel(
				*before_tree, *before_model,
				before_types ? before_types.get() : nullptr, after_tree, after, &after_types,
				edited, new_name);
			if (!unchanged)
			{
				return std::unexpected(unchanged.error());
			}
		}

		return plan;
	}

	std::expected<std::vector<DocumentOccurrences>, RefactoringError>
	RefactoringService::WorkspaceReferences(
		const AnalysisSnapshot& snapshot,
		DocumentId document,
		std::size_t offset,
		bool include_declaration,
		std::stop_token stop)
	{
		auto resolved =
			refactor_detail::ResolveWorkspaceEntity(snapshot, document, offset, stop);
		if (!resolved)
		{
			return std::unexpected(resolved.error());
		}

		const auto& index  = resolved->index;
		const auto& entity = index->Entities()[resolved->entity];
		std::map<DocumentId, std::vector<std::uint32_t>> by_document;
		for (const auto occurrence_index : entity.occurrences)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(
					Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
			}

			if (occurrence_index >= index->Occurrences().size())
			{
				return std::unexpected(Error(RefactoringErrorCode::IncompleteAnalysis,
					"The symbol index is inconsistent"));
			}

			const auto& indexed = index->Occurrences()[occurrence_index];
			if (indexed.resolution != OccurrenceResolution::Resolved)
			{
				return std::unexpected(Error(RefactoringErrorCode::IncompleteAnalysis,
					"An occurrence of this symbol is not resolved"));
			}

			if (!include_declaration &&
				(indexed.role == OccurrenceRole::Declaration ||
				indexed.role == OccurrenceRole::Definition))
			{
				continue;
			}

			if (indexed.token == kNone)
			{
				return std::unexpected(Error(RefactoringErrorCode::IncompleteAnalysis,
					"An occurrence of this symbol is not resolved"));
			}

			by_document[indexed.document].push_back(indexed.token);
		}

		std::vector<DocumentOccurrences> result;
		for (auto& [affected, tokens] : by_document)
		{
			std::ranges::sort(tokens);
			result.push_back({affected, std::move(tokens)});
		}

		return result;
	}

	std::expected<RefactoringPlan, RefactoringError> RefactoringService::RenameLocal(
		const AnalysisSnapshot& snapshot,
		DocumentId document,
		std::size_t offset,
		std::string_view new_name,
		std::stop_token stop)
	{
		if (!Identifier(new_name))
		{
			return std::unexpected(Error(RefactoringErrorCode::InvalidName,
				"Use a non-reserved ASCII C++ identifier"));
		}

		auto refs = References(snapshot, document, offset, true, stop);
		if (!refs)
		{
			// Locals and fields resolve in a single tree; free functions and
			// globals resolve across the workspace instead.
			if (refs.error().code == RefactoringErrorCode::UnsupportedSymbol)
			{
				return RenameWorkspace(snapshot, document, offset, new_name, stop);
			}

			return std::unexpected(refs.error());
		}

		const auto tree     = snapshot.Syntax(document);
		const auto model    = snapshot.Semantic(document);
		const bool is_field = IsFieldTarget(*model, refs->symbol);
		if (new_name != refs->name)
		{
			if (is_field)
			{
				const auto field_types = snapshot.Types(document);
				if (!field_types)
				{
					return std::unexpected(Error(RefactoringErrorCode::IncompleteAnalysis,
						"Document types are not available"));
				}

				const auto checked =
					CheckFieldName(*model, *tree, *field_types, refs->symbol, new_name, stop);
				if (!checked)
				{
					return std::unexpected(checked.error());
				}
			}
			else
			{
				const auto checked = CheckNameScopes(*model, refs->symbol, new_name, stop);
				if (!checked)
				{
					return std::unexpected(checked.error());
				}
			}
		}

		RefactoringPlan plan {"Rename " + refs->name + " to " + std::string(new_name),
			snapshot.Revision(), {}};
		DocumentEdits edits {document,
			snapshot.Path(document),
			snapshot.Version(document),
			snapshot.Source(document), {},
			snapshot.Options(document)};
		if (new_name != refs->name)
		{
			edits.edits.reserve(refs->tokens.size());
			for (auto index : refs->tokens)
			{
				const auto& token = tree->Tokens()[index];
				edits.edits.push_back(
					{token.offset, token.length, refs->name, std::string(new_name)});
			}

			AppendDocumentationEdits(*tree, edits,
				{tree->Tokens()[model->Symbols().decl_token[refs->symbol]].offset},
				refs->name, new_name, model->Symbols().kind[refs->symbol] == SymbolKind::Class);
		}

		plan.documents.push_back(std::move(edits));
		if (plan.documents.back().edits.empty())
		{
			// Renaming to the same name: the preview would be byte-identical and
			// the reparse/rebind below trivially passes, so skip both.
			return plan;
		}

		const auto preview = PreviewRefactoring(snapshot, plan, stop);
		if (!preview)
		{
			return std::unexpected(preview.error());
		}

		if (!tree->Directives().empty() ||!snapshot.Options(document).Macros().empty())
		{
			const auto verified = refactor_detail::VerifyPreprocessing(
				snapshot, document, preview->front().source, stop);
			if (!verified)
			{
				return std::unexpected(verified.error());
			}
		}

		// Revalidate incrementally: the preview only touches the edited spans, so
		// untouched top-level items are copied from the current tree instead of
		// being re-parsed. Reuse is best-effort; items that cannot be proven
		// reusable are parsed normally, so this cannot change the result.
		const auto& applied = plan.documents.back().edits;
		const auto original = snapshot.Source(document);
		const auto reuse =
			MakeReuse(tree.get(), applied, original->size(), preview->front().source.size());

		const auto after_tree = ParseTree::Parse(
			preview->front().source, snapshot.Options(document), stop, nullptr, &reuse);
		if (after_tree.Cancelled() || stop.stop_requested())
		{
			return std::unexpected(Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
		}

		if (!after_tree.Diagnostics().empty())
		{
			return std::unexpected(
				Error(RefactoringErrorCode::SemanticChange, "Rename introduces parse errors"));
		}

		const auto after = Binder::Bind(after_tree);
		std::unordered_set<std::size_t> edited;
		for (const auto& edit : applied)
		{
			edited.insert(edit.offset);
		}

		const TypeModel* check_types       = nullptr;
		const TypeModel* check_after_types = nullptr;
		std::optional<TypeModel> owned_after_types;
		std::shared_ptr<const TypeModel> before_types;
		if (is_field)
		{
			// Member resolution is positional and name-based: a pure rename
			// leaves every MemberExpression attributed to the same symbol.
			// Any drift (e.g. a use captured by a same-named base member)
			// vetoes the rename.
			before_types = snapshot.Types(document);
			if (!before_types)
			{
				return std::unexpected(Error(RefactoringErrorCode::SemanticChange,
					"Document types are not available"));
			}

			owned_after_types.emplace(Typer::Type(after));
			check_types       = before_types.get();
			check_after_types = &owned_after_types.value();
		}

		const auto unchanged = CheckRenamedModel(*tree, *model, check_types, after_tree, after,
			check_after_types, edited, new_name);
		if (!unchanged)
		{
			return std::unexpected(unchanged.error());
		}

		if (stop.stop_requested())
		{
			return std::unexpected(Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
		}

		return plan;
	}
} // namespace heimdall
