#include "Support.hpp"
#include <Heimdall/Refactoring.hpp>

#include <algorithm>
#include <unordered_set>

namespace heimdall
{
    namespace
    {
        RefactoringError Error(RefactoringErrorCode code, std::string message)
        {
            return { code, std::move(message) };
        }

        bool Identifier(std::string_view name)
        {
            // Start with portable identifiers. Reserved names are not generated.
            if (name.empty() || name.front() == '_' || name.find("__") != std::string_view::npos)
            {
                return false;
            }

            const auto alpha = [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
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
                "and",    "and_eq", "bitand", "bitor", "compl", "not",
                "not_eq", "or",     "or_eq",  "xor",   "xor_eq"
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

            const auto& scopes         = model.Scopes();
            const auto& soa            = model.Tree().NodesSoA();
            const auto  outermost_body = [&](ScopeId body, ScopeId owner) {
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
            const auto& soa       = model.Tree().NodesSoA();
            NodeId      innermost = 0;
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
        bool FunctionTokenRange(const SemanticModel& model, ScopeId scope, std::uint32_t& begin,
                                std::uint32_t& end)
        {
            const auto& scopes   = model.Scopes();
            ScopeId     function = scope;
            while (function < scopes.Size() && scopes.kind[function] == ScopeKind::Block)
            {
                function = scopes.parent[function];
            }

            if (function >= scopes.Size() || scopes.kind[function] != ScopeKind::Function)
            {
                return false;
            }

            const auto& soa  = model.Tree().NodesSoA();
            const auto  node = scopes.node[function];
            if (node >= soa.size())
            {
                return false;
            }

            begin = soa.FirstToken(node);
            end   = begin + soa.TokenCount(node);
            return true;
        }

        std::expected<void, RefactoringError> CheckNameScopes(const SemanticModel& model,
                                                              SymbolId             target,
                                                              std::string_view     name,
                                                              std::stop_token      stop)
        {
            const auto& symbols = model.Symbols();
            const auto  scope   = symbols.scope[target];
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
                    declarations.find(token) != declarations.end() ||
                    std::binary_search(model.Refs().token.begin(), model.Refs().token.end(), token))
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

                const auto use_scope = ScopeAt(model, token);
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

        std::expected<SymbolOccurrences, RefactoringError> References(
            const AnalysisSnapshot& snapshot,
            DocumentId              document,
            std::size_t             offset,
            bool                    include_declaration,
            std::stop_token         stop)
        {
            if (stop.stop_requested())
            {
                return std::unexpected(Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
            }

            const auto source = snapshot.Source(document);
            const auto tree   = snapshot.Syntax(document);
            if (!source || !tree || offset >= source->size())
            {
                return std::unexpected(
                    Error(RefactoringErrorCode::InvalidSelection, "Select an identifier"));
            }

            if (!tree->Diagnostics().empty())
            {
                return std::unexpected(
                    Error(RefactoringErrorCode::IncompleteAnalysis, "Document has parse errors"));
            }

            const auto    model    = snapshot.Semantic(document);
            const auto&   symbols  = model->Symbols();
            const auto&   tokens   = tree->Tokens();
            std::uint32_t selected = kNone;
            // Tokens are emitted in offset order: binary search the container
            // instead of scanning from the start of the file.
            {
                const auto matches = [&](const Token& token) {
                    return token.kind == TokenKind::Identifier && token.tok == Tok::None;
                };
                const auto begin = tokens.begin(), end = tokens.end();
                const auto after = std::upper_bound(
                    begin, end, offset, [](std::size_t value, const Token& token) {
                        return value < token.offset;
                    });
                if (after != begin)
                {
                    const auto candidate = std::prev(after);
                    if (candidate->offset <= offset &&
                        offset - candidate->offset < candidate->length && matches(*candidate))
                    {
                        selected = static_cast<std::uint32_t>(candidate - begin);
                    }
                }

                if (selected == kNone)
                {
                    // Defensive fallback: identical to the old linear scan.
                    for (std::uint32_t i = 0; i < tokens.size(); ++i)
                    {
                        if (offset >= tokens[i].offset &&
                            offset - tokens[i].offset < tokens[i].length && matches(tokens[i]))
                        {
                            selected = i;
                            break;
                        }
                    }
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

            if (target == kNone || (symbols.kind[target] != SymbolKind::Variable &&
                                    symbols.kind[target] != SymbolKind::Parameter))
            {
                return std::unexpected(Error(RefactoringErrorCode::UnsupportedSymbol,
                                             "Only local variables and parameters are supported"));
            }

            const auto& scopes   = model->Scopes();
            ScopeId     function = symbols.scope[target];
            while (function < scopes.Size() && scopes.kind[function] == ScopeKind::Block)
            {
                function = scopes.parent[function];
            }

            if (function >= scopes.Size() || scopes.kind[function] != ScopeKind::Function)
            {
                return std::unexpected(
                    Error(RefactoringErrorCode::UnsupportedSymbol, "Symbol is not local"));
            }

            const auto& soa  = tree->NodesSoA();
            const auto  node = scopes.node[function];
            // Lambdas/captures and templates have uses the binder does not model.
            for (auto i = node; i < soa.SubtreeEnd(node); ++i)
            {
                if (soa.Kind(i) == GrammarKind::LambdaExpression ||
                    soa.Kind(i) == GrammarKind::TemplateDeclaration ||
                    soa.Kind(i) == GrammarKind::RequiresExpression)
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

            SymbolOccurrences result { target,
                                       tokens[selected].offset,
                                       tokens[selected].length,
                                       std::string(model->Names().Text(symbols.name[target])),
                                       {} };
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
            if (!tree->Directives().empty() || !snapshot.Options(document).Macros().empty())
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

    std::expected<SymbolOccurrences, RefactoringError> RefactoringService::LocalReferences(
        const AnalysisSnapshot& snapshot,
        DocumentId              document,
        std::size_t             offset,
        bool                    include_declaration,
        std::stop_token         stop)
    {
        return References(snapshot, document, offset, include_declaration, stop);
    }

    bool refactor_detail::ValidName(std::string_view name)
    {
        return Identifier(name);
    }

    std::expected<RefactoringPlan, RefactoringError> RefactoringService::RenameLocal(
        const AnalysisSnapshot& snapshot,
        DocumentId              document,
        std::size_t             offset,
        std::string_view        new_name,
        std::stop_token         stop)
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

        const auto tree  = snapshot.Syntax(document);
        const auto model = snapshot.Semantic(document);
        if (new_name != refs->name)
        {
            const auto checked = CheckNameScopes(*model, refs->symbol, new_name, stop);
            if (!checked)
            {
                return std::unexpected(checked.error());
            }
        }

        RefactoringPlan plan { "Rename " + refs->name + " to " + std::string(new_name),
                               snapshot.Revision(),
                               {} };
        DocumentEdits   edits { document,
                                snapshot.Path(document),
                                snapshot.Version(document),
                                snapshot.Source(document),
                                {},
                                snapshot.Options(document) };
        if (new_name != refs->name)
        {
            edits.edits.reserve(refs->tokens.size());
            for (auto index : refs->tokens)
            {
                const auto& token = tree->Tokens()[index];
                edits.edits.push_back(
                    { token.offset, token.length, refs->name, std::string(new_name) });
            }
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

        if (!tree->Directives().empty() || !snapshot.Options(document).Macros().empty())
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
        ParseReuse        reuse {};
        const ParseReuse* reuse_ptr = nullptr;
        {
            const auto& applied  = plan.documents.back().edits;
            const auto  original = snapshot.Source(document);
            std::size_t old_start = applied.front().offset;
            std::size_t old_end   = old_start;
            for (const auto& edit : applied)
            {
                old_start = std::min(old_start, edit.offset);
                old_end   = std::max(old_end, edit.offset + edit.length);
            }

            reuse.previous   = tree.get();
            reuse.offset     = old_start;
            reuse.old_length = old_end - old_start;
            reuse.new_length =
                preview->front().source.size() - original->size() + reuse.old_length;
            reuse_ptr = &reuse;
        }

        const auto after_tree = ParseTree::Parse(
            preview->front().source, snapshot.Options(document), stop, nullptr, reuse_ptr);
        if (after_tree.Cancelled() || stop.stop_requested())
        {
            return std::unexpected(Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
        }

        if (!after_tree.Diagnostics().empty())
        {
            return std::unexpected(
                Error(RefactoringErrorCode::SemanticChange, "Rename introduces parse errors"));
        }

        const auto  after          = Binder::Bind(after_tree);
        const auto& before_symbols = model->Symbols();
        const auto& after_symbols  = after.Symbols();
        if (after_tree.Tokens().size() != tree->Tokens().size() ||
            after_symbols.Size() != before_symbols.Size() ||
            after.Refs().token != model->Refs().token)
        {
            return std::unexpected(
                Error(RefactoringErrorCode::SemanticChange, "Rename changes symbol bindings"));
        }

        if (after.Refs().target != model->Refs().target)
        {
            return std::unexpected(Error(RefactoringErrorCode::NameCollision,
                                         "The new name changes lookup in an affected scope"));
        }

        for (SymbolId i = 0; i < before_symbols.Size(); ++i)
        {
            const auto expected =
                i == refs->symbol ? new_name : model->Names().Text(before_symbols.name[i]);
            if (after.Names().Text(after_symbols.name[i]) != expected ||
                before_symbols.decl_token[i] != after_symbols.decl_token[i] ||
                before_symbols.scope[i] != after_symbols.scope[i] ||
                before_symbols.kind[i] != after_symbols.kind[i])
            {
                return std::unexpected(
                    Error(RefactoringErrorCode::SemanticChange, "Rename changes declarations"));
            }
        }

        if (stop.stop_requested())
        {
            return std::unexpected(Error(RefactoringErrorCode::Cancelled, "Request cancelled"));
        }

        return plan;
    }
} // namespace heimdall
