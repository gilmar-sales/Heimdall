#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Heimdall/LineTable.hpp>
#include <Heimdall/SemanticRules.hpp>

#include "detail/TokenView.hpp"

namespace heimdall
{

	namespace
	{

		using detail::TokenView;

		// Severity-explicit emitter (mirrors ControlFlowRules' Reporter): concept
		// well-formedness is ill-formed/no-diagnostic-required code, hence Error,
		// except redeclaration shape (Warning).
		struct Emitter
		{
			explicit Emitter(const SemanticModel& model) : mModel(model) {}

			void Add(
				RuleId rule,
				Severity severity,
				std::string_view code,
				std::string message,
				std::size_t offset,
				std::size_t length)
			{
				if (!mBuilt)
				{
					mLines.Build(mModel.Tree().Source());
					mBuilt = true;
				}

				const auto position = mLines.Lookup(offset);
				mOut.push_back(Diagnostic {rule, severity, std::string(code), std::move(message),
						offset, length, position.line, position.column, false, TextEdit {0, 0, std::string()}});
			}

			std::vector<Diagnostic> Take()
			{
				std::sort(mOut.begin(), mOut.end(),
					[](const Diagnostic& a, const Diagnostic& b)
					{
						return a.offset < b.offset;
					});
				return std::move(mOut);
			}

		private:
			const SemanticModel& mModel;
			LineTable mLines;
			bool mBuilt = false;
			std::vector<Diagnostic> mOut;
		};

		struct ConceptInfo
		{
			SymbolId symbol      = kNone;
			std::uint32_t node   = kNone; // ConceptDefinition
			std::uint32_t name   = kNone; // raw token index of the name
			std::uint32_t parent = kNone; // enclosing TemplateDeclaration or kNone
			std::uint32_t params = 0;
		};

		std::vector<ConceptInfo> CollectConcepts(const SemanticModel& model)
		{
			const auto& symbols = model.Symbols();
			const auto& nodes   = model.Tree().NodesSoA();
			TokenView view(model);
			std::vector<ConceptInfo> concepts;
			for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
			{
				if (symbols.kind[symbol] != SymbolKind::Concept)
				{
					continue;
				}

				const auto node = symbols.decl_node[symbol];
				if (node >= nodes.size() || nodes.Kind(node) != GrammarKind::ConceptDefinition)
				{
					continue;
				}

				// First word is the name (`concept` itself is a keyword, not a word).
				const auto[begin, end] = view.Range(node);
				std::uint32_t name     = kNone;
				for (auto position = begin; position < end; ++position)
				{
					if (view.IsWord(position))
					{
						name = view.TokenAt(position);
						break;
					}
				}

				if (name == kNone)
				{
					continue;
				}

				ConceptInfo info;
				info.symbol       = symbol;
				info.node         = node;
				info.name         = name;
				const auto parent = nodes.Parent(node);
				if (parent < nodes.size() && nodes.Kind(parent) == GrammarKind::TemplateDeclaration)
				{
					info.parent           = parent;
					const auto& templates = model.Templates();
					for (TemplateId id = 0; id < templates.node.size(); ++id)
					{
						if (templates.node[id] == parent)
						{
							info.params = templates.parameterCount[id];
							break;
						}
					}
				}

				concepts.push_back(info);
			}

			return concepts;
		}

		// Constraint body of `concept Name = <here>;`: after the `=` following the
		// name, before the closing `;`. Empty when malformed.
		bool ConceptConstraintRange(
			const TokenView& view,
			const ConceptInfo& info,
			std::size_t& begin,
			std::size_t& end)
		{
			const auto[rangeBegin, rangeEnd] = view.Range(info.node);
			const auto namePos               = view.PositionOf(info.name);
			std::size_t depth                = 0;
			for (auto position = namePos; position < rangeEnd; ++position)
			{
				const Tok tok = view.At(position);
				if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace)
				{
					++depth;
				}
				else if ((tok == Tok::RParen || tok == Tok::RBracket || tok == Tok::RBrace) && depth != 0)
				{
					--depth;
				}
				else if (tok == Tok::Eq && depth == 0)
				{
					begin = position + 1;
					end = rangeEnd > rangeBegin && view.At(rangeEnd - 1) == Tok::Semi ? rangeEnd - 1
					: rangeEnd;
					return begin < end;
				}
			}

			return false;
		}

		// Splits [begin, end) at top-level commas/equals. Brackets nest; `>>`
		// closes two angles (binder convention); anything unbalanced just ends.
		std::vector<std::pair<std::size_t, std::size_t>> SplitTopLevel(
			const TokenView& view,
			std::size_t begin,
			std::size_t end,
			bool splitEquals)
		{
			std::vector<std::pair<std::size_t, std::size_t>> parts;
			std::size_t depth     = 0;
			std::size_t angle     = 0;
			std::size_t partBegin = begin;
			for (auto position = begin; position < end; ++position)
			{
				const Tok tok = view.At(position);
				if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace)
				{
					++depth;
				}
				else if ((tok == Tok::RParen || tok == Tok::RBracket || tok == Tok::RBrace) && depth != 0)
				{
					--depth;
				}
				else if (depth == 0 && tok == Tok::Lt)
				{
					++angle;
				}
				else if (depth == 0 && tok == Tok::Gt && angle != 0)
				{
					--angle;
				}
				else if (depth == 0 && tok == Tok::Shr && angle >= 2)
				{
					angle -= 2;
				}
				else if (depth == 0 && angle == 0 &&
					(tok == Tok::Comma ||(splitEquals && tok == Tok::Eq)))
				{
					parts.emplace_back(partBegin, position);
					partBegin = position + 1;
				}
			}

