#include <algorithm>
#include <map>
#include <unordered_set>

#include <Heimdall/AnalysisContext.hpp>

#include "Support.hpp"

namespace heimdall
{
	namespace
	{
		RefactoringError Block(std::string message)
		{
			return {RefactoringErrorCode::IncompleteAnalysis, std::move(message)};
		}

		std::expected<AnalysisContext, RefactoringError> Context(
			const AnalysisSnapshot& snapshot,
			DocumentId document,
			std::stop_token stop)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(
					RefactoringError {RefactoringErrorCode::Cancelled, "Request cancelled"});
			}

			if (!snapshot.Source(document))
			{
				return std::unexpected(Block("Document is not available"));
			}

			AnalysisContext context(snapshot, document);
			if (!context.Syntax().Diagnostics().empty())
			{
				return std::unexpected(Block("Document has parse errors"));
			}

			return context;
		}

		// Function-local macro safety without a compiler: the file must not
		// include unknown headers or branch on configuration, and the function
		// itself must neither sit under a directive nor name a known or
		// contextual macro. Unknown header macros cannot be enumerated, so any
		// `#include` vetoes; conditional directives veto for the same reason.
		std::expected<void, RefactoringError> MacroFreeFunction(
			const AnalysisContext& context,
			NodeId function)
		{
			const auto& tree   = context.Syntax();
			const auto& soa    = tree.NodesSoA();
			const auto& tokens = tree.Tokens();
			if (function >= soa.size())
			{
				return std::unexpected(Block("Preprocessor context is not supported"));
			}

			const auto range     = context.NodeRange(function);
			const auto range_end = range.offset + range.length;
			std::unordered_set<std::string_view> defined;
			for (const auto& directive : tree.Directives())
			{
				switch (directive.kind)
				{
				case DirectiveKind::Include:
				case DirectiveKind::Undef:
				case DirectiveKind::If:
				case DirectiveKind::Ifdef:
				case DirectiveKind::Ifndef:
				case DirectiveKind::Elif:
				case DirectiveKind::Else:
				case DirectiveKind::Endif:
				case DirectiveKind::Other:
					return std::unexpected(
						Block("Preprocessor context is not supported for this operation"));
				case DirectiveKind::Define:
				case DirectiveKind::Pragma:
					break;
				}

				const bool intersects =
					directive.offset < range_end &&
					directive.offset + directive.length > range.offset;
				if (directive.kind == DirectiveKind::Pragma)
				{
					// Only macro-stack pragmas affect names; other pragmas
					// (including inside functions, e.g. OpenMP) are inert.
					const auto text = tree.Source().substr(
						directive.offset, directive.length);
					if (text.find("push_macro") != std::string_view::npos ||
						text.find("pop_macro") != std::string_view::npos)
					{
						return std::unexpected(
							Block("Preprocessor context is not supported for this operation"));
					}

					continue;
				}

				if (intersects)
				{
					return std::unexpected(
						Block("Preprocessor context is not supported for this operation"));
				}

				if (directive.kind != DirectiveKind::Define)
				{
					continue;
				}

				// The second identifier in the span names the macro.
				bool seen_define = false;
				for (const auto& token : tokens)
				{
					if (token.offset < directive.offset ||
						token.offset >= directive.offset + directive.length)
					{
						continue;
					}

					if (token.kind != TokenKind::Identifier || token.tok != Tok::None)
					{
						continue;
					}

					if (!seen_define)
					{
						if (tree.Text(token) != "define")
						{
							break;
						}

						seen_define = true;
						continue;
					}

					defined.insert(tree.Text(token));
					break;
				}
			}

			constexpr std::string_view contextual[] = {
				"__FILE__", "__LINE__", "__func__", "__FUNCTION__", "__PRETTY_FUNCTION__",
				"__DATE__", "__TIME__", "__TIMESTAMP__", "__COUNTER__", "__has_include",
				"__has_cpp_attribute"
			};
			const auto& predefined = context.Snapshot().Options(context.Document()).Macros();
			const auto first       = soa.FirstToken(function);
			const auto end         = first + soa.TokenCount(function);
			for (auto i = first; i < end && i < tokens.size(); ++i)
			{
				if (tokens[i].kind != TokenKind::Identifier || tokens[i].tok != Tok::None)
				{
					continue;
				}

				const auto spelling = tree.Text(tokens[i]);
				if (defined.find(spelling) != defined.end() ||
					predefined.find(spelling) != predefined.end())
				{
					return std::unexpected(
						Block("Preprocessor context is not supported for this operation"));
				}

				for (const auto builtin : contextual)
				{
					if (spelling == builtin)
					{
						return std::unexpected(
							Block("Preprocessor context is not supported for this operation"));
					}
				}
			}

			return {};
		}

		NodeId ExactNode(const AnalysisContext& context, std::size_t offset, std::size_t length)
		{
			const auto& soa = context.Syntax().NodesSoA();
			NodeId result   = InvalidNode;
			for (NodeId node = 0; node < soa.size(); ++node)
			{
				const auto range = context.NodeRange(node);
				if (range.offset == offset && range.length == length)
				{
					result = node;
				}
			}

			return result;
		}

		bool OrdinaryFunction(const AnalysisContext& context, NodeId node)
		{
			const auto& soa = context.Syntax().NodesSoA();
			NodeId function = InvalidNode;
			for (auto current = node; current < soa.size(); current = soa.Parent(current))
			{
				if (soa.Kind(current) == GrammarKind::LambdaExpression ||
					soa.Kind(current) == GrammarKind::TemplateDeclaration)
				{
					return false;
				}

				if (soa.Kind(current) == GrammarKind::FunctionDefinition && function == InvalidNode)
				{
					function = current;
				}

				if (current == 0 || soa.Parent(current) == current)
				{
					break;
				}
			}

			if (function == InvalidNode)
			{
				return false;
			}

			const auto& model   = context.Semantic();
			const auto& symbols = model.Symbols();
			for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
				if (symbols.decl_node[symbol] == function &&
				symbols.kind[symbol] == SymbolKind::Function)
			{
				const auto type = context.Types().SymbolType(symbol);
				if (context.Types().Types().Kind(type) != TypeKind::Builtin ||
					context.Types().Types().IsBuiltin(type, BuiltinType::Void))
				{
					return false;
				}

				// Deduced return types and function contracts need more proof.
				for (auto i = soa.FirstToken(function); i < soa.FirstToken(node); ++i)
				{
					const auto tok = context.Syntax().Tokens()[i].tok;
					if (tok == Tok::KwAuto || tok == Tok::KwDecltype || tok == Tok::KwNoexcept)
					{
						return false;
					}

					if (tok == Tok::LBrace)
					{
						break;
					}
				}

				return true;
			}

			return false;
		}

		// A pure read site: `symbol` provides the value, `param` names the
		// synthesized parameter and `value` spells the argument at the call
		// site. Views point into the pinned source buffer.
		struct PureRead
		{
			SymbolId symbol = kNone;
			std::string_view param;
			std::string_view value;
		};

		// Any `volatile` in the declaration's tokens: re-execution may observe
		// a different value, so the read is not pure. Unknown symbols are
		// impure by default.
		bool VolatileQualified(const AnalysisContext& context, SymbolId symbol)
		{
			const auto& tree    = context.Syntax();
			const auto& soa     = tree.NodesSoA();
			const auto& symbols = context.Semantic().Symbols();
			if (symbol >= symbols.Size())
			{
				return true;
			}

			auto node = symbols.decl_node[symbol];
			while (node < soa.size() && soa.Kind(node) != GrammarKind::Declaration &&
				soa.Kind(node) != GrammarKind::DeclarationStatement &&
				soa.Kind(node) != GrammarKind::ParameterDeclaration)
			{
				if (node == 0 || soa.Parent(node) == node)
				{
					return true;
				}

				node = soa.Parent(node);
			}

			if (node >= soa.size())
			{
				return true;
			}

			const auto first = soa.FirstToken(node);
			const auto end =
				std::min<std::uint32_t>(first + soa.TokenCount(node),
				static_cast<std::uint32_t>(tree.Tokens().size()));
			for (auto i = first; i < end; ++i)
			{
				if (tree.Tokens()[i].tok == Tok::KwVolatile)
				{
					return true;
				}
			}

			return false;
		}

		// `constexpr`/`consteval` on a function head (tokens before the body):
		// observably deterministic, so calls with pure arguments are pure.
		// Templates are excluded (dependent bodies).
		bool ConstexprFunction(const AnalysisContext& context, SymbolId symbol)
		{
			const auto& tree    = context.Syntax();
			const auto& soa     = tree.NodesSoA();
			const auto& symbols = context.Semantic().Symbols();
			if (symbol >= symbols.Size() || symbols.kind[symbol] != SymbolKind::Function ||
				(symbols.flags[symbol] & SymbolFlag::Template) != 0)
			{
				return false;
			}

			auto node = symbols.decl_node[symbol];
			while (node < soa.size() && soa.Kind(node) != GrammarKind::FunctionDefinition &&
				soa.Kind(node) != GrammarKind::FunctionDeclaration)
			{
				if (node == 0 || soa.Parent(node) == node)
				{
					return false;
				}

				node = soa.Parent(node);
			}

			if (node >= soa.size())
			{
				return false;
			}

			auto limit = soa.FirstToken(node) + soa.TokenCount(node);
			for (const auto child : context.Semantic().ChildrenOf(node))
			{
				if (soa.Kind(child) == GrammarKind::CompoundStatement)
				{
					limit = soa.FirstToken(child);
					break;
				}
			}

			const auto end =
				std::min<std::uint32_t>(limit, static_cast<std::uint32_t>(tree.Tokens().size()));
			for (auto i = soa.FirstToken(node); i < end; ++i)
			{
				if (tree.Tokens()[i].tok == Tok::KwConstexpr ||
					tree.Tokens()[i].tok == Tok::KwConsteval)
				{
					return true;
				}
			}

			return false;
		}

		bool PureNode(const AnalysisContext& context, NodeId node, std::vector<PureRead>& reads);

		// Structural purity of a member-access receiver (no value-type gate:
		// objects and pointers are not evaluated as values here).
		bool PureReceiver(const AnalysisContext& context, NodeId node)
		{
			const auto& tree    = context.Syntax();
			const auto& soa     = tree.NodesSoA();
			const auto& model   = context.Semantic();
			const auto& symbols = model.Symbols();
			switch (soa.Kind(node))
			{
			case GrammarKind::IdentifierExpression:
			{
				// `this` is an implicit pure receiver, never a parameter.
				const auto receiver_range = context.NodeRange(node);
				if (tree.Source().substr(receiver_range.offset, receiver_range.length) == "this")
				{
					return true;
				}

				const auto token = soa.FirstToken(node);
				if (token >= tree.Tokens().size() || tree.Tokens()[token].tok != Tok::None)
				{
					return false;
				}

				const auto target = model.ResolveToken(token);
				return target != kNone && target < symbols.Size() &&
					(symbols.kind[target] == SymbolKind::Variable ||
					symbols.kind[target] == SymbolKind::Parameter) &&
					!VolatileQualified(context, target);
			}
			case GrammarKind::MemberExpression:
			{
				const auto kids = model.ChildrenOf(node);
				if (kids.size() != 2)
				{
					return false;
				}

				const auto member = context.Types().MemberSymbol(node);
				if (member == kNone || member >= symbols.Size() ||
					(symbols.kind[member] != SymbolKind::Variable &&
					symbols.kind[member] != SymbolKind::Parameter) ||
					VolatileQualified(context, member))
				{
					return false;
				}

				return PureReceiver(context, kids[0]);
			}
			default:
				return false;
			}
		}

		bool PureNode(const AnalysisContext& context, NodeId node, std::vector<PureRead>& reads)
		{
			const auto& tree    = context.Syntax();
			const auto& soa     = tree.NodesSoA();
			const auto& model   = context.Semantic();
			const auto& symbols = model.Symbols();
			const auto& types   = context.Types().Types();
			const auto gated =[&](NodeId gated_node)
			{
				const auto type = context.Types().NodeType(gated_node);
				return types.IsInteger(type) || types.IsBool(type);
			};

			switch (soa.Kind(node))
			{
			case GrammarKind::LiteralExpression:
				return gated(node);
			case GrammarKind::ParenthesizedExpression:
			case GrammarKind::UnaryExpression:
			case GrammarKind::BinaryExpression:
				if (!gated(node))
				{
					return false;
				}

				for (const auto child : model.ChildrenOf(node))
				{
					if (!PureNode(context, child, reads))
					{
						return false;
					}
				}

				return true;
			case GrammarKind::IdentifierExpression:
			{
				const auto range    = context.NodeRange(node);
				const auto spelling = tree.Source().substr(range.offset, range.length);
				if (spelling == "true" || spelling == "false")
				{
					return gated(node);
				}

				// Member names and objects are validated by the
				// MemberExpression branch.
				const auto parent = soa.Parent(node);
				if (parent < soa.size() &&
					soa.Kind(parent) == GrammarKind::MemberExpression)
				{
					return true;
				}

				const auto token = soa.FirstToken(node);
				if (token >= tree.Tokens().size() || tree.Tokens()[token].tok != Tok::None)
				{
					return false;
				}

				const auto target = model.ResolveToken(token);
				if (target == kNone || target >= symbols.Size() ||
					(symbols.kind[target] != SymbolKind::Variable &&
					symbols.kind[target] != SymbolKind::Parameter) ||
					VolatileQualified(context, target) ||!gated(node))
				{
					return false;
				}

				reads.push_back({target, tree.Text(tree.Tokens()[token]),
						tree.Text(tree.Tokens()[token])});
				return true;
			}
			case GrammarKind::CallExpression:
			{
				if (!gated(node))
				{
					return false;
				}

				const auto kids = model.ChildrenOf(node);
				if (kids.empty())
				{
					return false;
				}

				const auto callee = kids[0];
				if (soa.Kind(callee) != GrammarKind::IdentifierExpression ||
					soa.TokenCount(callee) != 1)
				{
					return false;
				}

				const auto callee_token = soa.FirstToken(callee);
				// Keywords (`sizeof`, `noexcept`, ...) are never callees.
				if (callee_token >= tree.Tokens().size() ||
					tree.Tokens()[callee_token].tok != Tok::None)
				{
					return false;
				}

				const auto target = model.ResolveToken(callee_token);
				if (target == kNone ||!ConstexprFunction(context, target))
				{
					return false;
				}

				for (std::size_t k = 1; k < kids.size(); ++k)
				{
					if (!PureNode(context, kids[k], reads))
					{
						return false;
					}
				}

				return true;
			}
			case GrammarKind::MemberExpression:
			{
				if (!gated(node))
				{
					return false;
				}

				const auto kids = model.ChildrenOf(node);
				if (kids.size() != 2)
				{
					return false;
				}

				const auto member = context.Types().MemberSymbol(node);
				if (member == kNone || member >= symbols.Size() ||
					(symbols.kind[member] != SymbolKind::Variable &&
					symbols.kind[member] != SymbolKind::Parameter) ||
					VolatileQualified(context, member) ||
					!PureReceiver(context, kids[0]))
				{
					return false;
				}

				const auto name_token = soa.FirstToken(kids[1]);
				const auto name_range = context.NodeRange(node);
				reads.push_back({member, tree.Text(tree.Tokens()[name_token]),
						tree.Source().substr(name_range.offset, name_range.length)});
				return true;
			}
			default:
				return false;
			}
		}

		// Checks only supported pure built-in expressions, never class operators
		// with effects, volatile reads, unresolved names, methods or templates.
		bool PureInputs(
			const AnalysisContext& context,
			NodeId expression,
			std::vector<PureRead>& reads)
		{
			const auto& tree  = context.Syntax();
			const auto& soa   = tree.NodesSoA();
			const auto type   = context.Types().NodeType(expression);
			const auto& types = context.Types().Types();
			if (!types.IsInteger(type) && !types.IsBool(type))
			{
				return false;
			}

			for (auto node = expression; node < soa.SubtreeEnd(expression); ++node)
			{
				// The parser represents true/false as identifier expressions.
				const auto range    = context.NodeRange(node);
				const auto spelling = tree.Source().substr(range.offset, range.length);
				const bool boolean_literal = soa.Kind(node) == GrammarKind::IdentifierExpression &&
					(spelling == "true" || spelling == "false");
				if (soa.Kind(node) != GrammarKind::LiteralExpression &&
					soa.Kind(node) != GrammarKind::BinaryExpression &&
					soa.Kind(node) != GrammarKind::UnaryExpression &&
					soa.Kind(node) != GrammarKind::ParenthesizedExpression &&
					soa.Kind(node) != GrammarKind::IdentifierExpression &&
					soa.Kind(node) != GrammarKind::CallExpression &&
					soa.Kind(node) != GrammarKind::MemberExpression && !boolean_literal)
				{
					return false;
				}

				// A bool result may still contain floating-point arithmetic.
				// Materialization can change excess precision or fenv effects.
				// Member receivers are exempt: their type is not evaluated here.
				const auto parent = soa.Parent(node);
				const bool receiver =
					soa.Kind(node) == GrammarKind::IdentifierExpression && parent < soa.size() &&
					soa.Kind(parent) == GrammarKind::MemberExpression;
				if (!receiver)
				{
					const auto operand_type = context.Types().NodeType(node);
					if (!types.IsInteger(operand_type) && !types.IsBool(operand_type))
					{
						return false;
					}
				}
			}

			// Structural purity and effect analysis: every read must resolve to
			// a non-volatile local, parameter or field, and every call to a
			// constexpr function with pure arguments.
			if (!PureNode(context, expression, reads))
			{
				return false;
			}

			for (auto i = soa.FirstToken(expression);
				i < soa.FirstToken(expression) + soa.TokenCount(expression); ++i)
			{
				const auto& token = tree.Tokens()[i];
				// User-defined literals require overload/effect analysis.
				if (token.kind == TokenKind::Number &&
					tree.Text(token).find('_') != std::string_view::npos)
				{
					return false;
				}

				if (token.kind == TokenKind::Whitespace || token.kind == TokenKind::Number ||
					token.kind == TokenKind::CharacterLiteral ||
					token.kind == TokenKind::Identifier)
				{
					continue;
				}

				switch (token.tok)
				{
				case Tok::KwTrue:
				case Tok::KwFalse:
				case Tok::LParen:
				case Tok::RParen:
				case Tok::Plus:
				case Tok::Minus:
				case Tok::Star:
				case Tok::Slash:
				case Tok::Percent:
				case Tok::Lt:
				case Tok::Gt:
				case Tok::Le:
				case Tok::Ge:
				case Tok::EqEq:
				case Tok::BangEq:
				case Tok::Bang:
				case Tok::Tilde:
				case Tok::Amp:
				case Tok::Pipe:
				case Tok::Caret:
				case Tok::AmpAmp:
				case Tok::PipePipe:
				case Tok::Shl:
				case Tok::Shr:
				case Tok::Dot:
				case Tok::Arrow:
				case Tok::Comma:
				case Tok::ColonColon:
					break;
				default:
					return false;
				}
			}

			return true;
		}

		bool ConstantScalar(const AnalysisContext& context, NodeId expression)
		{
			std::vector<PureRead> ignored;
			return PureInputs(context, expression, ignored);
		}

		struct Param
		{
			std::string_view name;
			std::string_view value;
			std::string type;
		};

		// Reads bound outside the extracted code become by-value parameters in
		// first-appearance order. The same spelling must carry the same value;
		// only scalar inputs cross the boundary.
		std::expected<std::vector<Param>, RefactoringError> BuildParams(
			const AnalysisContext& context,
			const std::vector<PureRead>& reads)
		{
			std::vector<Param> params;
			for (const auto& read : reads)
			{
				const auto input_type = context.Types().SymbolType(read.symbol);
				if (!context.Types().Types().IsInteger(input_type) &&
					!context.Types().Types().IsBool(input_type))
				{
					return std::unexpected(
						Block("Only scalar inputs are supported for extraction"));
				}

				bool duplicate = false;
				for (const auto& param : params)
				{
					if (param.name == read.param)
					{
						if (param.value != read.value)
						{
							return std::unexpected(
								Block("The same name reads different values here"));
						}

						duplicate = true;
						break;
					}
				}

				if (!duplicate)
				{
					params.push_back(
						{read.param, read.value, context.Types().Spell(input_type)});
				}
			}

			return params;
		}

		// Contiguous sibling declaration statements exactly covered by the
		// selection (same enclosing compound, nothing else intersecting).
		// Empty when the selection is not a statement range.
		[[nodiscard]] std::vector<NodeId> MatchStatements(
			const AnalysisContext& context,
			std::size_t offset,
			std::size_t length)
		{
			const auto& tree = context.Syntax();
			const auto& soa  = tree.NodesSoA();
			const auto end   = offset + length;
			std::vector<NodeId> found;
			NodeId parent = kNone;
			for (NodeId node = 0; node < soa.size(); ++node)
			{
				if (soa.Kind(node) != GrammarKind::DeclarationStatement)
				{
					continue;
				}

				const auto range = context.NodeRange(node);
				if (range.offset < offset || range.offset + range.length > end)
				{
					continue;
				}

				if (parent == kNone)
				{
					parent = soa.Parent(node);
				}
				else if (soa.Parent(node) != parent)
				{
					return {};
				}

				found.push_back(node);
			}

			if (found.empty() || parent >= soa.size() ||
				soa.Kind(parent) != GrammarKind::CompoundStatement)
			{
				return {};
			}

			const auto first_range = context.NodeRange(found.front());
			const auto last_range  = context.NodeRange(found.back());
			if (first_range.offset != offset || last_range.offset + last_range.length != end)
			{
				return {};
			}

			// Ancestors of the range are exempt; any other statement-level node
			// intersecting the selection (nested blocks, labels, control flow)
			// rejects the range.
			std::unordered_set<NodeId> ancestors;
			for (auto current = found.front();
				current < soa.size() && current != soa.Parent(current);
				current = soa.Parent(current))
			{
				ancestors.insert(current);
				if (current == 0)
				{
					break;
				}
			}

			for (NodeId node = 0; node < soa.size(); ++node)
			{
				switch (soa.Kind(node))
				{
				case GrammarKind::Declaration:
				case GrammarKind::DeclarationStatement:
				case GrammarKind::ExpressionStatement:
				case GrammarKind::ReturnStatement:
				case GrammarKind::JumpStatement:
				case GrammarKind::EmptyStatement:
				case GrammarKind::IfStatement:
				case GrammarKind::LoopStatement:
				case GrammarKind::SwitchStatement:
				case GrammarKind::CaseLabel:
				case GrammarKind::TryStatement:
				case GrammarKind::DoStatement:
				case GrammarKind::CompoundStatement:
				case GrammarKind::FunctionDefinition:
				case GrammarKind::FunctionDeclaration:
				case GrammarKind::LambdaExpression:
					break;
				default:
					continue;
				}

				const auto range = context.NodeRange(node);
				if (range.length == 0 || range.offset >= end ||
					range.offset + range.length <= offset)
				{
					continue;
				}

				if (ancestors.find(node) != ancestors.end())
				{
					continue;
				}

				bool covered = false;
				for (const auto covered_node : found)
				{
					if (covered_node == node)
					{
						covered = true;
						break;
					}
				}

				if (!covered)
				{
					return {};
				}
			}

			return found;
		}

		std::expected<RefactoringPlan, RefactoringError> ValidateSyntax(
			const AnalysisSnapshot& snapshot,
			RefactoringPlan plan,
			std::stop_token stop,
			const std::vector<std::pair<DocumentId, SymbolId>>& moved = {})
		{
			const auto preview = PreviewRefactoring(snapshot, plan, stop);
			if (!preview)
			{
				return std::unexpected(preview.error());
			}

			for (const auto& document : *preview)
			{
				const auto tree =
					ParseTree::Parse(document.source, snapshot.Options(document.document), stop);
				if (tree.Cancelled() || stop.stop_requested())
				{
					return std::unexpected(
						RefactoringError {RefactoringErrorCode::Cancelled, "Request cancelled"});
				}

				if (!tree.Diagnostics().empty())
				{
					return std::unexpected(
						RefactoringError {RefactoringErrorCode::SemanticChange,
							"Transformation introduces parse errors"});
				}

				const auto before                      = snapshot.Semantic(document.document);
				const auto after                       = Binder::Bind(tree);
				const auto after_types                 = Typer::Type(after);
				const auto before_types                = snapshot.Types(document.document);
				const std::vector<SourceEdit>* changes = nullptr;
				for (const auto& renamed : plan.documents)
				{
					if (renamed.document == document.document)
					{
						changes = &renamed.edits;
						break;
					}
				}

				if (changes == nullptr)
				{
					return std::unexpected(RefactoringError {
							RefactoringErrorCode::SemanticChange,
							"Transformation loses a document"});
				}

				const auto mapped =[&](std::size_t offset)->std::optional<std::size_t>
				{
					std::size_t result = offset;
					for (const auto& edit : *changes)
					{
						if (offset >= edit.offset && offset - edit.offset < edit.length)
						{
							return std::nullopt;
						}

						if (edit.offset <= offset)
						{
							result -= edit.length;
							result += edit.replacement.size();
						}
					}

					return result;
				};
				std::vector<SymbolId> identities(before->Symbols().Size(), kNone);
				const auto& before_tokens = before->Tree().Tokens();
				const auto& after_tokens  = tree.Tokens();
				const auto is_moved =[&](SymbolId symbol)
				{
					for (const auto& [moved_document, moved_symbol] : moved)
					{
						if (moved_document == document.document && moved_symbol == symbol)
						{
							return true;
						}
					}

					return false;
				};

				for (SymbolId i = 0; i < before->Symbols().Size(); ++i)
				{
					if (is_moved(i))
					{
						continue;
					}

					const auto location =
						mapped(before_tokens[before->Symbols().decl_token[i]].offset);
					if (!location)
					{
						continue;
					} // the inlined declaration was removed

					for (SymbolId j = 0; j < after.Symbols().Size(); ++j)
						if (after_tokens[after.Symbols().decl_token[j]].offset == *location)
					{
						identities[i] = j;
						break;
					}

					const auto j = identities[i];
					if (j == kNone || before->Symbols().kind[i] != after.Symbols().kind[j] ||
						before->Symbols().flags[i] != after.Symbols().flags[j] ||
						before->Names().Text(before->Symbols().name[i]) !=
						after.Names().Text(after.Symbols().name[j]) ||
						before_types->Spell(before_types->SymbolType(i)) !=
						after_types.Spell(after_types.SymbolType(j)))
					{
						return std::unexpected(RefactoringError {
								RefactoringErrorCode::SemanticChange,
								"Transformation changes an existing declaration or type"});
					}
				}

				for (std::size_t i = 0; i < before->Refs().token.size(); ++i)
				{
					const auto location = mapped(before_tokens[before->Refs().token[i]].offset);
					if (!location)
					{
						continue;
					}

					const auto token =
						std::ranges::lower_bound(after_tokens, *location, {}, &Token::offset);
					if (token == after_tokens.end() || token->offset != *location)
					{
						return std::unexpected(
							RefactoringError {RefactoringErrorCode::SemanticChange,
								"Transformation loses an existing reference"});
					}

					const auto target   = before->Refs().target[i];
					const auto expected = target == kNone ? kNone : identities[target];
					const auto rebound =
						after.ResolveToken(static_cast<std::uint32_t>(token - after_tokens.begin()));
					bool moved = false;
					if (target != kNone && expected == kNone && rebound != kNone)
					{
						// Moved declarations (multi-statement extraction): the old
						// target's declaration was replaced, so surviving uses
						// must resolve to the fresh call-site declarator: same
						// name, kind and scope, with no pre-image. Helper-local
						// redeclarations live in another scope and do not count.
						const auto old_name  = before->Names().Text(before->Symbols().name[target]);
						const auto old_kind  = before->Symbols().kind[target];
						const auto use_scope = after.Symbols().scope[rebound];
						std::size_t matches  = 0;
						for (SymbolId fresh = 0; fresh < after.Symbols().Size(); ++fresh)
						{
							if (after.Symbols().kind[fresh] != old_kind ||
								after.Names().Text(after.Symbols().name[fresh]) != old_name ||
								after.Symbols().scope[fresh] != use_scope)
							{
								continue;
							}

							bool inherited = false;
							for (const auto image : identities)
							{
								if (image == fresh)
								{
									inherited = true;
									break;
								}
							}

							if (!inherited)
							{
								++matches;
								moved = (fresh == rebound);
							}
						}

						moved = moved && matches == 1;
					}

					if (rebound != expected && !moved)
					{
						return std::unexpected(RefactoringError {
								RefactoringErrorCode::SemanticChange,
								"Transformation changes an existing reference binding"});
					}

					if (target != kNone && expected == kNone && !moved)
					{
						return std::unexpected(RefactoringError {
								RefactoringErrorCode::SemanticChange,
								"Transformation changes an existing reference binding"});
					}
				}
			}

			if (stop.stop_requested())
			{
				return std::unexpected(
					RefactoringError {RefactoringErrorCode::Cancelled, "Request cancelled"});
			}

			return plan;
		}

		RefactoringPlan Plan(
			const AnalysisContext& context,
			std::string title,
			std::vector<SourceEdit> edits)
		{
			const auto& snapshot = context.Snapshot();
			const auto id        = context.Document();
			return {std::move(title),
				snapshot.Revision(),
				{
					{
						id, snapshot.Path(id), snapshot.Version(id), snapshot.Source(id),
							std::move(edits), snapshot.Options(id)
			}}};
		}
	} // namespace

	// The inserted initializer lives in the enclosing block until the block
	// ends. It is only crossed when control can land past it without
	// running it: a later `case` label in the same block, or a `goto` from
	// before it to a label at or after the insertion point. Gotos after
	// it, labels it does not cross and jumps in nested blocks all execute
	// the initializer first and are safe. Shared by the single-statement,
	// constant and statement extractions (all insert before a statement).
	std::expected<void, RefactoringError> CheckInitializerJumps(
		const AnalysisContext& context,
		NodeId insertion_statement,
		std::size_t insert_offset,
		NodeId function)
	{
		const auto& tree = context.Syntax();
		const auto& soa  = tree.NodesSoA();
		NodeId block     = insertion_statement;
		while (block < soa.size() && soa.Kind(block) != GrammarKind::CompoundStatement &&
			soa.Kind(block) != GrammarKind::FunctionDefinition)
		{
			if (block == 0 || soa.Parent(block) == block)
			{
				break;
			}

			block = soa.Parent(block);
		}

		if (block >= soa.size() || soa.Kind(block) != GrammarKind::CompoundStatement)
		{
			return std::unexpected(
				Block("Extraction in functions with case labels needs jump analysis"));
		}

		const auto stmt_first = soa.FirstToken(insertion_statement);
		const auto stmt_end   = stmt_first + soa.TokenCount(insertion_statement);
		const auto block_end  = soa.FirstToken(block) + soa.TokenCount(block);

		// A later `case`/`default` label in the same block dispatches past
		// the inserted initializer, which is ill-formed.
		for (auto i = block; i < soa.SubtreeEnd(block); ++i)
		{
			if (soa.Kind(i) == GrammarKind::CaseLabel && soa.Parent(i) == block &&
				soa.FirstToken(i) > stmt_first)
			{
				return std::unexpected(
					Block("Extraction in functions with case labels needs jump analysis"));
			}
		}

		bool goto_before = false;
		for (auto i = soa.FirstToken(function);
			i < soa.FirstToken(function) + soa.TokenCount(function); ++i)
		{
			if (tree.Tokens()[i].tok == Tok::KwGoto &&
				tree.Tokens()[i].offset < insert_offset)
			{
				goto_before = true;
				break;
			}
		}

		if (goto_before)
		{
			// A label attached to the statement (`done: return ...`) or
			// after it in the same block is a landing spot past the
			// initializer. Colons inside the expression cannot be labels.
			const auto& tokens = tree.Tokens();
			bool labelled = stmt_first > 0 && stmt_first <= tokens.size() &&
				tokens[stmt_first - 1].tok == Tok::Colon;
			for (auto i = stmt_end; !labelled && i < block_end && i < tokens.size(); ++i)
			{
				if (tokens[i].tok == Tok::Colon)
				{
					labelled = true;
				}
			}

			if (labelled)
			{
				return std::unexpected(
					Block("Extraction in functions with goto needs jump analysis"));
			}
		}

		return {};
	}

	std::expected<RefactoringPlan, RefactoringError> RefactoringService::ExtractVariable(
		const AnalysisSnapshot& snapshot,
		DocumentId document,
		std::size_t offset,
		std::size_t length,
		std::string_view new_name,
		std::stop_token stop)
	{
		if (!refactor_detail::ValidName(new_name))
		{
			return std::unexpected(
				RefactoringError {RefactoringErrorCode::InvalidName, "Invalid C++ identifier"});
		}

		auto context = Context(snapshot, document, stop);
		if (!context)
		{
			return std::unexpected(context.error());
		}

		const auto& tree = context->Syntax();
		const auto& soa  = tree.NodesSoA();
		const auto node  = ExactNode(*context, offset, length);
		if (node == InvalidNode ||!ConstantScalar(*context, node))
		{
			return std::unexpected(Block("Select a complete built-in constant expression"));
		}

		const auto statement = soa.Parent(node);
		if (statement >= soa.size() || soa.Kind(statement) != GrammarKind::ReturnStatement ||
			soa.Parent(statement) >= soa.size() ||
			soa.Kind(soa.Parent(statement)) != GrammarKind::CompoundStatement ||
			!OrdinaryFunction(*context, node))
		{
			return std::unexpected(
				Block("Only complete expressions in standalone scalar returns are supported"));
		}

		auto function = statement;
		while (function < soa.size() && soa.Kind(function) != GrammarKind::FunctionDefinition)
		{
			if (function == 0)
			{
				break;
			}

			function = soa.Parent(function);
		}

		if (auto macros = MacroFreeFunction(*context, function); !macros)
		{
			return std::unexpected(macros.error());
		}

		const auto range = context->NodeRange(statement);
		if (auto jumps = CheckInitializerJumps(*context, statement, range.offset, function);
			!jumps)
		{
			return std::unexpected(jumps.error());
		}

		const auto source       = tree.Source();
		const auto line         = range.offset == 0 ? 0 : source.rfind('\n', range.offset - 1) + 1;
		const auto prefix       = source.substr(line, range.offset - line);
		const bool own_line     = prefix.find_first_not_of(" \t") == std::string_view::npos;
		const auto expression   = std::string(source.substr(offset, length));
		const auto newline      = source.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
		std::string declaration = "auto " + std::string(new_name) + " = " + expression + ";";
		declaration += own_line ? std::string(newline) + std::string(prefix) : " ";
		auto plan = Plan(*context, "Extract variable '" + std::string(new_name) + "'",
			{
				{
					range.offset, 0, {}, std::move(declaration)
			},
				{offset, length, expression, std::string(new_name)}});
		return ValidateSyntax(snapshot, std::move(plan), stop);
	}

	std::expected<RefactoringPlan, RefactoringError> RefactoringService::ExtractConstant(
		const AnalysisSnapshot& snapshot,
		DocumentId document,
		std::size_t offset,
		std::size_t length,
		std::string_view new_name,
		std::stop_token stop)
	{
		if (!refactor_detail::ValidName(new_name))
		{
			return std::unexpected(
				RefactoringError {RefactoringErrorCode::InvalidName, "Invalid C++ identifier"});
		}

		auto context = Context(snapshot, document, stop);
		if (!context)
		{
			return std::unexpected(context.error());
		}

		const auto& tree = context->Syntax();
		const auto& soa  = tree.NodesSoA();
		const auto node  = ExactNode(*context, offset, length);
		if (node == InvalidNode || soa.Kind(node) != GrammarKind::LiteralExpression)
		{
			return std::unexpected(Block("Select a complete literal"));
		}

		const auto literal_type = context->Types().NodeType(node);
		if (!context->Types().Types().IsArithmetic(literal_type))
		{
			return std::unexpected(Block("Only arithmetic literals can be extracted"));
		}

		const auto spelling = tree.Source().substr(offset, length);
		for (const auto& token : tree.Tokens())
		{
			if (token.kind == TokenKind::Number &&
				tree.Text(token).find('_') != std::string_view::npos &&
				tree.Text(token) == spelling)
			{
				return std::unexpected(Block("User-defined literals are not supported"));
			}
		}

		if (!OrdinaryFunction(*context, node))
		{
			return std::unexpected(Block("Only literals in ordinary functions can be extracted"));
		}

		// Outermost statement containing the literal (direct child of a block).
		NodeId statement = node;
		while (statement < soa.size() && soa.Parent(statement) < soa.size() &&
			soa.Kind(soa.Parent(statement)) != GrammarKind::CompoundStatement)
		{
			if (statement == 0 || soa.Parent(statement) == statement)
			{
				break;
			}

			statement = soa.Parent(statement);
		}

		if (statement >= soa.size() || soa.Parent(statement) >= soa.size() ||
			soa.Kind(soa.Parent(statement)) != GrammarKind::CompoundStatement)
		{
			return std::unexpected(Block("Only literals in ordinary functions can be extracted"));
		}

		auto function = statement;
		while (function < soa.size() && soa.Kind(function) != GrammarKind::FunctionDefinition)
		{
			if (function == 0)
			{
				break;
			}

			function = soa.Parent(function);
		}

		if (function >= soa.size())
		{
			return std::unexpected(Block("Only literals in ordinary functions can be extracted"));
		}

		if (auto macros = MacroFreeFunction(*context, function); !macros)
		{
			return std::unexpected(macros.error());
		}

		// Every identical literal in the function is replaced. Literals nested
		// in lambdas veto: naming them would change the capture set.
		std::vector<NodeId> matches;
		for (auto i = function; i < soa.SubtreeEnd(function); ++i)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(
					RefactoringError {RefactoringErrorCode::Cancelled, "Request cancelled"});
			}

			if (soa.Kind(i) != GrammarKind::LiteralExpression)
			{
				continue;
			}

			const auto candidate = context->NodeRange(i);
			if (tree.Source().substr(candidate.offset, candidate.length) != spelling ||
				context->Types().NodeType(i) != literal_type)
			{
				continue;
			}

			for (auto ancestor = i;
				ancestor < soa.size() && ancestor != function && ancestor != soa.Parent(ancestor);
				ancestor = soa.Parent(ancestor))
			{
				if (soa.Kind(ancestor) == GrammarKind::LambdaExpression)
				{
					return std::unexpected(
						Block("Literals captured in lambdas are not supported"));
				}
			}

			matches.push_back(i);
		}

		if (matches.empty())
		{
			return std::unexpected(Block("Select a complete literal"));
		}

		const auto range = context->NodeRange(statement);
		if (auto jumps = CheckInitializerJumps(*context, statement, range.offset, function);
			!jumps)
		{
			return std::unexpected(jumps.error());
		}

		for (const auto& token : tree.Tokens())
		{
			if (token.kind == TokenKind::Identifier && tree.Text(token) == new_name)
			{
				return std::unexpected(
					RefactoringError {RefactoringErrorCode::NameCollision,
						"The new name already occurs in this document"});
			}
		}

		const auto source   = tree.Source();
		const auto line     = range.offset == 0 ? 0 : source.rfind('\n', range.offset - 1) + 1;
		const auto prefix   = source.substr(line, range.offset - line);
		const bool own_line = prefix.find_first_not_of(" \t") == std::string_view::npos;
		const auto type     = context->Types().Spell(literal_type);
		const auto newline  = source.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
		std::string declaration =
			"static constexpr " + type + " " + std::string(new_name) + " = " + std::string(spelling) +
			";";
		declaration += own_line ? std::string(newline) + std::string(prefix) : " ";
		std::vector<SourceEdit> edits {
			{
				range.offset, 0, {}, declaration
		}};
		for (const auto match : matches)
		{
			const auto match_range = context->NodeRange(match);
			edits.push_back({match_range.offset, match_range.length,
					std::string(
					tree.Source().substr(match_range.offset, match_range.length)),
					std::string(new_name)});
		}

		return ValidateSyntax(snapshot, Plan(*context, "Extract constant '" + std::string(new_name) + "'",
			std::move(edits)),
			stop);
	}

	std::expected<RefactoringPlan, RefactoringError> RefactoringService::InlineVariable(
		const AnalysisSnapshot& snapshot,
		DocumentId document,
		std::size_t offset,
		std::stop_token stop)
	{
		auto refs = LocalReferences(snapshot, document, offset, true, stop);
		if (!refs)
		{
			return std::unexpected(refs.error());
		}

		auto context = Context(snapshot, document, stop);
		if (!context)
		{
			return std::unexpected(context.error());
		}

		const auto& model   = context->Semantic();
		const auto& symbols = model.Symbols();
		if (symbols.kind[refs->symbol] != SymbolKind::Variable ||
			(symbols.flags[refs->symbol] & SymbolFlag::Static) || refs->tokens.size() < 2)
		{
			return std::unexpected(
				Block("Select a non-static local variable with at least one read"));
		}

		// The variable's function must be macro-free (its declaration is
		// removed and every read rewritten).
		{
			const auto& scopes = model.Scopes();
			auto function      = symbols.scope[refs->symbol];
			while (function < scopes.Size() && scopes.kind[function] == ScopeKind::Block)
			{
				function = scopes.parent[function];
			}

			if (function >= scopes.Size() || scopes.kind[function] != ScopeKind::Function ||
				scopes.node[function] >= context->Syntax().NodesSoA().size())
			{
				return std::unexpected(Block("A standalone local declaration is required"));
			}

			if (auto macros = MacroFreeFunction(
				*context, scopes.node[function]);
				!macros)
			{
				return std::unexpected(macros.error());
			}
		}

		const auto& tree   = context->Syntax();
		const auto& soa    = tree.NodesSoA();
		NodeId declaration = symbols.decl_node[refs->symbol];
		while (declaration < soa.size() &&
			soa.Kind(declaration) != GrammarKind::DeclarationStatement)
		{
			if (declaration == 0)
			{
				break;
			}

			declaration = soa.Parent(declaration);
		}

		if (declaration >= soa.size() ||
			soa.Kind(declaration) != GrammarKind::DeclarationStatement ||
			!OrdinaryFunction(*context, declaration))
		{
			return std::unexpected(Block("A standalone local declaration is required"));
		}

		NodeId literal = InvalidNode;
		for (auto i = declaration; i < soa.SubtreeEnd(declaration); ++i)
			if (soa.Kind(i) == GrammarKind::LiteralExpression)
		{
			if (literal != InvalidNode)
			{
				return std::unexpected(Block("Only one literal initializer is supported"));
			}

			literal = i;
		}

		if (literal == InvalidNode ||!ConstantScalar(*context, literal) ||
			context->Types().Spell(context->Types().SymbolType(refs->symbol)) !=
			context->Types().Spell(context->Types().NodeType(literal)))
		{
			return std::unexpected(
				Block("Initializer must be a scalar literal with exactly the declared type"));
		}

		const auto literal_range     = context->NodeRange(literal);
		const auto declaration_range = context->NodeRange(declaration);
		// Verify the entire declaration shape, so an expression merely containing
		// a literal (e.g. a call or increment) can never be removed as pure.
		std::vector<std::string_view> significant;
		for (auto i = soa.FirstToken(declaration);
			i < soa.FirstToken(declaration) + soa.TokenCount(declaration); ++i)
		{
			const auto& token = tree.Tokens()[i];
			if (token.kind == TokenKind::Whitespace)
			{
				continue;
			}

			if (token.kind == TokenKind::LineComment || token.kind == TokenKind::BlockComment)
			{
				return std::unexpected(Block("Declaration comments would be removed"));
			}

			significant.push_back(tree.Text(token));
		}

		if (significant.size() != 5 || significant[1] != refs->name || significant[2] != "=" ||
			significant[3] != tree.Source().substr(literal_range.offset, literal_range.length) ||
			significant[4] != ";")
		{
			return std::unexpected(
				Block("Only a simple scalar declaration with a literal initializer is supported"));
		}

		const auto value =
			std::string(tree.Source().substr(literal_range.offset, literal_range.length));
		std::vector<SourceEdit> edits {
			{
				declaration_range.offset,
					declaration_range.length,
					std::string(tree.Source().substr(declaration_range.offset, declaration_range.length)), {}
			}
		};
		for (auto token : refs->tokens)
		{
			if (token == symbols.decl_token[refs->symbol])
			{
				continue;
			}

			const auto& use       = tree.Tokens()[token];
			const auto expression = ExactNode(*context, use.offset, use.length);
			if (expression == InvalidNode ||
				soa.Kind(expression) != GrammarKind::IdentifierExpression)
			{
				return std::unexpected(Block("A read is not modeled"));
			}

			const auto statement = soa.Parent(expression);
			if (statement >= soa.size() || soa.Kind(statement) != GrammarKind::ReturnStatement ||
				soa.Kind(soa.Parent(statement)) != GrammarKind::CompoundStatement)
			{
				return std::unexpected(
					Block("Only direct reads in standalone return statements are supported"));
			}

			edits.push_back({use.offset, use.length, refs->name, value});
		}

		return ValidateSyntax(
			snapshot, Plan(*context, "Inline variable '" + refs->name + "'", std::move(edits)),
			stop);
	}

	// Multi-statement extraction: a run of initialized declarations moves
	// into a static helper returning the single value used afterwards.
	// Straight-line pure code only: no jumps, labels, pointers or
	// references across the boundary, one live output, scalar inputs.
	std::expected<RefactoringPlan, RefactoringError> ExtractStatements(
		const AnalysisSnapshot& snapshot,
		DocumentId document,
		const AnalysisContext& context,
		const std::vector<NodeId>& statements,
		std::size_t offset,
		std::size_t length,
		std::string_view new_name,
		std::stop_token stop)
	{
		const auto& tree    = context.Syntax();
		const auto& soa     = tree.NodesSoA();
		const auto& model   = context.Semantic();
		const auto& symbols = model.Symbols();
		const auto& tokens  = tree.Tokens();
		const auto end      = offset + length;

		struct Declared
		{
			NodeId statement = kNone;
			NodeId init      = InvalidNode;
			SymbolId symbol  = kNone;
			std::string_view name;
		};

		std::vector<Declared> declared;
		std::vector<PureRead> reads;
		for (const auto statement : statements)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(
					RefactoringError {RefactoringErrorCode::Cancelled, "Request cancelled"});
			}

			NodeId init_decl = kNone;
			int declarators  = 0;
			for (const auto child : model.ChildrenOf(statement))
			{
				if (soa.Kind(child) == GrammarKind::InitDeclarator)
				{
					++declarators;
					init_decl = child;
				}
			}

			if (declarators != 1 || init_decl == kNone)
			{
				return std::unexpected(
					Block("Only single initialized declarations can be extracted"));
			}

			NodeId declarator    = kNone;
			int declarator_count = 0;
			std::string_view name;
			std::uint32_t name_token = kNone;
			int names                = 0;
			NodeId init              = InvalidNode;
			for (const auto child : model.ChildrenOf(init_decl))
			{
				if (stop.stop_requested())
				{
					return std::unexpected(
						RefactoringError {RefactoringErrorCode::Cancelled, "Request cancelled"});
				}

				if (soa.Kind(child) == GrammarKind::Declarator)
				{
					++declarator_count;
					declarator = child;
					continue;
				}

				switch (soa.Kind(child))
				{
				case GrammarKind::LiteralExpression:
				case GrammarKind::IdentifierExpression:
				case GrammarKind::ParenthesizedExpression:
				case GrammarKind::UnaryExpression:
				case GrammarKind::BinaryExpression:
				case GrammarKind::CallExpression:
				case GrammarKind::MemberExpression:
					if (init != InvalidNode)
					{
						return std::unexpected(
							Block("Only single initialized declarations can be extracted"));
					}

					init = child;
					break;
				default:
					break;
				}
			}

			if (declarator_count != 1 || declarator == kNone || init == InvalidNode)
			{
				return std::unexpected(
					Block("Only single initialized declarations can be extracted"));
			}

			for (const auto name_child : model.ChildrenOf(declarator))
			{
				if (soa.Kind(name_child) == GrammarKind::DeclaredName)
				{
					++names;
					name_token = soa.FirstToken(name_child);
					if (name_token < tokens.size())
					{
						name = tree.Text(tokens[name_token]);
					}
				}
			}

			if (names != 1 || name.empty())
			{
				return std::unexpected(
					Block("Only single initialized declarations can be extracted"));
			}

			// Plain value declarators: no pointers, references, arrays or
			// storage qualifiers with lifetime or effect implications.
			const auto declarator_first = soa.FirstToken(declarator);
			const auto init_first       = soa.FirstToken(init);
			for (auto i = declarator_first; i < init_first && i < tokens.size(); ++i)
			{
				if (tokens[i].tok == Tok::Star || tokens[i].tok == Tok::Amp ||
					tokens[i].tok == Tok::AmpAmp || tokens[i].tok == Tok::LBracket ||
					tokens[i].tok == Tok::KwStatic || tokens[i].tok == Tok::KwVolatile)
				{
					return std::unexpected(
						Block("Only plain value declarations can be extracted"));
				}
			}

			if (!PureInputs(context, init, reads))
			{
				return std::unexpected(Block("Only pure initializers can be extracted"));
			}

			SymbolId symbol = kNone;
			for (SymbolId i = 0; i < symbols.Size(); ++i)
			{
				if (symbols.kind[i] == SymbolKind::Variable && symbols.decl_token[i] == name_token)
				{
					symbol = i;
					break;
				}
			}

			if (symbol == kNone)
			{
				return std::unexpected(Block("A declaration is not modeled"));
			}

			declared.push_back({statement, init, symbol, name});
		}

		// Liveness: exactly one declared value may be read after the range.
		SymbolId output = kNone;
		for (const auto& declared_statement : declared)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(
					RefactoringError {RefactoringErrorCode::Cancelled, "Request cancelled"});
			}

			bool live = false;
			for (std::size_t i = 0; i < model.Refs().token.size(); ++i)
			{
				if (model.Refs().target[i] == declared_statement.symbol &&
					tokens[model.Refs().token[i]].offset >= end)
				{
					live = true;
					break;
				}
			}

			if (live)
			{
				if (output != kNone)
				{
					return std::unexpected(
						Block("Only one extracted value may be used afterwards"));
				}

				output = declared_statement.symbol;
			}
		}

		if (output == kNone)
		{
			return std::unexpected(Block("No extracted value is used afterwards"));
		}

		const auto output_type = context.Types().SymbolType(output);
		if (!context.Types().Types().IsInteger(output_type) &&
			!context.Types().Types().IsBool(output_type))
		{
			return std::unexpected(Block("Only scalar outputs can be extracted"));
		}

		// Inputs: reads bound outside the range.
		std::vector<PureRead> external;
		for (const auto& read : reads)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(
					RefactoringError {RefactoringErrorCode::Cancelled, "Request cancelled"});
			}

			const auto declared_offset = tokens[symbols.decl_token[read.symbol]].offset;
			if (declared_offset < offset || declared_offset >= end)
			{
				external.push_back(read);
			}
		}

		auto params_result = BuildParams(context, external);
		if (!params_result)
		{
			return std::unexpected(params_result.error());
		}

		// Jump safety, generalized from the single-statement rule: the
		// moved span must not be crossed by a later case label or by a
		// goto from before it, and it must not contain labels.
		NodeId block = statements.front();
		while (block < soa.size() && soa.Kind(block) != GrammarKind::CompoundStatement &&
			soa.Kind(block) != GrammarKind::FunctionDefinition)
		{
			if (block == 0 || soa.Parent(block) == block)
			{
				break;
			}

			block = soa.Parent(block);
		}

		NodeId function = block;
		while (function < soa.size() && soa.Kind(function) != GrammarKind::FunctionDefinition)
		{
			if (function == 0 || soa.Parent(function) == function)
			{
				break;
			}

			function = soa.Parent(function);
		}

		if (block >= soa.size() || soa.Kind(block) != GrammarKind::CompoundStatement ||
			function >= soa.size() ||
			(soa.Kind(soa.Parent(function)) != GrammarKind::TranslationUnit &&
			soa.Kind(soa.Parent(function)) != GrammarKind::NamespaceDefinition) ||
			!OrdinaryFunction(context, statements.front()))
		{
			return std::unexpected(
				Block("Only straight-line code in free functions can be extracted"));
		}

		if (auto macros = MacroFreeFunction(context, function); !macros)
		{
			return std::unexpected(macros.error());
		}

		const auto first_token = soa.FirstToken(statements.front());
		const auto block_end   = soa.FirstToken(block) + soa.TokenCount(block);
		for (auto i = block; i < soa.SubtreeEnd(block); ++i)
		{
			if (soa.Kind(i) == GrammarKind::CaseLabel && soa.Parent(i) == block &&
				soa.FirstToken(i) > first_token)
			{
				return std::unexpected(
					Block("Extraction in functions with case labels needs jump analysis"));
			}
		}

		const auto block_range      = context.NodeRange(block);
		const auto block_end_offset = block_range.offset + block_range.length;
		bool goto_before            = false;
		for (auto i = soa.FirstToken(function);
			i < soa.FirstToken(function) + soa.TokenCount(function); ++i)
		{
			if (tokens[i].tok == Tok::KwGoto && tokens[i].offset < offset)
			{
				goto_before = true;
				break;
			}
		}

		if (goto_before)
		{
			// A label attached to the range or after it in the same block
			// is a landing spot past the moved initializers.
			bool labelled = first_token > 0 && first_token <= tokens.size() &&
				tokens[first_token - 1].tok == Tok::Colon;
			for (auto i = first_token; !labelled && i < tokens.size(); ++i)
			{
				if (tokens[i].offset >= block_end_offset)
				{
					break;
				}

				if (tokens[i].tok == Tok::Colon && tokens[i].offset >= end)
				{
					labelled = true;
				}
			}

			if (labelled)
			{
				return std::unexpected(
					Block("Extraction in functions with goto needs jump analysis"));
			}
		}

		// Labels strictly inside the moved span (ternaries cannot appear:
		// pure initializers have no `?:`).
		for (auto i = first_token; i < tokens.size() && tokens[i].offset < end; ++i)
		{
			if (tokens[i].tok == Tok::Colon)
			{
				return std::unexpected(
					Block("Extraction in functions with goto needs jump analysis"));
			}
		}

		for (const auto& token : tree.Tokens())
		{
			if (token.kind == TokenKind::Identifier && tree.Text(token) == new_name)
			{
				return std::unexpected(
					RefactoringError {RefactoringErrorCode::NameCollision,
						"The new name already occurs in this document"});
			}
		}

		const auto& params = *params_result;
		std::string signature;
		std::string arguments;
		for (std::size_t i = 0; i < params.size(); ++i)
		{
			if (i != 0)
			{
				signature += ", ";
				arguments += ", ";
			}

			signature += params[i].type + " " + std::string(params[i].name);
			arguments += params[i].value;
		}

		const auto output_name    = model.Names().Text(symbols.name[output]);
		const auto source         = tree.Source();
		const auto function_range = context.NodeRange(function);
		const auto line = function_range.offset == 0
		? 0
		: source.rfind('\n', function_range.offset - 1) + 1;
		const auto prefix = source.substr(line, function_range.offset - line);
		const bool own_line =
			prefix.find_first_not_of(" \t") == std::string_view::npos;
		const std::string newline =
			source.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
		const std::string moved = std::string(source.substr(offset, length));
		const auto indent       = std::string(own_line ? prefix : std::string_view {});
		std::string definition = "static constexpr " +
			context.Types().Spell(output_type) + " " +
			std::string(new_name) + "(" + signature + ") noexcept" + newline;
		definition += indent + "{" + newline + moved;
		if (!moved.empty() && moved.back() != '\n')
		{
			definition += newline;
		}

		definition += indent + "    return " + std::string(output_name) + ";" + newline +
			indent + "}" + newline + newline;
		if (own_line)
		{
			definition += prefix;
		}

		const std::string call =
			"auto " + std::string(output_name) + " = " + std::string(new_name) + "(" +
			arguments + ");";
		return ValidateSyntax(snapshot,
			Plan(context, "Extract function '" + std::string(new_name) + "'",
			{
				{
					function_range.offset, 0, {}, std::move(definition)
			},
				{offset, length, moved, call}}),
			stop);
	}

	std::expected<RefactoringPlan, RefactoringError> RefactoringService::ExtractFunction(
		const AnalysisSnapshot& snapshot,
		DocumentId document,
		std::size_t offset,
		std::size_t length,
		std::string_view new_name,
		std::stop_token stop)
	{
		if (!refactor_detail::ValidName(new_name))
		{
			return std::unexpected(
				RefactoringError {RefactoringErrorCode::InvalidName, "Invalid C++ identifier"});
		}

		auto context = Context(snapshot, document, stop);
		if (!context)
		{
			return std::unexpected(context.error());
		}

		const auto& tree = context->Syntax();
		const auto& soa  = tree.NodesSoA();
		const auto node  = ExactNode(*context, offset, length);
		// A pure scalar expression: literals, reads of non-volatile
		// locals/parameters/fields and constexpr calls. Effect analysis in
		// PureInputs decides; reads become parameters below.
		std::vector<PureRead> reads;
		if (node == InvalidNode ||!PureInputs(*context, node, reads) ||
			!OrdinaryFunction(*context, node))
		{
			// Statement ranges take the multi-statement path instead. Line
			// selections carry indentation, so trim ASCII whitespace first.
			const auto source_text = tree.Source();
			auto trimmed           = offset;
			auto trimmed_end       = offset + length;
			while (trimmed < trimmed_end &&
				(source_text[trimmed] == ' ' || source_text[trimmed] == '\t' ||
				source_text[trimmed] == '\r' || source_text[trimmed] == '\n'))
			{
				++trimmed;
			}

			while (trimmed_end > trimmed &&
				(source_text[trimmed_end - 1] == ' ' || source_text[trimmed_end - 1] == '\t' ||
				source_text[trimmed_end - 1] == '\r' || source_text[trimmed_end - 1] == '\n'))
			{
				--trimmed_end;
			}

			if (auto statements = MatchStatements(*context, trimmed, trimmed_end - trimmed);
				!statements.empty())
			{
				return ExtractStatements(snapshot, document, *context, statements, trimmed,
					trimmed_end - trimmed, new_name, stop);
			}

			return std::unexpected(
				Block("Function extraction supports a complete pure scalar return"));
		}

		const auto statement = soa.Parent(node);
		if (statement >= soa.size() || soa.Kind(statement) != GrammarKind::ReturnStatement ||
			soa.Kind(soa.Parent(statement)) != GrammarKind::CompoundStatement)
		{
			return std::unexpected(Block("A standalone return expression is required"));
		}

		auto function = statement;
		while (function < soa.size() && soa.Kind(function) != GrammarKind::FunctionDefinition)
		{
			if (function == 0)
			{
				break;
			}

			function = soa.Parent(function);
		}

		if (function >= soa.size() ||
			(soa.Kind(soa.Parent(function)) != GrammarKind::TranslationUnit &&
			soa.Kind(soa.Parent(function)) != GrammarKind::NamespaceDefinition))
		{
			return std::unexpected(
				Block("Only free functions in a file or namespace scope are supported"));
		}

		if (auto macros = MacroFreeFunction(*context, function); !macros)
		{
			return std::unexpected(macros.error());
		}

		for (const auto& token : tree.Tokens())
		{
			if (token.kind == TokenKind::Identifier && tree.Text(token) == new_name)
			{
				return std::unexpected(
					RefactoringError {RefactoringErrorCode::NameCollision,
						"The new name already occurs in this document"});
			}
		}

		const auto source         = tree.Source();
		const auto function_range = context->NodeRange(function);
		const auto line =
			function_range.offset == 0 ? 0 : source.rfind('\n', function_range.offset - 1) + 1;
		const auto prefix         = source.substr(line, function_range.offset - line);
		const bool own_line       = prefix.find_first_not_of(" \t") == std::string_view::npos;
		const std::string newline = source.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
		const auto value          = std::string(source.substr(offset, length));
		const auto type           = context->Types().Spell(context->Types().NodeType(node));

		auto params_result = BuildParams(*context, reads);
		if (!params_result)
		{
			return std::unexpected(params_result.error());
		}

		const auto& params = *params_result;
		std::string signature;
		std::string arguments;
		for (std::size_t i = 0; i < params.size(); ++i)
		{
			if (i != 0)
			{
				signature += ", ";
				arguments += ", ";
			}

			signature += params[i].type + " " + std::string(params[i].name);
			arguments += params[i].value;
		}

		std::string definition = "static constexpr " + type + " " + std::string(new_name) + "(" +
			signature + ") noexcept" + newline;
		definition += std::string(own_line ? prefix : std::string_view {}) + "{" + newline;
		definition += std::string(own_line ? prefix : std::string_view {}) + "    return " + value +
			";" + newline;
		definition +=
			std::string(own_line ? prefix : std::string_view {}) + "}" + newline + newline;
		if (own_line)
		{
			definition += prefix;
		}

		const auto call =
			std::string(new_name) + "(" + arguments + ")";
		return ValidateSyntax(snapshot,
			Plan(*context, "Extract function '" + std::string(new_name) + "'",
			{
				{
					function_range.offset, 0, {}, std::move(definition)
			},
				{offset, length, value, call}}),
			stop);
	}

	std::expected<RefactoringPlan, RefactoringError> RefactoringService::InlineFunction(
		const AnalysisSnapshot& snapshot,
		DocumentId document,
		std::size_t offset,
		std::stop_token stop)
	{
		auto context = Context(snapshot, document, stop);
		if (!context)
		{
			return std::unexpected(context.error());
		}

		const auto& tree    = context->Syntax();
		const auto& soa     = tree.NodesSoA();
		const auto& model   = context->Semantic();
		const auto& symbols = model.Symbols();

		// Smallest call expression containing the cursor.
		NodeId call = InvalidNode;
		for (NodeId node = 0; node < soa.size(); ++node)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(
					RefactoringError {RefactoringErrorCode::Cancelled, "Request cancelled"});
			}

			if (soa.Kind(node) != GrammarKind::CallExpression)
			{
				continue;
			}

			const auto range = context->NodeRange(node);
			if (offset < range.offset || offset >= range.offset + range.length)
			{
				continue;
			}

			if (call == InvalidNode ||
				context->NodeRange(node).length < context->NodeRange(call).length)
			{
				call = node;
			}
		}

		if (call == InvalidNode)
		{
			return std::unexpected(Block("Select a function call to inline"));
		}

		const auto call_kids = model.ChildrenOf(call);
		if (call_kids.empty() || soa.Kind(call_kids[0]) != GrammarKind::IdentifierExpression ||
			soa.TokenCount(call_kids[0]) != 1)
		{
			return std::unexpected(Block("Only direct calls to named functions can be inlined"));
		}

		const auto callee_token = soa.FirstToken(call_kids[0]);
		if (callee_token >= tree.Tokens().size() ||
			tree.Tokens()[callee_token].tok != Tok::None)
		{
			return std::unexpected(Block("Only direct calls to named functions can be inlined"));
		}

		const auto callee = model.ResolveToken(callee_token);
		if (callee == kNone || callee >= symbols.Size() ||
			symbols.kind[callee] != SymbolKind::Function ||
			(symbols.flags[callee] & SymbolFlag::Template) != 0)
		{
			return std::unexpected(Block("The callee is not a known plain function"));
		}

		// Same-document definition with a single `return expr;` body.
		auto definition = symbols.decl_node[callee];
		while (definition < soa.size() && soa.Kind(definition) != GrammarKind::FunctionDefinition)
		{
			if (definition == 0 || soa.Parent(definition) == definition)
			{
				break;
			}

			definition = soa.Parent(definition);
		}

		if (definition >= soa.size() || soa.Kind(definition) != GrammarKind::FunctionDefinition)
		{
			return std::unexpected(Block("Only calls to defined functions can be inlined"));
		}

		NodeId body = kNone;
		for (const auto child : model.ChildrenOf(definition))
		{
			if (soa.Kind(child) == GrammarKind::CompoundStatement)
			{
				body = child;
				break;
			}
		}

		NodeId returned        = kNone;
		std::size_t statements = 0;
		if (body != kNone)
		{
			for (const auto child : model.ChildrenOf(body))
			{
				if (stop.stop_requested())
				{
					return std::unexpected(
						RefactoringError {RefactoringErrorCode::Cancelled, "Request cancelled"});
				}

				// Trivia and nested scopes never appear as direct children;
				// every other statement shape is unsupported.
				if (soa.Kind(child) == GrammarKind::ReturnStatement)
				{
					returned = child;
				}

				++statements;
			}
		}

		if (returned == kNone || statements != 1)
		{
			return std::unexpected(Block("Only functions with a single return can be inlined"));
		}

		const auto body_range = context->NodeRange(returned);
		NodeId value          = InvalidNode;
		for (const auto child : model.ChildrenOf(returned))
		{
			if (stop.stop_requested())
			{
				return std::unexpected(
					RefactoringError {RefactoringErrorCode::Cancelled, "Request cancelled"});
			}

			if (child == returned)
			{
				continue;
			}

			if (value != InvalidNode)
			{
				return std::unexpected(Block("Only functions with a single return can be inlined"));
			}

			value = child;
		}

		if (value == InvalidNode)
		{
			return std::unexpected(Block("Only functions with a single return can be inlined"));
		}

		// Parameters of the callee in declaration order: Parameter symbols
		// owned by the definition's function scope.
		struct Param
		{
			std::string_view spelling;
			SymbolId symbol = kNone;
		};

		const ScopeId callee_scope = model.ScopeOfNode(definition);
		std::vector<Param> params;
		for (SymbolId i = 0; i < symbols.Size(); ++i)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(
					RefactoringError {RefactoringErrorCode::Cancelled, "Request cancelled"});
			}

			if (symbols.kind[i] != SymbolKind::Parameter || symbols.scope[i] != callee_scope)
			{
				continue;
			}

			const auto decl = symbols.decl_token[i];
			if (decl >= tree.Tokens().size())
			{
				return std::unexpected(Block("Only named parameters can be inlined"));
			}

			params.push_back({tree.Text(tree.Tokens()[decl]), i});
		}

		std::ranges::sort(params,[&](const Param& a, const Param& b)
			{
				return symbols.decl_token[a.symbol] < symbols.decl_token[b.symbol];
		});
		for (std::size_t i = 0; i < params.size(); ++i)
		{
			for (std::size_t j = i + 1; j < params.size(); ++j)
			{
				if (params[i].spelling == params[j].spelling)
				{
					return std::unexpected(Block("Duplicate parameter names are not supported"));
				}
			}
		}

		// The body must read only its parameters: anything else (globals,
		// statics, captures of the caller scope) would rebind at the call site.
		std::vector<PureRead> reads;
		if (!PureInputs(*context, value, reads))
		{
			return std::unexpected(Block("Only pure function bodies can be inlined"));
		}

		for (const auto& read : reads)
		{
			bool parameter = false;
			for (const auto& param : params)
			{
				if (read.symbol == param.symbol)
				{
					parameter = true;
					break;
				}
			}

			if (!parameter)
			{
				return std::unexpected(
					Block("Only functions of their own parameters can be inlined"));
			}
		}

		// Recursion would inline one level and change the call graph.
		{
			const auto callee_name = model.Names().Text(symbols.name[callee]);
			const auto body_first  = soa.FirstToken(body);
			const auto body_end =
				std::min<std::uint32_t>(body_first + soa.TokenCount(body),
				static_cast<std::uint32_t>(tree.Tokens().size()));
			for (auto i = body_first; i < body_end; ++i)
			{
				if (stop.stop_requested())
				{
					return std::unexpected(
						RefactoringError {RefactoringErrorCode::Cancelled, "Request cancelled"});
				}

				if (tree.Tokens()[i].kind == TokenKind::Identifier &&
					tree.Text(tree.Tokens()[i]) == callee_name)
				{
					return std::unexpected(Block("Recursive functions cannot be inlined"));
				}
			}
		}

		// Arguments must match positionally and be pure (inlining duplicates
		// their evaluation order into the body).
		std::vector<std::string> arguments;
		for (std::size_t k = 1; k < call_kids.size(); ++k)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(
					RefactoringError {RefactoringErrorCode::Cancelled, "Request cancelled"});
			}

			std::vector<PureRead> argument_reads;
			if (!PureInputs(*context, call_kids[k], argument_reads))
			{
				return std::unexpected(Block("Only calls with pure arguments can be inlined"));
			}

			const auto argument_range = context->NodeRange(call_kids[k]);
			arguments.push_back(
				std::string(tree.Source().substr(argument_range.offset, argument_range.length)));
		}

		if (arguments.size() != params.size())
		{
			return std::unexpected(Block("Argument and parameter counts differ"));
		}

		// Both the call site and the moved body must be macro-free.
		{
			NodeId caller = call;
			while (caller < soa.size() && soa.Kind(caller) != GrammarKind::FunctionDefinition)
			{
				if (caller == 0 || soa.Parent(caller) == caller)
				{
					break;
				}

				caller = soa.Parent(caller);
			}

			for (const auto scope_function :
				{
					caller, definition
			})
			{
				if (auto macros = MacroFreeFunction(*context, scope_function); !macros)
				{
					return std::unexpected(macros.error());
				}
			}
		}

		// Substitute parameters with parenthesized arguments over the raw body
		// text (whitespace and comments preserved); the whole replacement is
		// parenthesized so precedence never changes.
		const auto value_range = context->NodeRange(value);
		std::string inlined    = "(";
		std::size_t cursor     = value_range.offset;
		const auto value_end   = value_range.offset + value_range.length;
		const auto& tokens     = tree.Tokens();
		for (std::uint32_t i = soa.FirstToken(value);
			i < soa.FirstToken(value) + soa.TokenCount(value) && i < tokens.size(); ++i)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(
					RefactoringError {RefactoringErrorCode::Cancelled, "Request cancelled"});
			}

			if (tokens[i].kind != TokenKind::Identifier || tokens[i].tok != Tok::None)
			{
				continue;
			}

			const auto resolved = model.ResolveToken(i);
			for (std::size_t p = 0; p < params.size(); ++p)
			{
				if (resolved == params[p].symbol && tree.Text(tokens[i]) == params[p].spelling)
				{
					inlined.append(tree.Source().substr(cursor, tokens[i].offset - cursor));
					inlined += "(" + arguments[p] + ")";
					cursor = tokens[i].offset + tokens[i].length;
					break;
				}
			}
		}

		inlined.append(tree.Source().substr(cursor, value_end - cursor));
		inlined += ")";

		const auto call_range = context->NodeRange(call);
		return ValidateSyntax(snapshot,
			Plan(*context, "Inline function '" +
			std::string(
			model.Names().Text(symbols.name[callee])) +
			"'",
			{
				{
					call_range.offset, call_range.length,
					std::string(tree.Source().substr(call_range.offset,
					call_range.length)),
					std::move(inlined)
		}}),
			stop);
	}

	std::expected<RefactoringPlan, RefactoringError> RefactoringService::ExtractMethod(
		const AnalysisSnapshot& snapshot,
		DocumentId document,
		std::size_t offset,
		std::size_t length,
		std::string_view new_name,
		std::stop_token stop)
	{
		if (!refactor_detail::ValidName(new_name))
		{
			return std::unexpected(
				RefactoringError {RefactoringErrorCode::InvalidName, "Invalid C++ identifier"});
		}

		auto context = Context(snapshot, document, stop);
		if (!context)
		{
			return std::unexpected(context.error());
		}

		const auto& tree    = context->Syntax();
		const auto& soa     = tree.NodesSoA();
		const auto& model   = context->Semantic();
		const auto& symbols = model.Symbols();
		const auto node     = ExactNode(*context, offset, length);
		std::vector<PureRead> reads;
		if (node == InvalidNode ||!PureInputs(*context, node, reads) ||
			!OrdinaryFunction(*context, node))
		{
			return std::unexpected(
				Block("Method extraction supports a complete pure scalar return"));
		}

		const auto statement = soa.Parent(node);
		if (statement >= soa.size() || soa.Kind(statement) != GrammarKind::ReturnStatement ||
			soa.Parent(statement) >= soa.size() ||
			soa.Kind(soa.Parent(statement)) != GrammarKind::CompoundStatement)
		{
			return std::unexpected(Block("A standalone return expression is required"));
		}

		// Nearest enclosing class; free functions take ExtractFunction.
		NodeId class_node = node;
		while (class_node < soa.size() && soa.Kind(class_node) != GrammarKind::RecordDefinition)
		{
			if (class_node == 0 || soa.Parent(class_node) == class_node)
			{
				break;
			}

			class_node = soa.Parent(class_node);
		}

		if (class_node >= soa.size() || soa.Kind(class_node) != GrammarKind::RecordDefinition)
		{
			return std::unexpected(Block("Only expressions in member functions can be extracted"));
		}

		SymbolId owner = kNone;
		for (SymbolId i = 0; i < symbols.Size(); ++i)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(
					RefactoringError {RefactoringErrorCode::Cancelled, "Request cancelled"});
			}

			if (symbols.kind[i] == SymbolKind::Class && symbols.decl_node[i] == class_node)
			{
				owner = i;
				break;
			}
		}

		if (owner == kNone ||(symbols.flags[owner] & SymbolFlag::Template) != 0)
		{
			return std::unexpected(
				Block("Only expressions in non-template named classes can be extracted"));
		}

		for (auto ancestor = class_node;
			ancestor < soa.size() && ancestor != soa.Parent(ancestor); ancestor = soa.Parent(ancestor))
		{
			if (stop.stop_requested())
			{
				return std::unexpected(
					RefactoringError {RefactoringErrorCode::Cancelled, "Request cancelled"});
			}

			if (soa.Kind(ancestor) == GrammarKind::TemplateDeclaration)
			{
				return std::unexpected(
					Block("Only expressions in non-template named classes can be extracted"));
			}

			if (ancestor == 0)
			{
				break;
			}
		}

		const ScopeId class_scope = symbols.member_scope[owner];
		if (class_scope == kNone)
		{
			return std::unexpected(
				Block("Only expressions in non-template named classes can be extracted"));
		}

		// Reads of this class stay direct (instance method when any is
		// non-static); everything else becomes a parameter.
		bool needs_this = false;
		std::vector<PureRead> external;
		for (const auto& read : reads)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(
					RefactoringError {RefactoringErrorCode::Cancelled, "Request cancelled"});
			}

			if (read.symbol < symbols.Size() && symbols.scope[read.symbol] == class_scope)
			{
				if ((symbols.flags[read.symbol] & SymbolFlag::Static) == 0)
				{
					needs_this = true;
				}

				continue;
			}

			external.push_back(read);
		}

		// Instance state is unavailable in static member functions.
		if (needs_this)
		{
			NodeId enclosing = node;
			while (enclosing < soa.size() && soa.Kind(enclosing) != GrammarKind::FunctionDefinition)
			{
				if (enclosing == 0 || soa.Parent(enclosing) == enclosing)
				{
					break;
				}

				enclosing = soa.Parent(enclosing);
			}

			for (SymbolId i = 0; i < symbols.Size(); ++i)
			{
				if (symbols.kind[i] == SymbolKind::Function && symbols.decl_node[i] == enclosing &&
					(symbols.flags[i] & SymbolFlag::Static) != 0)
				{
					return std::unexpected(
						Block("Instance members cannot be used from a static function"));
				}
			}
		}

		// The call site lives in the enclosing member function and the new
		// method is inserted in the class body: both ranges must be macro-free.
		NodeId enclosing = node;
		while (enclosing < soa.size() && soa.Kind(enclosing) != GrammarKind::FunctionDefinition)
		{
			if (enclosing == 0 || soa.Parent(enclosing) == enclosing)
			{
				break;
			}

			enclosing = soa.Parent(enclosing);
		}

		if (auto macros = MacroFreeFunction(*context, enclosing); !macros)
		{
			return std::unexpected(macros.error());
		}

		const auto class_range = context->NodeRange(class_node);
		for (const auto& directive : tree.Directives())
		{
			if (directive.offset < class_range.offset + class_range.length &&
				directive.offset + directive.length > class_range.offset)
			{
				return std::unexpected(
					Block("Preprocessor context is not supported for this operation"));
			}
		}

		auto params_result = BuildParams(*context, external);
		if (!params_result)
		{
			return std::unexpected(params_result.error());
		}

		const auto wanted = model.Names().Find(new_name);
		if (wanted != kNone && model.LookupMember(owner, wanted) != kNone)
		{
			return std::unexpected(
				RefactoringError {RefactoringErrorCode::NameCollision,
					"The class already declares the new name"});
		}

		for (const auto& token : tree.Tokens())
		{
			if (token.kind == TokenKind::Identifier && tree.Text(token) == new_name)
			{
				return std::unexpected(
					RefactoringError {RefactoringErrorCode::NameCollision,
						"The new name already occurs in this document"});
			}
		}

		const auto& params = *params_result;
		std::string signature;
		std::string arguments;
		for (std::size_t i = 0; i < params.size(); ++i)
		{
			if (i != 0)
			{
				signature += ", ";
				arguments += ", ";
			}

			signature += params[i].type + " " + std::string(params[i].name);
			arguments += params[i].value;
		}

		const auto expression = std::string(tree.Source().substr(offset, length));
		const auto type       = context->Types().Spell(context->Types().NodeType(node));
		const auto last       = soa.FirstToken(class_node) + soa.TokenCount(class_node);
		std::string definition;
		if (last == 0 || last > tree.Tokens().size())
		{
			return std::unexpected(Block("The enclosing class is malformed"));
		}

		// Pure reads are const-compatible, so instance methods are always
		// const: callable from const and non-const member functions alike.
		definition += std::string(new_name) + "(" + signature + ")";
		if (needs_this)
		{
			definition = " " + type + " " + definition + " const { return " + expression + "; } ";
		}
		else
		{
			definition =
				" static " + type + " " + definition + " { return " + expression + "; } ";
		}

		const auto& closing = tree.Tokens()[last - 1];
		const std::string call =
			needs_this ? "this->" + std::string(new_name) + "(" + arguments + ")"
		: std::string(new_name) + "(" + arguments + ")";
		return ValidateSyntax(snapshot,
			Plan(*context, "Extract method '" + std::string(new_name) + "'",
			{
				{
					closing.offset, 0, {}, std::move(definition)
			},
				{offset, length, expression, call}}),
			stop);
	}

	RefactoringError SignatureError(RefactoringErrorCode code, std::string message)
	{
		return {code, std::move(message)};
	}

	// Splits the `( ... )` range into top-level comma parts (tracking
	// `(){}[]` nesting; `<>` needs name resolution and stays unparsed).
	// Returns token ranges [start, end) including trivia.
	std::vector<std::pair<std::uint32_t, std::uint32_t>> SplitParams(
		const ParseTree& tree,
		std::uint32_t open,
		std::uint32_t close)
	{
		std::vector<std::pair<std::uint32_t, std::uint32_t>> parts;
		const auto& tokens = tree.Tokens();
		std::size_t depth  = 0;
		auto start         = open + 1;
		for (auto i = open + 1; i < close; ++i)
		{
			const auto tok = tokens[i].tok;
			if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace)
			{
				++depth;
			}
			else if (tok == Tok::RParen || tok == Tok::RBracket || tok == Tok::RBrace)
			{
				depth = depth == 0 ? 0 : depth - 1;
			}
			else if (tok == Tok::Comma && depth == 0)
			{
				parts.emplace_back(start, i);
				start = i + 1;
			}
		}

		parts.emplace_back(start, close);
		return parts;
	}

	// Last depth-0 plain identifier before a depth-0 `=` (or the part end):
	// the parameter name. None for `...`, abstract or complex declarators.
	std::uint32_t PartName(const ParseTree& tree, std::uint32_t start, std::uint32_t end)
	{
		const auto& tokens = tree.Tokens();
		std::uint32_t name = kNone;
		std::size_t depth  = 0;
		for (auto i = start; i < end; ++i)
		{
			const auto tok = tokens[i].tok;
			if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace)
			{
				++depth;
			}
			else if (tok == Tok::RParen || tok == Tok::RBracket || tok == Tok::RBrace)
			{
				depth = depth == 0 ? 0 : depth - 1;
			}
			else if (depth == 0 && tok == Tok::Eq)
			{
				break;
			}
			else if (depth == 0 && tokens[i].kind == TokenKind::Identifier &&
				tok == Tok::None)
			{
				name = i;
			}
		}

		return name;
	}

	NodeId InnermostNode(const ParseTree& tree, std::uint32_t token)
	{
		// Smallest covering range wins: node indices are creation order, not
		// nesting depth (a binary operator node is created after its left
		// operand), so the highest index is not always the innermost.
		const auto& soa        = tree.NodesSoA();
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

	bool Trivia(const Token& token)
	{
		return token.kind == TokenKind::Whitespace || token.kind == TokenKind::LineComment ||
			token.kind == TokenKind::BlockComment;
	}

	// `( ... )` following the function name at `name_token`: scans forward
	// for `(` (aborting on statement boundaries), then token-level matching.
	std::expected<std::pair<std::uint32_t, std::uint32_t>, RefactoringError> ParamParens(
		const ParseTree& tree,
		std::uint32_t name_token)
	{
		const auto& tokens = tree.Tokens();
		auto open          = name_token + 1;
		while (open < tokens.size() && tokens[open].tok != Tok::LParen)
		{
			if (tokens[open].tok == Tok::Semi || tokens[open].tok == Tok::LBrace ||
				tokens[open].tok == Tok::RBrace || tokens[open].tok == Tok::Eq)
			{
				return std::unexpected(SignatureError(
					RefactoringErrorCode::IncompleteAnalysis, "Cannot locate the parameter list"));
			}

			++open;
		}

		if (open >= tokens.size())
		{
			return std::unexpected(SignatureError(
				RefactoringErrorCode::IncompleteAnalysis, "Cannot locate the parameter list"));
		}

		std::size_t depth = 0;
		for (auto i = open; i < tokens.size(); ++i)
		{
			if (tokens[i].tok == Tok::LParen)
			{
				++depth;
			}
			else if (tokens[i].tok == Tok::RParen && --depth == 0)
			{
				return std::pair<std::uint32_t, std::uint32_t> {open, i};
			}
		}

		return std::unexpected(SignatureError(
			RefactoringErrorCode::IncompleteAnalysis, "Cannot locate the parameter list"));
	}

	const RefactoredSource* PreviewFor(
		const std::vector<RefactoredSource>& preview,
		DocumentId document)
	{
		for (const auto& refactored : preview)
		{
			if (refactored.document == document)
			{
				return &refactored;
			}
		}

		return nullptr;
	}

	std::expected<void, RefactoringError> VerifyDocPreprocessing(
		const AnalysisSnapshot& snapshot,
		DocumentId document,
		std::string_view preview_source,
		std::stop_token stop)
	{
		const auto syntax = snapshot.Syntax(document);
		if (!syntax)
		{
			return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
				"Document is not available"));
		}

		if (syntax->Directives().empty() && snapshot.Options(document).Macros().empty())
		{
			return {};
		}

		return refactor_detail::VerifyPreprocessing(snapshot, document, preview_source, stop);
	}

	// Calls are verified against the tree instead of trusting the index role:
	// the callee token must sit in the callee child of a CallExpression.
	// Anything else naming the function (address-taken, decay, mislabeled
	// shapes) is a value use. Returns the argument children on success.
	std::expected<std::vector<std::uint32_t>, RefactoringError> CallArguments(
		const ParseTree& tree,
		const SemanticModel& model,
		std::uint32_t token)
	{
		const auto& soa = tree.NodesSoA();
		auto node       = InnermostNode(tree, token);
		while (node < soa.size() && soa.Kind(node) != GrammarKind::CallExpression)
		{
			if (node == 0 || soa.Parent(node) == node)
			{
				break;
			}

			node = soa.Parent(node);
		}

		if (node >= soa.size() || soa.Kind(node) != GrammarKind::CallExpression)
		{
			return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
				"A use is not a direct call"));
		}

		const auto kids = model.ChildrenOf(node);
		if (kids.empty() || token < soa.FirstToken(kids[0]) ||
			token >= soa.FirstToken(kids[0]) + soa.TokenCount(kids[0]))
		{
			return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
				"A use is not a direct call"));
		}

		return std::vector<std::uint32_t>(kids.begin() + 1, kids.end());
	}

	std::expected<RefactoringPlan, RefactoringError> RefactoringService::AddParameter(
		const AnalysisSnapshot& snapshot,
		DocumentId document,
		std::size_t offset,
		std::string_view type,
		std::string_view name,
		std::string_view default_value,
		std::stop_token stop)
	{
		if (!refactor_detail::ValidName(name))
		{
			return std::unexpected(SignatureError(
				RefactoringErrorCode::InvalidName, "Use a non-reserved ASCII C++ identifier"));
		}

		for (const auto text :
			{
				type, default_value
		})
		{
			if (text.empty() || text.find_first_of(";{}") != std::string_view::npos)
			{
				return std::unexpected(SignatureError(
					RefactoringErrorCode::InvalidName, "Invalid parameter type or default value"));
			}
		}

		auto resolved = refactor_detail::ResolveWorkspaceEntity(snapshot, document, offset, stop);
		if (!resolved)
		{
			return std::unexpected(resolved.error());
		}

		const auto& index  = resolved->index;
		const auto& entity = index->Entities()[resolved->entity];
		if (entity.kind != SymbolKind::Function)
		{
			return std::unexpected(SignatureError(RefactoringErrorCode::UnsupportedSymbol,
				"Only functions take parameters"));
		}

		struct Declaration
		{
			DocumentId document = InvalidDocument;
			std::uint32_t token = kNone;
		};

		std::vector<Declaration> declarations;
		int definitions = 0;
		for (const auto occurrence_index : entity.occurrences)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::Cancelled,
					"Request cancelled"));
			}

			if (occurrence_index >= index->Occurrences().size())
			{
				return std::unexpected(SignatureError(
					RefactoringErrorCode::IncompleteAnalysis, "The symbol index is inconsistent"));
			}

			const auto& indexed = index->Occurrences()[occurrence_index];
			if (indexed.resolution != OccurrenceResolution::Resolved)
			{
				return std::unexpected(SignatureError(
					RefactoringErrorCode::IncompleteAnalysis, "An occurrence is not resolved"));
			}

			if (indexed.role == OccurrenceRole::Declaration ||
				indexed.role == OccurrenceRole::Definition)
			{
				if (indexed.token == kNone)
				{
					return std::unexpected(
						SignatureError(RefactoringErrorCode::IncompleteAnalysis,
						"A declaration is not modeled"));
				}

				declarations.push_back({indexed.document, indexed.token});
				if (indexed.role == OccurrenceRole::Definition)
				{
					++definitions;
				}

				continue;
			}

			// Any other occurrence must be a call (defaulted parameters
			// preserve every call); anything else is a value use.
			if (indexed.token == kNone)
			{
				return std::unexpected(SignatureError(
					RefactoringErrorCode::IncompleteAnalysis, "An occurrence is not resolved"));
			}

			const auto call_tree  = snapshot.Syntax(indexed.document);
			const auto call_model = snapshot.Semantic(indexed.document);
			if (!call_tree ||!call_model ||!call_tree->Diagnostics().empty())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"Document has parse errors"));
			}

			const auto call = CallArguments(*call_tree, *call_model, indexed.token);
			if (!call)
			{
				return std::unexpected(SignatureError(
					RefactoringErrorCode::IncompleteAnalysis,
					"The function is used as a value and cannot change signature"));
			}
		}

		if (definitions > 1)
		{
			return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
				"Multiple definitions are not supported"));
		}

		std::map<DocumentId, std::vector<SourceEdit>> by_document;
		for (const auto& declaration : declarations)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::Cancelled,
					"Request cancelled"));
			}

			const auto tree = snapshot.Syntax(declaration.document);
			if (!tree ||!tree->Diagnostics().empty())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"Document has parse errors"));
			}

			const auto& tokens = tree->Tokens();
			if (declaration.token >= tokens.size() ||
				tokens[declaration.token].kind != TokenKind::Identifier ||
				tokens[declaration.token].tok != Tok::None)
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"A declaration is not modeled"));
			}

			auto parens = ParamParens(*tree, declaration.token);
			if (!parens)
			{
				return std::unexpected(parens.error());
			}

			const auto open  = parens->first;
			const auto close = parens->second;
			std::vector<std::uint32_t> contents;
			for (auto i = open + 1; i < close; ++i)
			{
				if (!Trivia(tokens[i]))
				{
					contents.push_back(i);
				}
			}

			const std::string spelling =
				std::string(type) + " " + std::string(name) + " = " + std::string(default_value);
			if (contents.empty())
			{
				by_document[declaration.document].push_back(
					{tokens[close].offset, 0, {}, spelling});
			}
			else if (contents.size() == 1 && tokens[contents[0]].tok == Tok::KwVoid)
			{
				by_document[declaration.document].push_back({tokens[contents[0]].offset,
						tokens[contents[0]].length, "void",
						spelling});
			}
			else
			{
				for (const auto content : contents)
				{
					if (tokens[content].kind == TokenKind::Identifier &&
						tokens[content].tok == Tok::None && tree->Text(tokens[content]) == name)
					{
						return std::unexpected(
							SignatureError(RefactoringErrorCode::NameCollision,
							"The function already has this parameter"));
					}
				}

				by_document[declaration.document].push_back(
					{tokens[close].offset, 0, {}, ", " + spelling});
			}
		}

		RefactoringPlan plan {"Add parameter " + entity.qualified_name, snapshot.Revision(), {}};
		for (auto& [affected, edits] : by_document)
		{
			plan.documents.push_back({affected, snapshot.Path(affected),
					snapshot.Version(affected), snapshot.Source(affected),
					std::move(edits), snapshot.Options(affected)});
		}

		auto preview = PreviewRefactoring(snapshot, plan, stop);
		if (!preview)
		{
			return std::unexpected(preview.error());
		}

		for (const auto& renamed : plan.documents)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::Cancelled,
					"Request cancelled"));
			}

			const auto refactored = PreviewFor(*preview, renamed.document);
			if (refactored == nullptr)
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::StaleSnapshot,
					"Workspace changed"));
			}

			const auto verified =
				VerifyDocPreprocessing(snapshot, renamed.document, refactored->source, stop);
			if (!verified)
			{
				return std::unexpected(verified.error());
			}
		}

		return ValidateSyntax(snapshot, std::move(plan), stop);
	}

	std::expected<RefactoringPlan, RefactoringError> RefactoringService::RemoveParameter(
		const AnalysisSnapshot& snapshot,
		DocumentId document,
		std::size_t offset,
		std::string_view name,
		std::stop_token stop)
	{
		if (!refactor_detail::ValidName(name))
		{
			return std::unexpected(SignatureError(
				RefactoringErrorCode::InvalidName, "Use a non-reserved ASCII C++ identifier"));
		}

		auto resolved = refactor_detail::ResolveWorkspaceEntity(snapshot, document, offset, stop);
		if (!resolved)
		{
			return std::unexpected(resolved.error());
		}

		const auto& index  = resolved->index;
		const auto& entity = index->Entities()[resolved->entity];
		if (entity.kind != SymbolKind::Function)
		{
			return std::unexpected(SignatureError(RefactoringErrorCode::UnsupportedSymbol,
				"Only functions take parameters"));
		}

		struct Declaration
		{
			DocumentId document = InvalidDocument;
			std::uint32_t token = kNone;
		};

		struct CallSite
		{
			DocumentId document = InvalidDocument;
			std::uint32_t token = kNone;
		};

		std::vector<Declaration> declarations;
		std::vector<CallSite> calls;
		int definitions = 0;
		for (const auto occurrence_index : entity.occurrences)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::Cancelled,
					"Request cancelled"));
			}

			if (occurrence_index >= index->Occurrences().size())
			{
				return std::unexpected(SignatureError(
					RefactoringErrorCode::IncompleteAnalysis, "The symbol index is inconsistent"));
			}

			const auto& indexed = index->Occurrences()[occurrence_index];
			if (indexed.resolution != OccurrenceResolution::Resolved)
			{
				return std::unexpected(SignatureError(
					RefactoringErrorCode::IncompleteAnalysis, "An occurrence is not resolved"));
			}

			if (indexed.role == OccurrenceRole::Declaration ||
				indexed.role == OccurrenceRole::Definition)
			{
				if (indexed.token == kNone)
				{
					return std::unexpected(
						SignatureError(RefactoringErrorCode::IncompleteAnalysis,
						"A declaration is not modeled"));
				}

				declarations.push_back({indexed.document, indexed.token});
				if (indexed.role == OccurrenceRole::Definition)
				{
					++definitions;
				}

				continue;
			}

			if (indexed.token == kNone)
			{
				return std::unexpected(SignatureError(
					RefactoringErrorCode::IncompleteAnalysis, "An occurrence is not resolved"));
			}

			calls.push_back({indexed.document, indexed.token});
		}

		if (definitions > 1)
		{
			return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
				"Multiple definitions are not supported"));
		}

		// Every edited declaration must agree on arity and position, and the
		// parameter must be unused everywhere (bodies keep no trace of it).
		std::size_t param_count = 0;
		std::size_t param_index = 0;
		bool first              = true;
		struct Removal
		{
			DocumentId document = InvalidDocument;
			std::uint32_t start = kNone;
			std::uint32_t end   = kNone;
		};

		std::vector<Removal> removals;
		for (const auto& declaration : declarations)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::Cancelled,
					"Request cancelled"));
			}

			const auto tree = snapshot.Syntax(declaration.document);
			if (!tree ||!tree->Diagnostics().empty())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"Document has parse errors"));
			}

			const auto& tokens = tree->Tokens();
			if (declaration.token >= tokens.size())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"A declaration is not modeled"));
			}

			auto parens = ParamParens(*tree, declaration.token);
			if (!parens)
			{
				return std::unexpected(parens.error());
			}

			const auto parts  = SplitParams(*tree, parens->first, parens->second);
			std::size_t found = parts.size();
			for (std::size_t p = 0; p < parts.size(); ++p)
			{
				const auto candidate = PartName(*tree, parts[p].first, parts[p].second);
				if (candidate != kNone && tree->Text(tokens[candidate]) == name)
				{
					if (found != parts.size())
					{
						return std::unexpected(
							SignatureError(RefactoringErrorCode::IncompleteAnalysis,
							"Duplicate parameter names are not supported"));
					}

					found = p;
				}
			}

			if (found == parts.size())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::NameCollision,
					"No such parameter in this declaration"));
			}

			if (first)
			{
				param_count = parts.size();
				param_index = found;
				first       = false;
			}
			else if (parts.size() != param_count || found != param_index)
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"Declarations disagree on parameters"));
			}

			// The parameter symbol must have no uses (its declaration is not a
			// use). Header declarations own no parameter symbols, so fall back
			// to a textual use scan of their parameter list instead.
			const auto model    = snapshot.Semantic(declaration.document);
			const auto& symbols = model->Symbols();
			SymbolId symbol     = kNone;
			for (SymbolId i = 0; i < symbols.Size(); ++i)
			{
				if (symbols.kind[i] == SymbolKind::Parameter &&
					symbols.decl_token[i] >= parts[found].first &&
					symbols.decl_token[i] < parts[found].second)
				{
					symbol = i;
					break;
				}
			}

			if (symbol == kNone)
			{
				const auto open  = parens->first;
				const auto close = parens->second;
				for (auto i = open + 1; i < close; ++i)
				{
					if (stop.stop_requested())
					{
						return std::unexpected(
							SignatureError(RefactoringErrorCode::Cancelled, "Request cancelled"));
					}

					if (i >= parts[found].first && i < parts[found].second)
					{
						continue;
					}

					if (tokens[i].kind == TokenKind::Identifier &&
						tokens[i].tok == Tok::None && tree->Text(tokens[i]) == name)
					{
						return std::unexpected(
							SignatureError(RefactoringErrorCode::IncompleteAnalysis,
							"The parameter is used and cannot be removed"));
					}
				}
			}
			else
			{
				for (std::size_t i = 0; i < model->Refs().token.size(); ++i)
				{
					if (model->Refs().target[i] == symbol)
					{
						return std::unexpected(
							SignatureError(RefactoringErrorCode::IncompleteAnalysis,
							"The parameter is used and cannot be removed"));
					}
				}
			}

			const auto[part_start, part_end] = parts[found];
			if (parts.size() == 1)
			{
				removals.push_back({declaration.document, part_start, part_end});
			}
			else if (found > 0)
			{
				removals.push_back({declaration.document, parts[found - 1].second, part_end});
			}
			else
			{
				removals.push_back({declaration.document, part_start, parts[found + 1].first});
			}
		}

		std::map<DocumentId, std::vector<SourceEdit>> by_document;
		for (const auto& removal : removals)
		{
			const auto tree = snapshot.Syntax(removal.document);
			if (!tree)
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"Document is not available"));
			}

			const auto& tokens = tree->Tokens();
			if (removal.end <= removal.start || removal.end > tokens.size())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"A parameter is not modeled"));
			}

			const auto source = snapshot.Source(removal.document);
			const auto begin  = tokens[removal.start].offset;
			const auto& last  = tokens[removal.end - 1];
			const auto finish = last.offset + last.length;
			by_document[removal.document].push_back(
				{begin, finish - begin, std::string(source->substr(begin, finish - begin)), {}});
		}

		// Calls drop the argument at the removed position; calls omitting it
		// (fewer arguments via defaults) stay untouched.
		for (const auto& call : calls)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::Cancelled,
					"Request cancelled"));
			}

			const auto tree = snapshot.Syntax(call.document);
			if (!tree ||!tree->Diagnostics().empty())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"Document has parse errors"));
			}

			const auto& tokens = tree->Tokens();
			if (call.token >= tokens.size())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"A call is not modeled"));
			}

			const auto model = snapshot.Semantic(call.document);
			if (!model)
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"Document is not available"));
			}

			auto arguments = CallArguments(*tree, *model, call.token);
			if (!arguments)
			{
				return std::unexpected(SignatureError(
					RefactoringErrorCode::IncompleteAnalysis,
					"The function is used as a value and cannot change signature"));
			}

			const auto& kids = *arguments;
			if (kids.size() <= param_index)
			{
				continue;
			}

			const auto& soa      = tree->NodesSoA();
			const auto argument  = kids[param_index];
			const auto arg_first = soa.FirstToken(argument);
			const auto arg_end   = arg_first + soa.TokenCount(argument);
			if (arg_end <= arg_first || arg_end > tokens.size())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"A call is not modeled"));
			}

			std::uint32_t drop_start = arg_first;
			std::uint32_t drop_end   = arg_end;
			if (kids.size() == 1)
			{
				// Sole argument: keep the empty pair.
			}
			else if (param_index > 0)
			{
				const auto previous = kids[param_index - 1];
				drop_start =
					soa.FirstToken(previous) + soa.TokenCount(previous);
			}
			else
			{
				const auto next = kids[1];
				drop_end        = soa.FirstToken(next);
			}

			if (drop_end <= drop_start || drop_end > tokens.size())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"A call is not modeled"));
			}

			const auto source = snapshot.Source(call.document);
			const auto begin  = tokens[drop_start].offset;
			const auto& last  = tokens[drop_end - 1];
			const auto finish = last.offset + last.length;
			by_document[call.document].push_back(
				{begin, finish - begin, std::string(source->substr(begin, finish - begin)), {}});
		}

		RefactoringPlan plan {"Remove parameter " + entity.qualified_name, snapshot.Revision(), {}};
		for (auto& [affected, edits] : by_document)
		{
			plan.documents.push_back({affected, snapshot.Path(affected),
					snapshot.Version(affected), snapshot.Source(affected),
					std::move(edits), snapshot.Options(affected)});
		}

		auto preview = PreviewRefactoring(snapshot, plan, stop);
		if (!preview)
		{
			return std::unexpected(preview.error());
		}

		for (const auto& renamed : plan.documents)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::Cancelled,
					"Request cancelled"));
			}

			const auto refactored = PreviewFor(*preview, renamed.document);
			if (refactored == nullptr)
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::StaleSnapshot,
					"Workspace changed"));
			}

			const auto verified =
				VerifyDocPreprocessing(snapshot, renamed.document, refactored->source, stop);
			if (!verified)
			{
				return std::unexpected(verified.error());
			}
		}

		return ValidateSyntax(snapshot, std::move(plan), stop);
	}

	bool NamespaceSegment(std::string_view text)
	{
		if (text.empty())
		{
			return false;
		}

		const auto alpha =[](char c)
		{
			return (c >= 'a' && c <= 'z') ||(c >= 'A' && c <= 'Z') || c == '_';
		};
		if (!alpha(text.front()))
		{
			return false;
		}

		for (char c : text)
		{
			if (!alpha(c) && !(c >= '0' && c <= '9'))
			{
				return false;
			}
		}

		return true;
	}

	std::expected<RefactoringPlan, RefactoringError> RefactoringService::MoveToNamespace(
		const AnalysisSnapshot& snapshot,
		DocumentId document,
		std::size_t offset,
		std::string_view target_namespace,
		std::stop_token stop)
	{
		// `ns` or `a::b` made of plain identifiers.
		{
			std::size_t part = 0;
			bool ok          = !target_namespace.empty();
			while (ok)
			{
				const auto next = target_namespace.find("::", part);
				const auto piece = next == std::string_view::npos
				? target_namespace.substr(part)
				: target_namespace.substr(part, next - part);
				ok = NamespaceSegment(piece);
				if (next == std::string_view::npos)
				{
					break;
				}

				part = next + 2;
			}

			if (!ok)
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::InvalidName,
					"Invalid namespace name"));
			}
		}

		auto resolved = refactor_detail::ResolveWorkspaceEntity(snapshot, document, offset, stop);
		if (!resolved)
		{
			return std::unexpected(resolved.error());
		}

		const auto& index  = resolved->index;
		const auto& entity = index->Entities()[resolved->entity];
		if (entity.kind != SymbolKind::Function)
		{
			return std::unexpected(SignatureError(RefactoringErrorCode::UnsupportedSymbol,
				"Only free functions can be moved"));
		}

		bool namespace_exists = false;
		for (const auto owner : index->Named(std::string(target_namespace)))
		{
			if (stop.stop_requested())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::Cancelled,
					"Request cancelled"));
			}

			if (owner < index->Entities().size() &&
				index->Entities()[owner].kind == SymbolKind::Namespace)
			{
				namespace_exists = true;
				break;
			}
		}

		if (!namespace_exists)
		{
			return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
				"The target namespace is not indexed"));
		}

		if (target_namespace.find("::") != std::string_view::npos)
		{
			return std::unexpected(
				SignatureError(RefactoringErrorCode::IncompleteAnalysis,
				"Nested target namespaces are not supported yet"));
		}

		const auto qualifier_separator = entity.qualified_name.rfind("::");
		const std::string unqualified =
			qualifier_separator == std::string::npos
		? entity.qualified_name
		: entity.qualified_name.substr(qualifier_separator + 2);

		struct Occurrence
		{
			DocumentId document = InvalidDocument;
			std::uint32_t token = kNone;
			bool definition     = false;
		};

		std::vector<Occurrence> declarations;
		std::vector<Occurrence> calls;
		for (const auto occurrence_index : entity.occurrences)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::Cancelled,
					"Request cancelled"));
			}

			if (occurrence_index >= index->Occurrences().size())
			{
				return std::unexpected(SignatureError(
					RefactoringErrorCode::IncompleteAnalysis, "The symbol index is inconsistent"));
			}

			const auto& indexed = index->Occurrences()[occurrence_index];
			if (indexed.resolution != OccurrenceResolution::Resolved)
			{
				return std::unexpected(SignatureError(
					RefactoringErrorCode::IncompleteAnalysis, "An occurrence is not resolved"));
			}

			if (indexed.document != document)
			{
				return std::unexpected(
					SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"The function is used outside this document"));
			}

			if (indexed.token == kNone)
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"An occurrence is not modeled"));
			}

			if (indexed.role == OccurrenceRole::Declaration ||
				indexed.role == OccurrenceRole::Definition)
			{
				declarations.push_back(
					{indexed.document, indexed.token, indexed.role == OccurrenceRole::Definition});
				continue;
			}

			const auto tree  = snapshot.Syntax(indexed.document);
			const auto model = snapshot.Semantic(indexed.document);
			if (!tree ||!model ||!tree->Diagnostics().empty())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"Document has parse errors"));
			}

			if (!CallArguments(*tree, *model, indexed.token))
			{
				return std::unexpected(
					SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"The function is used as a value and cannot be moved"));
			}

			calls.push_back({indexed.document, indexed.token, false});
		}

		int definitions = 0;
		for (const auto& declaration : declarations)
		{
			if (declaration.definition)
			{
				++definitions;
			}
		}

		if (definitions != 1)
		{
			return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
				"Exactly one definition is required"));
		}

		const auto tree = snapshot.Syntax(document);
		if (!tree ||!tree->Diagnostics().empty())
		{
			return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
				"Document has parse errors"));
		}

		const auto& tokens = tree->Tokens();
		const auto& soa    = tree->NodesSoA();
		const auto source  = snapshot.Source(document);

		// The definition must sit at translation-unit scope with a plain name:
		// namespace blocks change linkage and scope when crossed, and
		// already-qualified definitions belong elsewhere already.
		for (const auto& declaration : declarations)
		{
			auto node = InnermostNode(*tree, declaration.token);
			while (node < soa.size() && soa.Kind(node) != GrammarKind::FunctionDefinition)
			{
				if (node == 0 || soa.Parent(node) == node)
				{
					break;
				}

				node = soa.Parent(node);
			}

			if (node >= soa.size() || soa.Kind(node) != GrammarKind::FunctionDefinition)
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"Only function definitions can be moved"));
			}

			for (auto ancestor = soa.Parent(node);
				ancestor < soa.size() && ancestor != soa.Parent(ancestor);
				ancestor = soa.Parent(ancestor))
			{
				if (soa.Kind(ancestor) == GrammarKind::NamespaceDefinition)
				{
					return std::unexpected(
						SignatureError(RefactoringErrorCode::IncompleteAnalysis,
						"Definitions inside namespace blocks are not supported"));
				}

				if (ancestor == 0 || soa.Kind(ancestor) == GrammarKind::TranslationUnit)
				{
					break;
				}
			}

			auto previous = declaration.token;
			while (previous > 0 && Trivia(tokens[previous - 1]))
			{
				--previous;
			}

			if (previous < declaration.token && tokens[previous].tok == Tok::ColonColon)
			{
				return std::unexpected(
					SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"Qualified definitions are not supported"));
			}
		}

		// A same-named function in the target namespace would collide.
		{
			const std::string qualified = std::string(target_namespace) + "::" + unqualified;
			for (const auto existing : index->Named(qualified))
			{
				if (stop.stop_requested())
				{
					return std::unexpected(SignatureError(RefactoringErrorCode::Cancelled,
						"Request cancelled"));
				}

				if (existing < index->Entities().size() &&
					index->Entities()[existing].kind == SymbolKind::Function)
				{
					return std::unexpected(
						SignatureError(RefactoringErrorCode::NameCollision,
						"The target namespace already declares this function"));
				}
			}
		}

		// Overloads and same-named members would make call attribution
		// ambiguous: exactly one function may carry this name here.
		{
			const auto model = snapshot.Semantic(document);
			if (!model)
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"Document is not available"));
			}

			const auto& symbols    = model->Symbols();
			std::size_t same_named = 0;
			for (SymbolId i = 0; i < symbols.Size(); ++i)
			{
				if (stop.stop_requested())
				{
					return std::unexpected(SignatureError(RefactoringErrorCode::Cancelled,
						"Request cancelled"));
				}

				if (symbols.kind[i] == SymbolKind::Function &&
					model->Names().Text(symbols.name[i]) == unqualified)
				{
					++same_named;
				}
			}

			if (same_named != 1)
			{
				return std::unexpected(
					SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"Overloads and same-named members are not supported"));
			}
		}

		std::vector<SourceEdit> edits;
		for (const auto& declaration : declarations)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::Cancelled,
					"Request cancelled"));
			}

			const auto& name = tokens[declaration.token];
			edits.push_back({name.offset, 0, {}, std::string(target_namespace) + "::"});
		}

		const auto model = snapshot.Semantic(document);
		if (!model)
		{
			return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
				"Document is not available"));
		}

		// Outermost-first `::`-joined chain of enclosing namespaces; empty at
		// translation-unit scope or past anonymous ones.
		const auto namespace_chain =[&](ScopeId scope)
		{
			const auto& scopes = model->Scopes();
			std::string chain;
			for (auto current = scope;
				current < scopes.Size() && current != SemanticModel::TranslationUnitScope;
				current = scopes.parent[current])
			{
				if (scopes.kind[current] != ScopeKind::Namespace)
				{
					continue;
				}

				const auto owner = scopes.owner[current];
				if (owner >= model->Symbols().Size())
				{
					return std::string {};
				}

				const auto piece = model->Names().Text(model->Symbols().name[owner]);
				chain            = chain.empty() ? std::string(piece) : std::string(piece) + "::" + chain;
			}

			return chain;
		};

		const std::string want_qualified =
			std::string(target_namespace) + "::" + unqualified;
		const std::string nested_prefix = want_qualified + "::";
		for (const auto& call : calls)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::Cancelled,
					"Request cancelled"));
			}

			// Callee node and its token range: the rewrite covers qualifiers,
			// so spellings normalize instead of stacking.
			auto node = InnermostNode(*tree, call.token);
			while (node < soa.size() && soa.Kind(node) != GrammarKind::CallExpression)
			{
				if (node == 0 || soa.Parent(node) == node)
				{
					break;
				}

				node = soa.Parent(node);
			}

			if (node >= soa.size() || soa.Kind(node) != GrammarKind::CallExpression)
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"A call is not modeled"));
			}

			const auto callee_kids = model->ChildrenOf(node);
			if (callee_kids.empty())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"A call is not modeled"));
			}

			const auto callee       = callee_kids[0];
			const auto callee_first = soa.FirstToken(callee);
			const auto callee_end   = callee_first + soa.TokenCount(callee);
			if (callee_end <= callee_first || callee_end > tokens.size() ||
				call.token < callee_first || call.token >= callee_end)
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"A call is not modeled"));
			}

			std::string spelling;
			for (auto i = callee_first; i < callee_end; ++i)
			{
				if (!Trivia(tokens[i]))
				{
					spelling += tree->Text(tokens[i]);
				}
			}

			const bool bare    = (spelling == unqualified);
			const bool global  = (spelling == std::string("::") + unqualified);
			const bool already = (spelling == want_qualified);
			if (!bare && !global && !already)
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::IncompleteAnalysis,
					"A qualified call is not modeled"));
			}

			const auto use_scope    = model->ScopeOfNode(InnermostNode(*tree, call.token));
			const std::string chain = namespace_chain(use_scope);
			const bool inside =
				chain == target_namespace || chain.starts_with(nested_prefix);
			const auto& scopes     = model->Scopes();
			const auto& first      = tokens[callee_first];
			const auto& last       = tokens[callee_end - 1];
			const auto span_start  = first.offset;
			const auto span_length = last.offset + last.length - first.offset;
			const std::string span_text =
				std::string(tree->Source().substr(span_start, span_length));
			if (inside)
			{
				// `N::f` inside N would mean `N::N::f`: it cannot name this
				// entity, so its presence contradicts the index.
				if (!bare)
				{
					return std::unexpected(
						SignatureError(RefactoringErrorCode::IncompleteAnalysis,
						"A qualified call is not modeled"));
				}

				edits.push_back({span_start, span_length, span_text, unqualified});
				continue;
			}

			// Outside: a nested namespace also named like the target would
			// capture the qualifier instead of the global one.
			for (auto scope = use_scope;
				scope < scopes.Size() && scope != SemanticModel::TranslationUnitScope;
				scope = scopes.parent[scope])
			{
				if (scopes.kind[scope] != ScopeKind::Namespace)
				{
					continue;
				}

				const auto owner = scopes.owner[scope];
				if (owner < model->Symbols().Size() &&
					model->Names().Text(model->Symbols().name[owner]) == target_namespace)
				{
					return std::unexpected(
						SignatureError(RefactoringErrorCode::IncompleteAnalysis,
						"A shadowing namespace is not supported"));
				}
			}

			if (already)
			{
				continue;
			}

			edits.push_back({span_start, span_length, span_text, want_qualified});
		}

		RefactoringPlan plan {"Move " + entity.qualified_name + " to " +
				std::string(target_namespace),
			snapshot.Revision(), {}};
		plan.documents.push_back({document, snapshot.Path(document), snapshot.Version(document),
				source, std::move(edits), snapshot.Options(document)});

		auto preview = PreviewRefactoring(snapshot, plan, stop);
		if (!preview)
		{
			return std::unexpected(preview.error());
		}

		if (!tree->Directives().empty() ||!snapshot.Options(document).Macros().empty())
		{
			const auto verified =
				refactor_detail::VerifyPreprocessing(snapshot, document, preview->front().source, stop);
			if (!verified)
			{
				return std::unexpected(verified.error());
			}
		}

		// The moved definition legitimately gains the Qualified flag, so its
		// declaration is exempt from the untouched-code comparison (its uses
		// are proven separately below).
		SymbolId moved_symbol = kNone;
		{
			const auto before_model        = snapshot.Semantic(document);
			std::uint32_t definition_token = kNone;
			for (const auto& declaration : declarations)
			{
				if (declaration.definition)
				{
					definition_token = declaration.token;
				}
			}

			if (before_model && definition_token != kNone)
			{
				for (SymbolId i = 0; i < before_model->Symbols().Size(); ++i)
				{
					if (before_model->Symbols().kind[i] == SymbolKind::Function &&
						before_model->Symbols().decl_token[i] == definition_token)
					{
						moved_symbol = i;
						break;
					}
				}
			}
		}

		const std::vector<std::pair<DocumentId, SymbolId>> moved =
			moved_symbol == kNone ? std::vector<std::pair<DocumentId, SymbolId>> {}
		: std::vector<std::pair<DocumentId, SymbolId>> {
			{
				document,
					moved_symbol
		}};
		const auto validated = ValidateSyntax(snapshot, plan, stop, moved);
		if (!validated)
		{
			return std::unexpected(validated.error());
		}

		// Binding proof for the move: overloads were vetoed upfront, so
		// exactly one function carries this name; it must be qualified (the
		// out-of-line definition) and every non-declaration use must resolve
		// to it.
		{
			const auto rebound_tree =
				ParseTree::Parse(preview->front().source, snapshot.Options(document), stop);
			if (rebound_tree.Cancelled() || stop.stop_requested())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::Cancelled,
					"Request cancelled"));
			}

			if (!rebound_tree.Diagnostics().empty())
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::SemanticChange,
					"Move introduces parse errors"));
			}

			const auto rebound          = Binder::Bind(rebound_tree);
			const auto& rebound_symbols = rebound.Symbols();
			const auto& rebound_tokens  = rebound_tree.Tokens();
			SymbolId moved              = kNone;
			for (SymbolId i = 0; i < rebound_symbols.Size(); ++i)
			{
				if (stop.stop_requested())
				{
					return std::unexpected(SignatureError(RefactoringErrorCode::Cancelled,
						"Request cancelled"));
				}

				if (rebound_symbols.kind[i] != SymbolKind::Function ||
					rebound.Names().Text(rebound_symbols.name[i]) != unqualified)
				{
					continue;
				}

				if (moved != kNone)
				{
					return std::unexpected(SignatureError(
						RefactoringErrorCode::SemanticChange, "Move creates duplicate functions"));
				}

				moved = i;
			}

			if (moved == kNone ||
				(rebound_symbols.flags[moved] & SymbolFlag::Qualified) == 0)
			{
				return std::unexpected(SignatureError(RefactoringErrorCode::SemanticChange,
					"Move loses the function"));
			}

			std::unordered_set<std::uint32_t> declarations;
			for (SymbolId i = 0; i < rebound_symbols.Size(); ++i)
			{
				declarations.insert(rebound_symbols.decl_token[i]);
			}

			for (std::uint32_t i = 0; i < rebound_tokens.size(); ++i)
			{
				if (stop.stop_requested())
				{
					return std::unexpected(SignatureError(RefactoringErrorCode::Cancelled,
						"Request cancelled"));
				}

				if (rebound_tokens[i].kind != TokenKind::Identifier ||
					rebound_tokens[i].tok != Tok::None ||
					rebound_tree.Text(rebound_tokens[i]) != unqualified ||
					declarations.find(i) != declarations.end())
				{
					continue;
				}

				// Only same-named functions compete with the move (locals,
				// parameters and types keep their bindings: the move
				// introduces no local names and renames no types).
				const auto resolved = rebound.ResolveToken(i);
				if (resolved == kNone ||
					(rebound_symbols.kind[resolved] == SymbolKind::Function && resolved != moved))
				{
					return std::unexpected(
						SignatureError(RefactoringErrorCode::SemanticChange,
						"Move changes a use binding"));
				}
			}
		}

		return validated;
	}
} // namespace heimdall
