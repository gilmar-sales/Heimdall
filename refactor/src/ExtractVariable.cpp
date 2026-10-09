#include "Support.hpp"
#include <Heimdall/AnalysisContext.hpp>
#include <algorithm>

namespace heimdall
{
    namespace
    {
        RefactoringError Block(std::string message)
        {
            return { RefactoringErrorCode::IncompleteAnalysis, std::move(message) };
        }

        std::expected<AnalysisContext, RefactoringError> Context(
            const AnalysisSnapshot& snapshot, DocumentId document, std::stop_token stop)
        {
            if (stop.stop_requested())
            {
                return std::unexpected(
                    RefactoringError { RefactoringErrorCode::Cancelled, "Request cancelled" });
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

            if (!context.Syntax().Directives().empty() ||
                !snapshot.Options(document).Macros().empty())
            {
                return std::unexpected(RefactoringError {
                    RefactoringErrorCode::MacroContext,
                    "Preprocessor context is not supported for this operation" });
            }

            return context;
        }

        NodeId ExactNode(const AnalysisContext& context, std::size_t offset, std::size_t length)
        {
            const auto& soa    = context.Syntax().NodesSoA();
            NodeId      result = InvalidNode;
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
            const auto& soa      = context.Syntax().NodesSoA();
            NodeId      function = InvalidNode;
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

        // Checks only supported pure built-in expressions, never class operators,
        // calls, identifiers, string literals, or implicit reads of volatile data.
        bool ConstantScalar(const AnalysisContext& context, NodeId expression)
        {
            const auto& tree  = context.Syntax();
            const auto& soa   = tree.NodesSoA();
            const auto  type  = context.Types().NodeType(expression);
            const auto& types = context.Types().Types();
            if (!types.IsInteger(type) && !types.IsBool(type))
            {
                return false;
            }

            for (auto node = expression; node < soa.SubtreeEnd(expression); ++node)
            {
                // The parser represents true/false as identifier expressions.
                const auto range           = context.NodeRange(node);
                const auto spelling        = tree.Source().substr(range.offset, range.length);
                const bool boolean_literal = soa.Kind(node) == GrammarKind::IdentifierExpression &&
                                             (spelling == "true" || spelling == "false");
                if (soa.Kind(node) != GrammarKind::LiteralExpression &&
                    soa.Kind(node) != GrammarKind::BinaryExpression &&
                    soa.Kind(node) != GrammarKind::UnaryExpression &&
                    soa.Kind(node) != GrammarKind::ParenthesizedExpression && !boolean_literal)
                {
                    return false;
                }

                // A bool result may still contain floating-point arithmetic.
                // Materialization can change excess precision or fenv effects.
                const auto operand_type = context.Types().NodeType(node);
                if (!types.IsInteger(operand_type) && !types.IsBool(operand_type))
                {
                    return false;
                }
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
                    token.kind == TokenKind::CharacterLiteral)
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
                        break;
                    default:
                        return false;
                }
            }

            return true;
        }