			parts.emplace_back(partBegin, end);
			return parts;
		}

		std::uint32_t AncestorFunction(const GrammarNodeSoA& nodes, std::uint32_t node)
		{
			auto current = nodes.Parent(node);
			for (std::size_t steps = 0; steps <= nodes.size() && current < nodes.size(); ++steps)
			{
				const auto kind = nodes.Kind(current);
				if (kind == GrammarKind::FunctionDefinition || kind == GrammarKind::FunctionDeclaration)
				{
					return current;
				}

				if (kind == GrammarKind::TranslationUnit || kind == GrammarKind::NamespaceDefinition)
				{
					return kNone;
				}

				if (nodes.Parent(current) == current)
				{
					return kNone;
				}

				current = nodes.Parent(current);
			}

			return kNone;
		}

		// Owning function of a RequiresClause: trailing clauses nest under it;
		// leading clauses parented by the TemplateDeclaration belong to the
		// function declared right after them.
		std::uint32_t ClauseFunction(const SemanticModel& model, std::uint32_t node)
		{
			const auto& nodes = model.Tree().NodesSoA();
			if (const auto owner = AncestorFunction(nodes, node); owner != kNone)
			{
				return owner;
			}

			const auto parent = nodes.Parent(node);
			if (parent >= nodes.size() || nodes.Kind(parent) != GrammarKind::TemplateDeclaration)
			{
				return kNone;
			}

			bool seen = false;
			for (const auto child : model.ChildrenOf(parent))
			{
				if (child == node)
				{
					seen = true;
					continue;
				}

				if (!seen)
				{
					continue;
				}

				return nodes.Kind(child) == GrammarKind::FunctionDefinition ||
					nodes.Kind(child) == GrammarKind::FunctionDeclaration
				? child
				: kNone;
			}

			return kNone;
		}

		bool IsInsideTemplate(const GrammarNodeSoA& nodes, std::uint32_t node)
		{
			auto current = nodes.Parent(node);
			for (std::size_t steps = 0; steps <= nodes.size() && current < nodes.size(); ++steps)
			{
				if (nodes.Kind(current) == GrammarKind::TemplateDeclaration)
				{
					return true;
				}

				if (current == 0 || nodes.Parent(current) == current)
				{
					return false;
				}

				current = nodes.Parent(current);
			}

			return false;
		}

		bool IsConjunction(const TokenView& view, std::size_t op)
		{
			return view.At(op) == Tok::AmpAmp || view.Text(op) == "and";
		}

		bool IsDisjunction(const TokenView& view, std::size_t op)
		{
			return view.At(op) == Tok::PipePipe || view.Text(op) == "or";
		}

		// ---- 4.1-4.4: concept definitions --------------------------------------
		// Bits selecting which definition checks run (one dispatch entry per
		// rule code shares the concept traversal).
		constexpr std::uint32_t kConceptRecursion   = 1u << 0;
		constexpr std::uint32_t kConceptConstrained = 1u << 1;
		constexpr std::uint32_t kConceptPlacement   = 1u << 2;
		constexpr std::uint32_t kConceptSpecialized = 1u << 3;
		constexpr std::uint32_t kRequiresParameters = 1u << 4;
		constexpr std::uint32_t kRequiresValidity   = 1u << 5;
		constexpr std::uint32_t kConceptDefinitionsAll =
			kConceptRecursion | kConceptConstrained | kConceptPlacement | kConceptSpecialized;

		void CheckConceptDefinitions(
			const SemanticModel& model,
			const TokenView& view,
			const std::vector<ConceptInfo>& concepts,
			Emitter& emit,
			std::uint32_t wanted)
		{
			const auto& nodes   = model.Tree().NodesSoA();
			const auto& symbols = model.Symbols();
			for (const auto& info : concepts)
			{
				const std::string name(model.Names().Text(symbols.name[info.symbol]));
				const auto[begin, end] = view.Range(info.node);
				const auto namePos     = view.PositionOf(info.name);

				// 4.3: concepts live at namespace scope.
				if ((wanted& kConceptPlacement) != 0)
				{
					auto current = nodes.Parent(info.node);
					bool done    = false;
					for (std::size_t steps = 0; steps <= nodes.size() && current < nodes.size() &&
						!done; ++steps)
					{
						switch (nodes.Kind(current))
						{
						case GrammarKind::TranslationUnit:
						case GrammarKind::NamespaceDefinition:
							done = true;
							break;
						case GrammarKind::RecordDefinition:
						case GrammarKind::FunctionDeclaration:
						case GrammarKind::FunctionDefinition:
						case GrammarKind::CompoundStatement:
							emit.Add(RuleId::ConceptPlacement, Severity::Error, "cpp/concept-placement",
								"concept '" + name + "' must be defined at namespace scope",
								view.Offset(namePos), view.End(namePos) - view.Offset(namePos));
							done = true;
							break;
						default:
							break;
						}

						if (nodes.Parent(current) == current)
						{
							break;
						}

						current = nodes.Parent(current);
					}
				}

				// 4.4: no explicit specialization or instantiation (`concept C<...>`);
				// the defined name is never followed by `<`.
				if ((wanted& kConceptSpecialized) != 0 && namePos + 1 < end &&
					view.At(namePos + 1) == Tok::Lt)
				{
					emit.Add(RuleId::ConceptSpecialization, Severity::Error, "cpp/concept-specialization",
						"concept '" + name + "' cannot be explicitly specialized or instantiated",
						view.Offset(namePos), view.End(namePos) - view.Offset(namePos));
				}

				// 4.2: a concept definition is never constrained.
				if ((wanted& kConceptConstrained) != 0 && info.parent != kNone)
				{
					const auto[headBegin, headEnd] = view.Range(info.parent);
					const auto conceptBegin        = begin;
					bool sawRequires               = false;
					std::size_t requiresAt         = 0;
					for (auto position = headBegin; position < headEnd && position < conceptBegin;
						++position)
					{
						if (view.At(position) == Tok::KwRequires)
						{
							sawRequires = true;
							requiresAt  = position;
							break;
						}
					}

					if (sawRequires)
					{
						emit.Add(RuleId::ConceptConstrained, Severity::Error, "cpp/concept-constrained",
							"concept '" + name + "' cannot have a requires-clause", view.Offset(requiresAt),
							view.End(requiresAt) - view.Offset(requiresAt));
					}

					// Constrained parameters: `template<C1 T>` has two top-level
					// words (`C1`, `T`); plain `typename T` / `int N` / `auto N` one.
					// Defaults are not parameters.
					auto head = headBegin;
					while (head < conceptBegin && view.At(head) != Tok::Lt)
					{
						++head;
					}

					const auto headClose = head < conceptBegin ? view.MatchAngle(head, conceptBegin)
					: conceptBegin;
					if (head < headClose)
					{
						for (const auto& part : SplitTopLevel(view, head + 1, headClose, false))
						{
							const auto noDefault  = SplitTopLevel(view, part.first, part.second, true);
							const auto& paramHead = noDefault.empty() ? part : noDefault.front();
							std::size_t words     = 0;
							for (auto position = paramHead.first; position < paramHead.second;)
							{
								if (view.At(position) == Tok::Lt)
								{
									const auto close = view.MatchAngle(position, paramHead.second);
									position         = close < paramHead.second ? close + 1 : paramHead.second;
									continue;
								}

								words += view.IsWord(position) ? 1 : 0;
								++position;
							}

							if (words >= 2)
							{
								emit.Add(RuleId::ConceptConstrained, Severity::Error,
									"cpp/concept-constrained",
									"concept '" + name +
									"' cannot be declared with constrained template parameters",
									view.Offset(namePos), view.End(namePos) - view.Offset(namePos));
								break;
							}
						}
					}
				}

				// 4.1: recursion — any use inside the own constraint that resolves
				// to the concept itself. Uses shadowed by a same-named local resolve
				// to the parameter instead, so they stay silent.
				if ((wanted& kConceptRecursion) == 0)
				{
					continue;
				}

				std::size_t constraintBegin = 0;
				std::size_t constraintEnd   = 0;
				if (!ConceptConstraintRange(view, info, constraintBegin, constraintEnd))
				{
					continue;
				}

				std::size_t reported = static_cast<std::size_t>(-1);
				for (std::uint32_t node = 0; node < nodes.size(); ++node)
				{
					const auto kind = nodes.Kind(node);
					if (kind != GrammarKind::IdentifierExpression &&
						kind != GrammarKind::TemplateIdExpression)
					{
						continue;
					}

					const auto[nodeBegin, nodeEnd] = view.Range(node);
					if (nodeBegin < constraintBegin || nodeEnd > constraintEnd || nodeBegin >= nodeEnd)
					{
						continue;
					}

					// Template-id heads that are qualified (`ns::C<T>`) name something
					// else textually; only bare heads can be self-mentions.
					if (kind == GrammarKind::TemplateIdExpression)
					{
						bool qualified = false;
						for (auto position = nodeBegin; position < nodeEnd; ++position)
						{
							if (view.At(position) == Tok::Lt)
							{
								break;
							}

							qualified |= view.At(position) == Tok::ColonColon;
						}

						if (qualified)
						{
							continue;
						}
					}

					if (view.Text(nodeBegin) != name)
					{
						continue;
					}

					if (model.ResolveToken(view.TokenAt(nodeBegin)) != info.symbol)
					{
						continue;
					}

					const auto offset = view.Offset(nodeBegin);
					if (offset == reported)
					{
						continue;
					}

					reported = offset;
					emit.Add(RuleId::ConceptRecursion, Severity::Error, "cpp/concept-recursive",
						"concept '" + name + "' refers to itself in its own definition", offset,
						view.End(nodeBegin) - offset);
				}
			}
		}

		// ---- 4.5: requires-clause form ------------------------------------------
		void CheckConstraintOperand(
			const SemanticModel& model,
			const TokenView& view,
			std::uint32_t node,
			Emitter& emit)
		{
			const auto& nodes = model.Tree().NodesSoA();
			switch (nodes.Kind(node))
			{
			case GrammarKind::BinaryExpression:
			{
				const auto children = model.ChildrenOf(node);
				if (children.size() != 2)
				{
					return;
				}

				const auto op = view.Range(children[0]).second;
				if (IsConjunction(view, op) || IsDisjunction(view, op))
				{
					CheckConstraintOperand(model, view, children[0], emit);
					CheckConstraintOperand(model, view, children[1], emit);
				}

				return;
			}
			case GrammarKind::ParenthesizedExpression:
				return; // Parentheses sanitize any constraint.
			case GrammarKind::CallExpression:
			{
				// `is_purrable<T>()` is not a primary expression: a call with
				// template arguments must be parenthesized. Plain `f(x)` is fine.
				const auto children = model.ChildrenOf(node);
				if (!children.empty() && nodes.Kind(children[0]) == GrammarKind::TemplateIdExpression)
				{
					const auto[begin, end] = view.Range(node);
					emit.Add(RuleId::RequiresForm, Severity::Error, "cpp/requires-form",
						"call with template arguments in a requires-clause must be parenthesized",
						view.Offset(begin), view.End(begin) - view.Offset(begin));
				}

				return;
			}
			default:
				return;
			}
		}

		void CheckRequiresClauses(
			const SemanticModel& model,
			const TokenView& view,
			Emitter& emit)
		{
			const auto& nodes = model.Tree().NodesSoA();
			for (std::uint32_t node = 0; node < nodes.size(); ++node)
			{
				if (nodes.Kind(node) != GrammarKind::RequiresClause)
				{
					continue;
				}

				for (const auto child : model.ChildrenOf(node))
				{
					CheckConstraintOperand(model, view, child, emit);
				}
			}
		}

		// ---- 4.6 + 4.8: requires-expressions --------------------------------------
		void CheckRequiresExpressions(
			const SemanticModel& model,
			const TokenView& view,
			Emitter& emit,
			std::uint32_t wanted)
		{
			const auto& nodes = model.Tree().NodesSoA();
			for (std::uint32_t node = 0; node < nodes.size(); ++node)
			{
				if (nodes.Kind(node) != GrammarKind::RequiresExpression)
				{
					continue;
				}

				const auto[begin, end] = view.Range(node);

				// 4.6: no default arguments, no trailing ellipsis in the local list.
				if ((wanted& kRequiresParameters) != 0 && begin + 1 < end &&
					view.At(begin + 1) == Tok::LParen)
				{
					const auto close = view.Match(begin + 1, end);
					if (close < end)
					{
						for (const auto& part : SplitTopLevel(view, begin + 2, close, false))
						{
							const auto valued = SplitTopLevel(view, part.first, part.second, true);
							if (valued.size() > 1)
							{
								const auto eq = valued[0].second;
								emit.Add(RuleId::RequiresParameter, Severity::Error,
									"cpp/requires-parameter",
									"requires-expression parameter cannot have a default argument",
									view.Offset(eq), view.End(eq) - view.Offset(eq));
								continue;
							}

							if (part.second == part.first + 1 && view.At(part.first) == Tok::Ellipsis)
							{
								emit.Add(RuleId::RequiresParameter, Severity::Error,
									"cpp/requires-parameter",
									"requires-expression parameter list cannot end with an ellipsis",
									view.Offset(part.first),
									view.End(part.first) - view.Offset(part.first));
							}
						}
					}
				}

				// 4.8 (narrowed): outside any template, a type requirement naming a
				// single undeclared identifier is ill-formed (no substitution can
				// save it). Qualified names and specializations stay silent.
				if ((wanted& kRequiresValidity) == 0)
				{
					continue;
				}

				if (IsInsideTemplate(nodes, node))
				{
					continue;
				}

				for (const auto child : model.ChildrenOf(node))
				{
					if (nodes.Kind(child) != GrammarKind::Requirement ||
						model.Tree().RequirementKindOf(child) != RequirementKind::Type)
					{
						continue;
					}

					const auto[childBegin, childEnd] = view.Range(child);
					if (childEnd != childBegin + 3 || view.At(childBegin) != Tok::KwTypename ||
						!view.IsWord(childBegin + 1) ||
						(view.At(childEnd - 1) != Tok::Semi && view.At(childEnd - 1) != Tok::RBrace))
					{
						continue;
					}

					const auto namePos = childBegin + 1;
					const auto nameId  = model.Names().Find(view.Text(namePos));
					const auto existing =
						nameId == kNone ? kNone
					: model.Lookup(model.ScopeOfNode(child), nameId, view.TokenAt(namePos));
					if (existing != kNone)
					{
						continue;
					}

					emit.Add(RuleId::RequiresValidity, Severity::Error, "cpp/requires-unknown-type",
						"type requirement names unknown type '" + std::string(view.Text(namePos)) + "'",
						view.Offset(namePos), view.End(namePos) - view.Offset(namePos));
				}
			}
		}

		// ---- 4.7: concept-id arity ----------------------------------------------
		// A concept-id in a constraint expression takes all its parameters
		// (`requires C<T>`); a type-constraint takes one less, the deduced type
		// being implicit (`template<C<T>>` is wrong for 1-param C, `template<C>`
		// and `-> C` are right).

		// `>` closing the `<` at open. Unlike TokenView::MatchAngle this treats
		// `>>` as two closers (binder convention): in template heads and
		// placeholders `>>` always closes, since a bare shift needs parentheses.
		std::size_t MatchAngleClose(const TokenView& view, std::size_t open, std::size_t limit)
		{
			std::size_t depth = 0;
			for (auto position = open; position < limit; ++position)
			{
				const Tok tok = view.At(position);
				if (tok == Tok::Lt)
				{
					++depth;
				}
				else if (tok == Tok::Gt)
				{
					if (depth <= 1)
					{
						return position;
					}

					--depth;
				}
				else if (tok == Tok::Shr)
				{
					if (depth <= 2)
					{
						return position;
					}

					depth -= 2;
				}
				else if (tok == Tok::Semi || tok == Tok::LBrace || tok == Tok::RParen ||
					tok == Tok::RBracket || tok == Tok::RBrace)
				{
					return limit;
				}
			}

			return limit;
		}

		// Explicit argument count of `<...>` at open (which is `<`).
		std::size_t CountExplicitArgs(const TokenView& view, std::size_t open, std::size_t limit)
		{
			const auto close = MatchAngleClose(view, open, limit);
			if (close >= limit || close == open + 1)
			{
				return 0;
			}

			std::size_t args  = 1;
			std::size_t paren = 0;
			std::size_t angle = 0;
			for (auto position = open + 1; position < close; ++position)
			{
				const Tok tok = view.At(position);
				if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace)
				{
					++paren;
				}
				else if ((tok == Tok::RParen || tok == Tok::RBracket || tok == Tok::RBrace) && paren != 0)
				{
					--paren;
				}
				else if (paren == 0 && tok == Tok::Lt)
				{
					++angle;
				}
				else if (paren == 0 && tok == Tok::Gt && angle != 0)
				{
					--angle;
				}
				else if (paren == 0 && angle == 0 && tok == Tok::Shr && position + 1 < close)
				{
					angle = 0;
					return args + 1; // conservative: `>>` inside means nesting we gave up on
				}
				else if (paren == 0 && angle == 0 && tok == Tok::Comma)
				{
					++args;
				}
			}

			return args;
		}

		struct ArityUse
		{
			std::size_t head; // view position of the concept name
			bool minusOne;    // type-constraint: deduced type is implicit
			std::uint32_t scopeNode;
		};

		void CheckArityUse(
			const SemanticModel& model,
			const TokenView& view,
			const std::vector<ConceptInfo>& concepts,
			std::vector<std::size_t>& reported,
			const ArityUse& use,
			Emitter& emit)
		{
			const auto& nodes = model.Tree().NodesSoA();
			if (!view.IsWord(use.head) || use.head + 1 >= view.Size() || view.At(use.head + 1) != Tok::Lt)
			{
				return;
			}

			const std::string headText(view.Text(use.head));
			const auto nameId = model.Names().Find(headText);
			if (nameId == kNone)
			{
				return;
			}

			const auto scopeNode = use.scopeNode < nodes.size() ? use.scopeNode : 0;
			const auto symbol =
				model.Lookup(model.ScopeOfNode(scopeNode), nameId, view.TokenAt(use.head));
			if (symbol == kNone || model.Symbols().kind[symbol] != SymbolKind::Concept)
			{
				return; // Unknown or shadowed: no conclusion.
			}

			std::uint32_t params = 0;
			for (const auto& info : concepts)
			{
				if (info.symbol == symbol)
				{
					params = info.params;
					break;
				}
			}

			if (params == 0)
			{
				return;
			}

			const auto given = CountExplicitArgs(view, use.head + 1, view.Size());
			const auto total = given +(use.minusOne ? 1 : 0);
			if (total == params)
			{
				return;
			}

			const auto offset = view.Offset(use.head);
			for (const auto done : reported)
			{
				if (done == offset)
				{
					return;
				}
			}

			reported.push_back(offset);
			std::string message = "concept '" + headText + "' has " + std::to_string(params) +
				" template parameter" +(params == 1 ? "" : "s") + " but is used with " +
				std::to_string(given) + " explicit argument" +(given == 1 ? "" : "s") + " here";
			if (use.minusOne)
			{
				message += " (the deduced type is implicit)";
			}

			emit.Add(RuleId::ConceptArity, Severity::Error, "cpp/concept-arity", std::move(message),
				offset, view.End(use.head) - offset);
		}

		void CheckConceptArity(
			const SemanticModel& model,
			const TokenView& view,
			const std::vector<ConceptInfo>& concepts,
			Emitter& emit)
		{
			const auto& nodes = model.Tree().NodesSoA();
			std::vector<std::size_t> reported;

			// Node-shaped uses: template-ids inside clauses, requirement bodies and
			// concept initializers. A `-> C<T>` compound constraint is a
			// type-constraint (deduced type implicit); anything else here takes all
			// its parameters. Callee-position calls (`f<T>()`) belong to 4.5.
			for (std::uint32_t node = 0; node < nodes.size(); ++node)
			{
				if (nodes.Kind(node) != GrammarKind::TemplateIdExpression)
				{
					continue;
				}

				const auto[begin, end] = view.Range(node);
				if (begin + 1 >= end ||!view.IsWord(begin) || view.At(begin + 1) != Tok::Lt)
				{
					continue; // Qualified (`ns::C<T>`): a different lookup domain.
				}

				const auto parent = nodes.Parent(node);
				if (parent < nodes.size() && nodes.Kind(parent) == GrammarKind::CallExpression)
				{
					const auto siblings = model.ChildrenOf(parent);
					if (!siblings.empty() && siblings.front() == node)
					{
						continue;
					}
				}

				// Nearest constraint context: requirement bodies (possibly behind
				// `->`), clauses, or concept initializers.
				bool insideRequirement = false;
				bool insideClause      = false;
				bool insideConceptInit = false;
				auto current           = parent;
				for (std::size_t steps = 0; steps <= nodes.size() && current < nodes.size(); ++steps)
				{
					const auto kind = nodes.Kind(current);
					if (kind == GrammarKind::Requirement)
					{
						insideRequirement = true;
					}
					else if (kind == GrammarKind::RequiresClause)
					{
						insideClause = true;
						break;
					}
					else if (kind == GrammarKind::RequiresExpression)
					{
						break;
					}
					else if (kind == GrammarKind::ConceptDefinition)
					{
						insideConceptInit = true;
						break;
					}
					else if (kind == GrammarKind::FunctionDefinition ||
						kind == GrammarKind::FunctionDeclaration ||
						kind == GrammarKind::TranslationUnit || kind == GrammarKind::NamespaceDefinition ||
						kind == GrammarKind::TemplateDeclaration)
					{
						break;
					}

					if (nodes.Parent(current) == current)
					{
						break;
					}

					current = nodes.Parent(current);
				}

				if (!insideRequirement && !insideClause && !insideConceptInit)
				{
					continue;
				}

				// Self-mentions belong to 4.1/4.4.
				if (insideConceptInit)
				{
					const auto[conceptBegin, conceptEnd] = view.Range(current);
					(void) conceptEnd;
					bool self = false;
					for (auto position = conceptBegin; position < begin; ++position)
					{
						if (view.IsWord(position) && view.Text(position) == view.Text(begin))
						{
							self = true;
							break;
						}
					}

					if (self)
					{
						continue;
					}
				}

				const bool minusOne =
					insideRequirement && begin > 0 && view.At(begin - 1) == Tok::Arrow;
				CheckArityUse(model, view, concepts, reported, {begin, minusOne, node}, emit);
			}

			// Token-shaped uses (the grammar skips these): constrained template
			// parameters and `Concept<...> auto` placeholders.
			for (std::uint32_t node = 0; node < nodes.size(); ++node)
			{
				const auto kind       = nodes.Kind(node);
				std::size_t zoneBegin = 0;
				std::size_t zoneEnd   = 0;
				bool isZone           = false;
				if (kind == GrammarKind::TemplateDeclaration)
				{
					const auto children = model.ChildrenOf(node);
					if (children.empty())
					{
						continue;
					}

					const auto[begin, end]           = view.Range(node);
					const auto[childBegin, childEnd] = view.Range(children.front());
					(void) childEnd;
					if (childBegin > begin)
					{
						zoneBegin = begin;
						zoneEnd   = childBegin;
						isZone    = true;
					}
				}
				else if (kind == GrammarKind::TypeSpecifier)
				{
					const auto[begin, end] = view.Range(node);
					bool placeholder       = false;
					for (auto position = begin; position < end; ++position)
					{
						if (view.At(position) == Tok::KwAuto || view.At(position) == Tok::KwDecltype)
						{
							placeholder = true;
							break;
						}
					}

					if (placeholder)
					{
						zoneBegin = begin;
						zoneEnd   = end;
						isZone    = true;
					}
				}

				if (!isZone)
				{
					continue;
				}

				for (auto position = zoneBegin; position + 1 < zoneEnd; ++position)
				{
					if (!view.IsWord(position) || view.At(position + 1) != Tok::Lt)
					{
						continue;
					}

					if (position > zoneBegin && view.At(position - 1) == Tok::ColonColon)
					{
						continue; // Qualified: a different lookup domain.
					}

					const auto close = MatchAngleClose(view, position + 1, zoneEnd);
					CheckArityUse(model, view, concepts, reported, {position, true, node}, emit);
					position = close < zoneEnd ? close : position;
				}
			}
		}

		// ---- 4.9: redeclaration shape (Warning) -----------------------------------
		// Ill-formed/no-diagnostic-required when a constrained declaration is
		// redeclared with a different syntactic form. Compares whitespace-normalized
		// requires-clause spellings across same (scope, name, signature) functions;
		// template heads are ignored (`class` vs `typename` may legally differ).
		void CheckConceptRedeclarations(
			const SemanticModel& model,
			const TokenView& view,
			Emitter& emit)
		{
			const auto& nodes   = model.Tree().NodesSoA();
			const auto& symbols = model.Symbols();
			std::map<std::uint32_t, std::vector<SymbolId>> clausesByFunction;
			for (std::uint32_t node = 0; node < nodes.size(); ++node)
			{
				if (nodes.Kind(node) != GrammarKind::RequiresClause)
				{
					continue;
				}

				const auto owner = ClauseFunction(model, node);
				if (owner == kNone)
				{
					continue;
				}

				clausesByFunction[owner].push_back(node);
			}

			std::map<SymbolId, std::string> spellingBySymbol;
			std::map<SymbolId, std::size_t> offsetBySymbol;
			for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
			{
				if (symbols.kind[symbol] != SymbolKind::Function)
				{
					continue;
				}

				const auto decl  = symbols.decl_node[symbol];
				const auto found = clausesByFunction.find(decl);
				if (found == clausesByFunction.end())
				{
					continue;
				}

				auto ordered = found->second;
				std::sort(ordered.begin(), ordered.end(),
					[&](std::uint32_t a, std::uint32_t b)
					{
						return view.Range(a).first < view.Range(b).first;
					});
				std::string spelling;
				for (const auto clause : ordered)
				{
					const auto[begin, end] = view.Range(clause);
					if (!spelling.empty())
					{
						spelling += " ";
					}

					spelling += view.Spell(begin, end);
				}

				spellingBySymbol[symbol]           = std::move(spelling);
				const auto[clauseBegin, clauseEnd] = view.Range(ordered.front());
				(void) clauseEnd;
				offsetBySymbol[symbol] = clauseBegin;
			}

			std::map<std::tuple<ScopeId, NameId, std::uint64_t>, std::vector<SymbolId>> groups;
			for (const auto& [symbol, spelling] : spellingBySymbol)
			{
				(void) spelling;
				if (symbols.signature[symbol] == 0)
				{
					continue; // Constructors share signature 0: cannot group safely.
				}

				groups[{symbols.scope[symbol], symbols.name[symbol], symbols.signature[symbol]}]
				.push_back(symbol);
			}

			for (const auto& [key, members] : groups)
			{
				(void) key;
				if (members.size() < 2)
				{
					continue;
				}

				auto ordered = members;
				std::sort(ordered.begin(), ordered.end(),
					[&](SymbolId a, SymbolId b)
					{
						return view.Offset(view.PositionOf(symbols.decl_token[a])) <
						view.Offset(view.PositionOf(symbols.decl_token[b]));
					});
				const auto& reference = spellingBySymbol[ordered.front()];
				for (std::size_t i = 1; i < ordered.size(); ++i)
				{
					if (spellingBySymbol[ordered[i]] == reference)
					{
						continue;
					}

					const auto at     = offsetBySymbol[ordered[i]];
					const auto nameId = symbols.name[ordered[i]];
					emit.Add(RuleId::ConceptRedeclaration, Severity::Warning,
						"cpp/concept-redeclaration",
						"constrained '" + std::string(model.Names().Text(nameId)) +
						"' redeclared with a different constraint spelling; redeclarations must use the "
						"same syntactic form",
						view.Offset(at), view.End(at) - view.Offset(at));
				}
			}
		}

	} // namespace

	std::vector<Diagnostic> SemanticRules::AnalyzeConcepts(const SemanticModel& model)
	{
		Emitter emit(model);
		if (!model.Tree().Diagnostics().empty())
		{
			return emit.Take();
		}

		const TokenView view(model);
		const auto concepts = CollectConcepts(model);
		CheckConceptDefinitions(model, view, concepts, emit, kConceptDefinitionsAll);
		CheckRequiresClauses(model, view, emit);
		CheckRequiresExpressions(model, view, emit, kRequiresParameters | kRequiresValidity);
		CheckConceptArity(model, view, concepts, emit);
		CheckConceptRedeclarations(model, view, emit);
		return emit.Take();
	}

	namespace
	{

		// One dispatch entry per rule code shares the traversals above through the
		// rule masks, so enabling several concept rules never repeats diagnostics.
		struct ConceptWork
		{
			explicit ConceptWork(const SemanticModel& model) : view(model), concepts(CollectConcepts(model)) {}

			TokenView view;
			std::vector<ConceptInfo> concepts;
		};

	} // namespace

	std::vector<Diagnostic> SemanticRules::AnalyzeConceptRecursion(const SemanticModel& model)
	{
		Emitter emit(model);
		if (model.Tree().Diagnostics().empty())
		{
			ConceptWork work(model);
			CheckConceptDefinitions(model, work.view, work.concepts, emit, kConceptRecursion);
		}

		return emit.Take();
	}

	std::vector<Diagnostic> SemanticRules::AnalyzeConceptConstrained(const SemanticModel& model)
	{
		Emitter emit(model);
		if (model.Tree().Diagnostics().empty())
		{
			ConceptWork work(model);
			CheckConceptDefinitions(model, work.view, work.concepts, emit, kConceptConstrained);
		}

		return emit.Take();
	}

	std::vector<Diagnostic> SemanticRules::AnalyzeConceptPlacement(const SemanticModel& model)
	{
		Emitter emit(model);
		if (model.Tree().Diagnostics().empty())
		{
			ConceptWork work(model);
			CheckConceptDefinitions(model, work.view, work.concepts, emit, kConceptPlacement);
		}

		return emit.Take();
	}

	std::vector<Diagnostic> SemanticRules::AnalyzeConceptSpecialization(
		const SemanticModel& model)
	{
		Emitter emit(model);
		if (model.Tree().Diagnostics().empty())
		{
			ConceptWork work(model);
			CheckConceptDefinitions(model, work.view, work.concepts, emit, kConceptSpecialized);
		}

		return emit.Take();
	}

	std::vector<Diagnostic> SemanticRules::AnalyzeRequiresForm(const SemanticModel& model)
	{
		Emitter emit(model);
		if (model.Tree().Diagnostics().empty())
		{
			const TokenView view(model);
			CheckRequiresClauses(model, view, emit);
		}

		return emit.Take();
	}

	std::vector<Diagnostic> SemanticRules::AnalyzeRequiresParameter(const SemanticModel& model)
	{
		Emitter emit(model);
		if (model.Tree().Diagnostics().empty())
		{
			const TokenView view(model);
			CheckRequiresExpressions(model, view, emit, kRequiresParameters);
		}

		return emit.Take();
	}

	std::vector<Diagnostic> SemanticRules::AnalyzeConceptArity(const SemanticModel& model)
	{
		Emitter emit(model);
		if (model.Tree().Diagnostics().empty())
		{
			ConceptWork work(model);
			CheckConceptArity(model, work.view, work.concepts, emit);
		}

		return emit.Take();
	}

	std::vector<Diagnostic> SemanticRules::AnalyzeRequiresValidity(const SemanticModel& model)
	{
		Emitter emit(model);
		if (model.Tree().Diagnostics().empty())
		{
			const TokenView view(model);
			CheckRequiresExpressions(model, view, emit, kRequiresValidity);
		}

		return emit.Take();
	}

	std::vector<Diagnostic> SemanticRules::AnalyzeConceptRedeclaration(const SemanticModel& model)
	{
		Emitter emit(model);
		if (model.Tree().Diagnostics().empty())
		{
			const TokenView view(model);
			CheckConceptRedeclarations(model, view, emit);
		}

		return emit.Take();
	}

	// ---- Phase 5: normalization --------------------------------------------
	namespace concepts
	{

		using ConstraintAtom       = SemanticRules::ConstraintAtom;
		using NormalizedConstraint = SemanticRules::NormalizedConstraint;

		constexpr std::size_t kMaxExpansionDepth = 8;
		constexpr std::size_t kMaxNormalClauses  = 64;

		struct Definition
		{
			std::uint32_t node      = kNone; // ConceptDefinition
			std::uint32_t template_ = kNone; // enclosing TemplateDeclaration
			std::vector<std::string> params;
		};

		bool FindDefinition(
			const SemanticModel& model,
			const TokenView& view,
			std::string_view name,
			Definition& out)
		{
			const auto& symbols = model.Symbols();
			const auto& nodes   = model.Tree().NodesSoA();
			const auto nameId   = model.Names().Find(name);
			if (nameId == kNone)
			{
				return false;
			}

			for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
			{
				if (symbols.kind[symbol] != SymbolKind::Concept || symbols.name[symbol] != nameId)
				{
					continue;
				}

				const auto node = symbols.decl_node[symbol];
				if (node >= nodes.size() || nodes.Kind(node) != GrammarKind::ConceptDefinition)
				{
					continue;
				}

				out.node          = node;
				const auto parent = nodes.Parent(node);
				if (parent < nodes.size() && nodes.Kind(parent) == GrammarKind::TemplateDeclaration)
				{
					out.template_          = parent;
					const auto& templates  = model.Templates();
					const auto& parameters = model.TemplateParameters();
					for (TemplateId id = 0; id < templates.node.size(); ++id)
					{
						if (templates.node[id] != parent)
						{
							continue;
						}

						for (std::uint32_t i = 0; i < templates.parameterCount[id]; ++i)
						{
							const auto entry = templates.firstParameter[id] + i;
							std::string param;
							if (entry < parameters.symbol.size() &&
								parameters.symbol[entry] != kNone &&
								parameters.symbol[entry] < symbols.Size())
							{
								param = std::string(
									model.Names().Text(symbols.name[parameters.symbol[entry]]));
							}

							out.params.push_back(std::move(param));
						}

						break;
					}
				}

				return true;
			}

			return false;
		}

		// Constraint body of a concept definition node (after `=`), or empty.
		std::pair<std::size_t, std::size_t> ConceptBody(const TokenView& view, std::uint32_t node)
		{
			const auto[begin, end] = view.Range(node);
			std::size_t depth      = 0;
			for (auto position = begin; position < end; ++position)
			{
				const Tok tok = view.At(position);
				if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace)
				{
					++depth;
				}
				else if ((tok == Tok::RParen || tok == Tok::RBracket || tok == Tok::RBrace) && depth != 0)
				{
					--depth;
				}
				else if (tok == Tok::Eq && depth == 0)
				{
					const auto bodyEnd =
						end > begin && view.At(end - 1) == Tok::Semi ? end - 1 : end;
					return {position + 1, bodyEnd};
				}
			}

			return {end, end};
		}

		std::string SubstituteWords(
			std::string_view text,
			const std::vector<std::string>& names,
			const std::vector<std::string>& args)
		{
			std::string out;
			std::size_t i = 0;
			while (i < text.size())
			{
				const char c = text[i];
				if ((c >= 'A' && c <= 'Z') ||(c >= 'a' && c <= 'z') || c == '_' ||
					(c >= '0' && c <= '9' && !out.empty()))
				{
					std::size_t j = i + 1;
					while (j < text.size() &&
						((text[j] >= 'A' && text[j] <= 'Z') ||(text[j] >= 'a' && text[j] <= 'z') ||
						text[j] == '_' ||(text[j] >= '0' && text[j] <= '9')))
					{
						++j;
					}

					const auto word = text.substr(i, j - i);
					bool replaced   = false;
					for (std::size_t k = 0; k < names.size() && k < args.size(); ++k)
					{
						if (!names[k].empty() && word == names[k])
						{
							out += args[k];
							replaced = true;
							break;
						}
					}

					if (!replaced)
					{
						out.append(word);
					}

					i = j;
				}
				else
				{
					out.push_back(c);
					++i;
				}
			}

			return out;
		}

		struct NormContext
		{
			const SemanticModel* model;
			TokenView* view;
			std::string provenance;
			std::vector<std::string> names; // outer parameter names
			std::vector<std::string> args;  // outer parameter spellings
			std::vector<std::string> active;
			std::size_t depth = 0;
		};

		// Splits [begin, end) at top-level `||` (wantOr) or `&&`.
		std::vector<std::pair<std::size_t, std::size_t>> SplitLogic(
			TokenView& view,
			std::size_t begin,
			std::size_t end,
			bool wantOr)
		{
			std::vector<std::pair<std::size_t, std::size_t>> parts;
			std::size_t depth = 0;
			std::size_t angle = 0;
			auto partBegin    = begin;
			for (auto position = begin; position < end; ++position)
			{
				const Tok tok = view.At(position);
				if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace)
				{
					++depth;
				}
				else if ((tok == Tok::RParen || tok == Tok::RBracket || tok == Tok::RBrace) && depth != 0)
				{
					--depth;
				}
				else if (depth == 0 && tok == Tok::Lt)
				{
					++angle;
				}
				else if (depth == 0 && tok == Tok::Gt && angle != 0)
				{
					--angle;
				}
				else if (depth == 0 && tok == Tok::Shr)
				{
					angle = angle >= 2 ? angle - 2 : 0;
				}
				else if (depth == 0 && angle == 0)
				{
					const bool junction = wantOr
					? (tok == Tok::PipePipe || view.Text(position) == "or")
					: (tok == Tok::AmpAmp || view.Text(position) == "and");
					if (junction)
					{
						parts.emplace_back(partBegin, position);
						partBegin = position + 1;
					}
				}
			}

			parts.emplace_back(partBegin, end);
			return parts;
		}

		NormalizedConstraint NormalizeRange(
			NormContext& context,
			std::size_t begin,
			std::size_t end);

		std::string AtomKey(const ConstraintAtom& atom)
		{
			std::string key = atom.provenance;
			key.push_back('\x1f');
			key += atom.expression;
			for (const auto& argument : atom.arguments)
			{
				key.push_back('\x1e');
				key += argument;
			}

			return key;
		}

		NormalizedConstraint MakeAtom(
			NormContext& context,
			std::size_t begin,
			std::size_t end,
			std::string provenance)
		{
			NormalizedConstraint result;
			result.kind            = NormalizedConstraint::Kind::Atom;
			result.atom.provenance = std::move(provenance);
			result.atom.expression = context.view->Spell(begin, end);
			result.atom.arguments  = context.args;
			return result;
		}

		// Whole range is `Name<args>` with `args == params` and a known definition:
		// expand the body with the parameter mapping.
		bool ExpandConcept(
			NormContext& context,
			std::size_t begin,
			std::size_t end,
			NormalizedConstraint& out)
		{
			if (!context.view->IsWord(begin) || begin + 1 >= end ||
				context.view->At(begin + 1) != Tok::Lt)
			{
				return false;
			}

			const auto close = MatchAngleClose(*context.view, begin + 1, end);
			if (close + 1 != end)
			{
				return false;
			}

			const std::string name(context.view->Text(begin));
			Definition definition;
			if (!FindDefinition(*context.model, *context.view, name, definition) ||
				definition.node == kNone || definition.params.empty())
			{
				return false;
			}

			for (const auto& active : context.active)
			{
				if (active == name)
				{
					return false;
				}
			}

			if (context.depth >= kMaxExpansionDepth)
			{
				return false;
			}

			const auto[bodyBegin, bodyEnd] = ConceptBody(*context.view, definition.node);
			if (bodyBegin >= bodyEnd)
			{
				return false;
			}

			// Positional mapping: spelled args with outer names substituted. An
			// identity mapping (`Meowable<T>` for `template<class T>`) keeps the
			// ambient one, so a use spells the same atom as the definition body.
			std::vector<std::string> args;
			for (const auto& part : SplitTopLevel(*context.view, begin + 2, close, false))
			{
				// Spellings carry a trailing blank; trim it so identity mappings
				// (`Meowable<T>` for `template<class T>`) compare equal to names.
				auto spelled = SubstituteWords(
					context.view->Spell(part.first, part.second), context.names, context.args);
				while (!spelled.empty() && spelled.back() == ' ')
				{
					spelled.pop_back();
				}

				args.push_back(std::move(spelled));
			}

			if (args.size() != definition.params.size())
			{
				return false;
			}

			if (args == definition.params)
			{
				args = context.args;
			}

			context.active.push_back(name);
			NormContext inner {context.model, context.view, "concept:" + name, definition.params, args,
				context.active, context.depth + 1};
			out = NormalizeRange(inner, bodyBegin, bodyEnd);
			context.active.pop_back();
			return true;
		}

		NormalizedConstraint NormalizeRange(
			NormContext& context,
			std::size_t begin,
			std::size_t end)
		{
			// One enclosing pair of parentheses belongs to grouping, not the form.
			if (begin < end && context.view->At(begin) == Tok::LParen &&
				context.view->Match(begin, end) == end - 1)
			{
				return NormalizeRange(context, begin + 1, end - 1);
			}

			if (const auto disjuncts = SplitLogic(*context.view, begin, end, true);
				disjuncts.size() > 1)
			{
				NormalizedConstraint result;
				result.kind = NormalizedConstraint::Kind::Any;
				for (const auto& part : disjuncts)
				{
					result.operands.push_back(NormalizeRange(context, part.first, part.second));
				}

				return result;
			}

			if (const auto conjuncts = SplitLogic(*context.view, begin, end, false);
				conjuncts.size() > 1)
			{
				NormalizedConstraint result;
				result.kind = NormalizedConstraint::Kind::All;
				for (const auto& part : conjuncts)
				{
					result.operands.push_back(NormalizeRange(context, part.first, part.second));
				}

				return result;
			}

			NormalizedConstraint expanded;
			if (ExpandConcept(context, begin, end, expanded))
			{
				return expanded;
			}

			return MakeAtom(context, begin, end, context.provenance);
		}

		using ClauseSet = std::vector<std::vector<std::string>>;

		std::optional<ClauseSet> ToDnf(const NormalizedConstraint& node)
		{
			if (node.kind == NormalizedConstraint::Kind::Atom)
			{
				return ClauseSet {
					{
						AtomKey(node.atom)
				}};
			}

			if (node.kind == NormalizedConstraint::Kind::Any)
			{
				ClauseSet out;
				for (const auto& operand : node.operands)
				{
					auto part = ToDnf(operand);
					if (!part)
					{
						return std::nullopt;
					}

					out.insert(out.end(), part->begin(), part->end());
					if (out.size() > kMaxNormalClauses)
					{
						return std::nullopt;
					}
				}

				return out;
			}

			ClauseSet out {{}};
			for (const auto& operand : node.operands)
			{
				auto part = ToDnf(operand);
				if (!part)
				{
					return std::nullopt;
				}

				ClauseSet next;
				for (const auto& have : out)
				{
					for (const auto& more : *part)
					{
						auto combined = have;
						combined.insert(combined.end(), more.begin(), more.end());
						next.push_back(std::move(combined));
						if (next.size() > kMaxNormalClauses)
						{
							return std::nullopt;
						}
					}
				}

				out = std::move(next);
			}

			return out;
		}

		std::optional<ClauseSet> ToCnf(const NormalizedConstraint& node)
		{
			if (node.kind == NormalizedConstraint::Kind::Atom)
			{
				return ClauseSet {
					{
						AtomKey(node.atom)
				}};
			}

			if (node.kind == NormalizedConstraint::Kind::All)
			{
				ClauseSet out;
				for (const auto& operand : node.operands)
				{
					auto part = ToCnf(operand);
					if (!part)
					{
						return std::nullopt;
					}

					out.insert(out.end(), part->begin(), part->end());
					if (out.size() > kMaxNormalClauses)
					{
						return std::nullopt;
					}
				}

				return out;
			}

			ClauseSet out {{}};
			for (const auto& operand : node.operands)
			{
				auto part = ToCnf(operand);
				if (!part)
				{
					return std::nullopt;
				}

				ClauseSet next;
				for (const auto& have : out)
				{
					for (const auto& more : *part)
					{
						auto combined = have;
						combined.insert(combined.end(), more.begin(), more.end());
						next.push_back(std::move(combined));
						if (next.size() > kMaxNormalClauses)
						{
							return std::nullopt;
						}
					}
				}

				out = std::move(next);
			}

			return out;
		}

	} // namespace concepts

	SemanticRules::NormalizedConstraint SemanticRules::NormalizeConstraint(
		const SemanticModel& model,
		std::uint32_t node)
	{
		TokenView view(model);
		const auto& nodes = model.Tree().NodesSoA();
		std::size_t begin = 0;
		std::size_t end   = 0;
		std::string provenance;
		if (node < nodes.size())
		{
			const auto range = view.Range(node);
			begin            = range.first;
			end              = range.second;
			if (nodes.Kind(node) == GrammarKind::RequiresClause && begin < end)
			{
				provenance = "clause@" + std::to_string(view.Offset(begin));
				++begin; // Skip `requires`.
			}
			else if (nodes.Kind(node) == GrammarKind::ConceptDefinition)
			{
				const auto body = concepts::ConceptBody(view, node);
				begin           = body.first;
				end             = body.second;
				for (auto position = range.first; position < range.second; ++position)
				{
					if (view.IsWord(position))
					{
						provenance = "concept:" + std::string(view.Text(position));
						break;
					}
				}
			}
		}

		if (begin >= end)
		{
			NormalizedConstraint empty;
			empty.kind            = NormalizedConstraint::Kind::Atom;
			empty.atom.provenance = std::move(provenance);
			return empty;
		}

		concepts::NormContext context {&model, &view, provenance, {}, {}, {}, 0};
		return concepts::NormalizeRange(context, begin, end);
	}

	bool SemanticRules::ConstraintSubsumes(
		const SemanticRules::NormalizedConstraint& general,
		const SemanticRules::NormalizedConstraint& specific)
	{
		const auto dnf = concepts::ToDnf(general);
		const auto cnf = concepts::ToCnf(specific);
		if (!dnf ||!cnf)
		{
			return false;
		}

		for (const auto& disjunct : *dnf)
		{
			for (const auto& conjunct : *cnf)
			{
				bool covered = false;
				for (const auto& left : disjunct)
				{
					for (const auto& right : conjunct)
					{
						covered |= left == right;
					}
				}

				if (!covered)
				{
					return false;
				}
			}
		}

		return true;
	}

	SemanticRules::NormalizedConstraint SemanticRules::NormalizeAssociatedConstraints(
		const SemanticModel& model,
		SymbolId function)
	{
		SemanticRules::NormalizedConstraint associated;
		associated.kind     = SemanticRules::NormalizedConstraint::Kind::All;
		const auto& symbols = model.Symbols();
		const auto& nodes   = model.Tree().NodesSoA();
		if (function >= symbols.Size() ||
			(symbols.kind[function] != SymbolKind::Function &&
			symbols.kind[function] != SymbolKind::Concept))
		{
			return associated;
		}

		const TokenView view(model);
		const auto decl = symbols.decl_node[function];
		if (decl >= nodes.size())
		{
			return associated;
		}

		// (1) Constrained template parameters and (2) the requires-clause
		// after `<>` live under the TemplateDeclaration.
		const auto parent = nodes.Parent(decl);
		if (parent < nodes.size() && nodes.Kind(parent) == GrammarKind::TemplateDeclaration)
		{
			const auto[tdBegin, tdEnd]     = view.Range(parent);
			const auto[declBegin, declEnd] = view.Range(decl);
			if (declBegin > tdBegin)
			{
				auto head = tdBegin;
				while (head < declBegin && view.At(head) != Tok::Lt)
				{
					++head;
				}

				const auto headClose =
					head < declBegin ? MatchAngleClose(view, head, declBegin) : declBegin;
				if (head < headClose)
				{
					for (const auto& part : SplitTopLevel(view, head + 1, headClose, false))
					{
						const auto noDefault = SplitTopLevel(view, part.first, part.second, true);
						const auto& core     = noDefault.empty() ? part : noDefault.front();
						std::size_t words    = 0;
						std::size_t lastWord = core.first;
						for (auto position = core.first; position < core.second;)
						{
							if (view.At(position) == Tok::Lt)
							{
								const auto close = view.MatchAngle(position, core.second);
								position         = close < core.second ? close + 1 : core.second;
								continue;
							}

							if (view.IsWord(position))
							{
								++words;
								lastWord = position;
							}

							++position;
						}

						if (words >= 2 && lastWord > core.first)
						{
							SemanticRules::NormalizedConstraint atom;
							atom.kind            = NormalizedConstraint::Kind::Atom;
							atom.atom.provenance = "template-head";
							atom.atom.expression = view.Spell(core.first, lastWord);
							associated.operands.push_back(std::move(atom));
						}
					}
				}
			}

			for (const auto child : model.ChildrenOf(parent))
			{
				if (nodes.Kind(child) == GrammarKind::RequiresClause)
				{
					associated.operands.push_back(NormalizeConstraint(model, child));
				}
			}
		}

		// (3) Abbreviated parameters: `Concept auto` placeholders.
		for (const auto child : model.ChildrenOf(decl))
		{
			if (nodes.Kind(child) != GrammarKind::Declarator)
			{
				continue;
			}

			for (const auto declaratorChild : model.ChildrenOf(child))
			{
				if (nodes.Kind(declaratorChild) != GrammarKind::FunctionSuffix)
				{
					continue;
				}

				for (const auto param : model.ChildrenOf(declaratorChild))
				{
					if (nodes.Kind(param) != GrammarKind::ParameterDeclaration)
					{
						continue;
					}

					for (const auto paramChild : model.ChildrenOf(param))
					{
						if (nodes.Kind(paramChild) != GrammarKind::TypeSpecifier)
						{
							continue;
						}

						const auto[specBegin, specEnd] = view.Range(paramChild);
						for (auto position = specBegin + 1; position < specEnd; ++position)
						{
							if ((view.At(position) == Tok::KwAuto ||
								view.At(position) == Tok::KwDecltype) &&
								view.IsWord(position - 1))
							{
								SemanticRules::NormalizedConstraint atom;
								atom.kind            = NormalizedConstraint::Kind::Atom;
								atom.atom.provenance = "abbreviated";
								atom.atom.expression = view.Spell(specBegin, specEnd);
								associated.operands.push_back(std::move(atom));
								break;
							}
						}
					}
				}
			}
		}

		// (4) Trailing requires-clause.
		for (std::uint32_t node = 0; node < nodes.size(); ++node)
		{
			if (nodes.Kind(node) != GrammarKind::RequiresClause)
			{
				continue;
			}

			if (ClauseFunction(model, node) == decl)
			{
				associated.operands.push_back(NormalizeConstraint(model, node));
			}
		}

		return associated;
	}

} // namespace heimdall
