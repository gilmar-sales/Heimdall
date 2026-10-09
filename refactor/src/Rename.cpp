#include <Heimdall/Refactoring.hpp>
#include "Support.hpp"

#include <algorithm>

namespace heimdall
{
    namespace
    {
        RefactoringError Error(RefactoringErrorCode code, std::string message)
        {
            return {code, std::move(message)};
        }

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
            constexpr std::string_view alternatives[] = {"and", "and_eq", "bitand", "bitor", "compl",
                "not", "not_eq", "or", "or_eq", "xor", "xor_eq"};
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
            const auto& soa = model.Tree().NodesSoA();
            const auto outermost_body =[&](ScopeId body, ScopeId owner)
            {
                if (body >= scopes.Size() || owner >= scopes.Size() || scopes.kind[body] != ScopeKind::Block ||
                    scopes.parent[body] != owner)
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
                return kind == GrammarKind::FunctionDefinition || kind == GrammarKind::IfStatement ||
                    kind == GrammarKind::LoopStatement || kind == GrammarKind::SwitchStatement;
            };
            // C++ forbids redeclaring a parameter/condition variable in the
            // outermost body, even though the binder gives that body a scope.
            // Two sibling bodies must remain independent from each other.
            return outermost_body(a, b) || outermost_body(b, a);
        }

        ScopeId ScopeAt(const SemanticModel& model, std::uint32_t token)
        {
            const auto& soa = model.Tree().NodesSoA();
            NodeId innermost = 0;
            for (NodeId node = 0; node < soa.size(); ++node)
            {
                if (token >= soa.FirstToken(node) && token - soa.FirstToken(node) < soa.TokenCount(node))
                {
                    innermost = node;
                }
            }

            return model.ScopeOfNode(innermost);
        }