        std::expected<RefactoringPlan, RefactoringError> ValidateSyntax(
            const AnalysisSnapshot& snapshot, RefactoringPlan plan, std::stop_token stop)
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
                        RefactoringError { RefactoringErrorCode::Cancelled, "Request cancelled" });
                }

                if (!tree.Diagnostics().empty())
                {
                    return std::unexpected(
                        RefactoringError { RefactoringErrorCode::SemanticChange,
                                           "Transformation introduces parse errors" });
                }

                const auto  before       = snapshot.Semantic(document.document);
                const auto  after        = Binder::Bind(tree);
                const auto  after_types  = Typer::Type(after);
                const auto  before_types = snapshot.Types(document.document);
                const auto& changes      = plan.documents.front().edits;
                const auto  mapped       = [&](std::size_t offset) -> std::optional<std::size_t> {
                    std::size_t result = offset;
                    for (const auto& edit : changes)
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
                const auto&           before_tokens = before->Tree().Tokens();
                const auto&           after_tokens  = tree.Tokens();
                for (SymbolId i = 0; i < before->Symbols().Size(); ++i)
                {
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
                            "Transformation changes an existing declaration or type" });
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
                            RefactoringError { RefactoringErrorCode::SemanticChange,
                                               "Transformation loses an existing reference" });
                    }

                    const auto target   = before->Refs().target[i];
                    const auto expected = target == kNone ? kNone : identities[target];
                    if (after.ResolveToken(
                            static_cast<std::uint32_t>(token - after_tokens.begin())) != expected ||
                        (target != kNone && expected == kNone))
                    {
                        return std::unexpected(RefactoringError {
                            RefactoringErrorCode::SemanticChange,
                            "Transformation changes an existing reference binding" });
                    }
                }
            }

            if (stop.stop_requested())
            {
                return std::unexpected(
                    RefactoringError { RefactoringErrorCode::Cancelled, "Request cancelled" });
            }

            return plan;
        }

        RefactoringPlan Plan(const AnalysisContext& context, std::string title,
                             std::vector<SourceEdit> edits)
        {
            const auto& snapshot = context.Snapshot();
            const auto  id       = context.Document();
            return { std::move(title),
                     snapshot.Revision(),
                     { { id, snapshot.Path(id), snapshot.Version(id), snapshot.Source(id),
                         std::move(edits), snapshot.Options(id) } } };
        }
    } // namespace

    std::expected<RefactoringPlan, RefactoringError> RefactoringService::ExtractVariable(
        const AnalysisSnapshot& snapshot,
        DocumentId              document,
        std::size_t             offset,
        std::size_t             length,
        std::string_view        new_name,
        std::stop_token         stop)
    {
        if (!refactor_detail::ValidName(new_name))
        {
            return std::unexpected(
                RefactoringError { RefactoringErrorCode::InvalidName, "Invalid C++ identifier" });
        }

        auto context = Context(snapshot, document, stop);
        if (!context)
        {
            return std::unexpected(context.error());
        }

        const auto& tree = context->Syntax();
        const auto& soa  = tree.NodesSoA();
        const auto  node = ExactNode(*context, offset, length);
        if (node == InvalidNode || !ConstantScalar(*context, node))
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

        // The inserted initializer must not be crossed by a goto or a later
        // case label. The parser does not yet provide a jump-target proof.
        for (auto i = function; i < soa.SubtreeEnd(function); ++i)
        {
            if (soa.Kind(i) == GrammarKind::CaseLabel)
            {
                return std::unexpected(
                    Block("Extraction in functions with case labels needs jump analysis"));
            }
        }

        for (auto i = soa.FirstToken(function);
             i < soa.FirstToken(function) + soa.TokenCount(function); ++i)
        {
            if (tree.Tokens()[i].tok == Tok::KwGoto)
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
                    RefactoringError { RefactoringErrorCode::NameCollision,
                                       "The new name already occurs in this document" });
            }
        }

        const auto  range       = context->NodeRange(statement);
        const auto  source      = tree.Source();
        const auto  line        = range.offset == 0 ? 0 : source.rfind('\n', range.offset - 1) + 1;
        const auto  prefix      = source.substr(line, range.offset - line);
        const bool  own_line    = prefix.find_first_not_of(" \t") == std::string_view::npos;
        const auto  expression  = std::string(source.substr(offset, length));
        const auto  newline     = source.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
        std::string declaration = "auto " + std::string(new_name) + " = " + expression + ";";
        declaration += own_line ? std::string(newline) + std::string(prefix) : " ";
        auto plan = Plan(*context, "Extract variable '" + std::string(new_name) + "'",
                         { { range.offset, 0, {}, std::move(declaration) },
                           { offset, length, expression, std::string(new_name) } });
        return ValidateSyntax(snapshot, std::move(plan), stop);
    }

    std::expected<RefactoringPlan, RefactoringError> RefactoringService::InlineVariable(
        const AnalysisSnapshot& snapshot,
        DocumentId              document,
        std::size_t             offset,
        std::stop_token         stop)
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

        const auto& tree        = context->Syntax();
        const auto& soa         = tree.NodesSoA();
        NodeId      declaration = symbols.decl_node[refs->symbol];
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

        if (literal == InvalidNode || !ConstantScalar(*context, literal) ||
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
            { declaration_range.offset,
              declaration_range.length,
              std::string(tree.Source().substr(declaration_range.offset, declaration_range.length)),
              {} }
        };
        for (auto token : refs->tokens)
        {
            if (token == symbols.decl_token[refs->symbol])
            {
                continue;
            }

            const auto& use        = tree.Tokens()[token];
            const auto  expression = ExactNode(*context, use.offset, use.length);
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

            edits.push_back({ use.offset, use.length, refs->name, value });
        }

        return ValidateSyntax(
            snapshot, Plan(*context, "Inline variable '" + refs->name + "'", std::move(edits)),
            stop);
    }

    std::expected<RefactoringPlan, RefactoringError> RefactoringService::ExtractFunction(
        const AnalysisSnapshot& snapshot,
        DocumentId              document,
        std::size_t             offset,
        std::size_t             length,
        std::string_view        new_name,
        std::stop_token         stop)
    {
        if (!refactor_detail::ValidName(new_name))
        {
            return std::unexpected(
                RefactoringError { RefactoringErrorCode::InvalidName, "Invalid C++ identifier" });
        }

        auto context = Context(snapshot, document, stop);
        if (!context)
        {
            return std::unexpected(context.error());
        }

        const auto& tree = context->Syntax();
        const auto& soa  = tree.NodesSoA();
        const auto  node = ExactNode(*context, offset, length);
        // A literal is always a valid constant initializer of its builtin type.
        // General expressions need constant evaluation and effect analysis first.
        if (node == InvalidNode || soa.Kind(node) != GrammarKind::LiteralExpression ||
            !ConstantScalar(*context, node) || !OrdinaryFunction(*context, node))
        {
            return std::unexpected(
                Block("Function extraction currently supports a complete scalar literal return"));
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

        for (const auto& token : tree.Tokens())
        {
            if (token.kind == TokenKind::Identifier && tree.Text(token) == new_name)
            {
                return std::unexpected(
                    RefactoringError { RefactoringErrorCode::NameCollision,
                                       "The new name already occurs in this document" });
            }
        }

        const auto source         = tree.Source();
        const auto function_range = context->NodeRange(function);
        const auto line =
            function_range.offset == 0 ? 0 : source.rfind('\n', function_range.offset - 1) + 1;
        const auto        prefix   = source.substr(line, function_range.offset - line);
        const bool        own_line = prefix.find_first_not_of(" \t") == std::string_view::npos;
        const std::string newline  = source.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
        const auto        value    = std::string(source.substr(offset, length));
        const auto        type     = context->Types().Spell(context->Types().NodeType(node));
        std::string       definition =
            "static constexpr " + type + " " + std::string(new_name) + "() noexcept" + newline;
        definition += std::string(own_line ? prefix : std::string_view {}) + "{" + newline;
        definition += std::string(own_line ? prefix : std::string_view {}) + "    return " + value +
                      ";" + newline;
        definition +=
            std::string(own_line ? prefix : std::string_view {}) + "}" + newline + newline;
        if (own_line)
        {
            definition += prefix;
        }

        return ValidateSyntax(snapshot,
                              Plan(*context, "Extract function '" + std::string(new_name) + "'",
                                   { { function_range.offset, 0, {}, std::move(definition) },
                                     { offset, length, value, std::string(new_name) + "()" } }),
                              stop);
    }
} // namespace heimdall