        std::expected<void, RefactoringError> CheckNameScopes(
            const SemanticModel& model,
            SymbolId target,
            std::string_view name,
            std::stop_token stop)
        {
            const auto& symbols = model.Symbols();
            const auto scope = symbols.scope[target];
            for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
            {
                if (stop.stop_requested())
                {
                    return std::unexpected(Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
                }

                if (symbol != target && model.Names().Text(symbols.name[symbol]) == name &&
                    SameDeclarationScope(model, scope, symbols.scope[symbol]))
                {
                    return std::unexpected(Error(RefactoringErrorCode::NameCollision,
                        "The new name is already declared in the same scope"));
                }
            }

            const auto& tree = model.Tree();
            const auto& significant = model.Significant();
            for (std::uint32_t token = symbols.decl_token[target] + 1; token < tree.Tokens().size(); ++token)
            {
                if (stop.stop_requested())
                {
                    return std::unexpected(Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
                }

                if (!model.IsCode(token) || tree.Tokens()[token].kind != TokenKind::Identifier || tree.Text(tree.Tokens()[token]) != name ||
                    std::ranges::find(symbols.decl_token, token) != symbols.decl_token.end() ||
                    std::binary_search(model.Refs().token.begin(), model.Refs().token.end(), token))
                {
                    continue;
                }

                const auto at = std::lower_bound(significant.begin(), significant.end(), token);
                if (at != significant.begin())
                {
                    const auto previous = tree.Tokens()[*(at - 1)].tok;
                    if (previous == Tok::Dot || previous == Tok::Arrow || previous == Tok::ColonColon)
                    {
                        continue;
                    }
                }

                const auto use_scope = ScopeAt(model, token);
                if (!Within(model, use_scope, scope))
                {
                    continue;
                }

                const auto existing = model.Lookup(use_scope, model.Names().Find(name), token);
                if (existing != kNone && Within(model, symbols.scope[existing], scope))
                {
                    continue;
                }

                // Rebind cannot certify an occurrence omitted by the binder.
                // Block only when this declaration could actually capture it.
                return std::unexpected(Error(RefactoringErrorCode::IncompleteAnalysis,
                    "An occurrence of the new name in an affected scope is not modeled"));
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
            const auto tree = snapshot.Syntax(document);
            if (!source ||!tree || offset >= source->size())
            {
                return std::unexpected(Error(RefactoringErrorCode::InvalidSelection, "Select an identifier"));
            }

            if (!tree->Diagnostics().empty())
            {
                return std::unexpected(Error(RefactoringErrorCode::IncompleteAnalysis,
                    "Document has parse errors"));
            }

            const auto model = snapshot.Semantic(document);
            const auto& symbols = model->Symbols();
            const auto& tokens = tree->Tokens();
            std::uint32_t selected = kNone;
            for (std::uint32_t i = 0; i < tokens.size(); ++i)
            {
                if (offset >= tokens[i].offset && offset - tokens[i].offset < tokens[i].length &&
                    tokens[i].kind == TokenKind::Identifier && tokens[i].tok == Tok::None)
                {
                    selected = i;
                    break;
                }
            }

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

            if (target == kNone ||(symbols.kind[target] != SymbolKind::Variable &&
                symbols.kind[target] != SymbolKind::Parameter))
            {
                return std::unexpected(Error(RefactoringErrorCode::UnsupportedSymbol,
                    "Only local variables and parameters are supported"));
            }

            const auto& scopes = model->Scopes();
            ScopeId function = symbols.scope[target];
            while (function < scopes.Size() && scopes.kind[function] == ScopeKind::Block)
            {
                function = scopes.parent[function];
            }

            if (function >= scopes.Size() || scopes.kind[function] != ScopeKind::Function)
            {
                return std::unexpected(Error(RefactoringErrorCode::UnsupportedSymbol, "Symbol is not local"));
            }

            const auto& soa = tree->NodesSoA();
            const auto node = scopes.node[function];
            // Lambdas/captures and templates have uses the binder does not model.
            for (auto i = node; i < soa.SubtreeEnd(node); ++i)
            {
                if (soa.Kind(i) == GrammarKind::LambdaExpression ||
                    soa.Kind(i) == GrammarKind::TemplateDeclaration ||
                    soa.Kind(i) == GrammarKind::RequiresExpression)
                {
                    return std::unexpected(Error(RefactoringErrorCode::IncompleteAnalysis,
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

            const auto begin = soa.FirstToken(node);
            const auto end = begin + soa.TokenCount(node);
            const auto function_start = tokens[begin].offset;
            const auto function_end = tokens[end - 1].offset + tokens[end - 1].length;
            for (const auto& directive : tree->Directives())
            {
                if (directive.kind == DirectiveKind::Include && directive.offset >= function_start &&
                    directive.offset < function_end)
                {
                    return std::unexpected(Error(RefactoringErrorCode::MacroContext,
                        "Includes inside a function are not supported for rename"));
                }
            }

            SymbolOccurrences result{target, tokens[selected].offset, tokens[selected].length,
                std::string(model->Names().Text(symbols.name[target])), {}};
            if (include_declaration)
            {
                result.tokens.push_back(symbols.decl_token[target]);
            }

            for (std::uint32_t i = begin; i < end && i < tokens.size(); ++i)
            {
                if (stop.stop_requested())
                {
                    return std::unexpected(Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
                }

                if (tokens[i].tok == Tok::KwAsm || tokens[i].tok == Tok::KwCoAwait ||
                    tokens[i].tok == Tok::KwCoYield || tokens[i].tok == Tok::KwCoReturn)
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
                    const bool declaration = std::ranges::find(symbols.decl_token, i) != symbols.decl_token.end();
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
                const auto verified = refactor_detail::VerifyPreprocessing(snapshot, document, *source, stop);
                if (!verified)
                {
                    return std::unexpected(verified.error());
                }
            }

            return result;
        }
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
            return std::unexpected(refs.error());
        }

        const auto tree = snapshot.Syntax(document);
        const auto model = snapshot.Semantic(document);
        if (new_name != refs->name)
        {
            const auto checked = CheckNameScopes(*model, refs->symbol, new_name, stop);
            if (!checked)
            {
                return std::unexpected(checked.error());
            }
        }

        RefactoringPlan plan{"Rename " + refs->name + " to " + std::string(new_name), snapshot.Revision(),
                {}};
        DocumentEdits edits{document, snapshot.Path(document), snapshot.Version(document),
            snapshot.Source(document), {},
            snapshot.Options(document)};
        if (new_name != refs->name)
            for (auto index : refs->tokens)
        {
            const auto& token = tree->Tokens()[index];
            edits.edits.push_back({token.offset, token.length, refs->name, std::string(new_name)});
        }

        plan.documents.push_back(std::move(edits));
        const auto preview = PreviewRefactoring(snapshot, plan, stop);
        if (!preview)
        {
            return std::unexpected(preview.error());
        }

        if (!tree->Directives().empty() ||!snapshot.Options(document).Macros().empty())
        {
            const auto verified = refactor_detail::VerifyPreprocessing(snapshot, document,
                preview->front().source, stop);
            if (!verified)
            {
                return std::unexpected(verified.error());
            }
        }

        const auto after_tree = ParseTree::Parse(preview->front().source, snapshot.Options(document), stop);
        if (after_tree.Cancelled() || stop.stop_requested())
        {
            return std::unexpected(Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
        }

        if (!after_tree.Diagnostics().empty())
        {
            return std::unexpected(Error(RefactoringErrorCode::SemanticChange,
                "Rename introduces parse errors"));
        }

        const auto after = Binder::Bind(after_tree);
        const auto& before_symbols = model->Symbols();
        const auto& after_symbols = after.Symbols();
        if (after_tree.Tokens().size() != tree->Tokens().size() || after_symbols.Size() != before_symbols.Size() ||
            after.Refs().token != model->Refs().token)
        {
            return std::unexpected(Error(RefactoringErrorCode::SemanticChange,
                "Rename changes symbol bindings"));
        }

        if (after.Refs().target != model->Refs().target)
        {
            return std::unexpected(Error(RefactoringErrorCode::NameCollision,
                "The new name changes lookup in an affected scope"));
        }

        for (SymbolId i = 0; i < before_symbols.Size(); ++i)
        {
            const auto expected = i == refs->symbol ? new_name : model->Names().Text(before_symbols.name[i]);
            if (after.Names().Text(after_symbols.name[i]) != expected ||
                before_symbols.decl_token[i] != after_symbols.decl_token[i] ||
                before_symbols.scope[i] != after_symbols.scope[i] ||
                before_symbols.kind[i] != after_symbols.kind[i])
            {
                return std::unexpected(Error(RefactoringErrorCode::SemanticChange, "Rename changes declarations"));
            }
        }

        if (stop.stop_requested())
        {
            return std::unexpected(Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
        }

        return plan;
    }
}
