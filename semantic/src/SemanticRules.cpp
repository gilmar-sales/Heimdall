#include <Heimdall/LineTable.hpp>
#include <Heimdall/SemanticRules.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace heimdall
{

    namespace
    {

        constexpr std::uint32_t kExcluded = SymbolFlag::Static | SymbolFlag::Constructor | SymbolFlag::Destructor |
            SymbolFlag::Qualified | SymbolFlag::Friend | SymbolFlag::Override | SymbolFlag::Final |
            SymbolFlag::Template;
        constexpr std::uint32_t kVirtualish = SymbolFlag::Virtual | SymbolFlag::Override | SymbolFlag::Final;

        bool IsTrivia(const Token &token)
        {
            return token.kind == TokenKind::Whitespace || token.kind == TokenKind::LineComment ||
                token.kind == TokenKind::BlockComment;
        }

        // Visited marks shared by every query of one rule run: a query bumps the
        // epoch instead of clearing the array, so a query costs only the classes
        // it actually walks.
        struct Visited
        {
            explicit Visited(std::size_t symbols) : stamp(symbols, 0) {}
            bool Mark(SymbolId symbol)
            {
                return stamp[symbol] != epoch && (stamp[symbol] = epoch, true);
            }

            std::vector<std::uint32_t> stamp;
            std::uint32_t epoch = 0;
        };

        // A virtual function with the same name and signature in any ancestor
        // whose base chain is resolved. Returns the owning class symbol.
        SymbolId FindOverridden(const SemanticModel &model, Visited &visited, SymbolId derived, NameId name,
            std::uint64_t signature)
        {
            ++visited.epoch;
            const auto &symbols = model.Symbols();
            const auto &bases = model.Bases();
            std::vector<SymbolId> pending;
            const auto push_bases =[&](SymbolId klass)
            {
                for (std::uint32_t i = 0; i < symbols.base_count[klass]; ++i)
                {
                    if (const auto target = bases.target[symbols.first_base[klass] + i]; target != kNone)
                    {
                        pending.push_back(target);
                    }
                }
            };

            push_bases(derived);
            visited.Mark(derived);
            while (!pending.empty())
            {
                const auto klass = pending.back();
                pending.pop_back();
                if (!visited.Mark(klass))
                {
                    continue;
                }

                for (auto member = model.LookupLocal(symbols.member_scope[klass], name); member != kNone;
                    member = symbols.next_same_name[member])
                {
                    if (symbols.kind[member] == SymbolKind::Function && symbols.signature[member] == signature &&
                        (symbols.flags[member] & kVirtualish) != 0 &&
                        (symbols.flags[member] & (SymbolFlag::Static | SymbolFlag::Constructor)) == 0)
                    {
                        return klass;
                    }
                }

                push_bases(klass);
            }

            return kNone;
        }

        // End of the declarator: just after the parameter list and its
        // cv/ref/noexcept/trailing-return parts, where `override` is written.
        bool OverrideInsertionOffset(const ParseTree &tree, std::uint32_t name_token, std::size_t & offset)
        {
            const auto &tokens = tree.Tokens();
            const auto next =[&](std::size_t from)
            {
                while (from < tokens.size() && IsTrivia(tokens[from]))
                {
                    ++from;
                }

                return from;
            };

            std::size_t i = next(name_token + 1);
            if (i >= tokens.size() || tokens[i].tok != Tok::LParen)
            {
                return false;
            }

            std::size_t depth = 0;
            std::size_t last = i;
            for (; i < tokens.size(); i = next(i + 1))
            {
                if (tokens[i].tok == Tok::LParen)
                {
                    ++depth;
                }
                else if (tokens[i].tok == Tok::RParen && --depth == 0)
                {
                    last = i;
                    break;
                }
            }

            if (i >= tokens.size())
            {
                return false;
            }

            for (i = next(i + 1); i < tokens.size(); i = next(i + 1))
            {
                const Tok tok = tokens[i].tok;
                if (tok == Tok::Semi || tok == Tok::LBrace || tok == Tok::Eq || tok == Tok::KwFinal ||
                    tok == Tok::Colon || tok == Tok::KwTry || tok == Tok::KwRequires)
                {
                    break;
                }

                if (tok == Tok::KwNoexcept)
                {
                    const auto open = next(i + 1);
                    if (open < tokens.size() && tokens[open].tok == Tok::LParen)
                    {
                        std::size_t nested = 0;
                        for (i = open; i < tokens.size(); i = next(i + 1))
                        {
                            if (tokens[i].tok == Tok::LParen)
                            {
                                ++nested;
                            }
                            else if (tokens[i].tok == Tok::RParen && --nested == 0)
                            {
                                break;
                            }
                        }

                        if (i >= tokens.size())
                        {
                            return false;
                        }
                    }
                }

                last = i;
            }

            offset = tokens[last].offset + tokens[last].length;
            return true;
        }

    } // namespace

    std::vector<Diagnostic> SemanticRules::AnalyzeOverride(const SemanticModel &model)
    {
        std::vector<Diagnostic> diagnostics;
        const auto &symbols = model.Symbols();
        const auto &scopes = model.Scopes();
        const auto &tree = model.Tree();
        LineTable lines;
        bool lines_built = false;
        Visited visited(symbols.Size());
        for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
        {
            if (symbols.kind[symbol] != SymbolKind::Function ||(symbols.flags[symbol] & kExcluded) != 0 ||
                symbols.signature[symbol] == 0 || symbols.name[symbol] == kNone)
            {
                continue;
            }

            const auto scope = symbols.scope[symbol];
            if (scopes.kind[scope] != ScopeKind::Class || scopes.owner[scope] == kNone)
            {
                continue;
            }

            const auto klass = scopes.owner[scope];
            const auto base = FindOverridden(model, visited, klass, symbols.name[symbol],
                symbols.signature[symbol]);
            if (base == kNone)
            {
                continue;
            }

            const auto token = symbols.decl_token[symbol];
            std::size_t insert_at = 0;
            if (!OverrideInsertionOffset(tree, token, insert_at))
            {
                continue;
            }

            if (!lines_built)
            {
                lines.Build(tree.Source());
                lines_built = true;
            }

            const auto &name_token = tree.Tokens()[token];
            const auto position = lines.Lookup(name_token.offset);
            const std::string name(model.Names().Text(symbols.name[symbol]));
            Diagnostic diagnostic{RuleId::ModernizeOverride, Severity::Warning, "cpp/modernize-override",
                "'" + name + "' overrides a virtual function of '" +
                    std::string(model.Names().Text(symbols.name[base])) + "'; add 'override'",
                name_token.offset, name_token.length, position.line, position.column, true,
                TextEdit{insert_at, 0, " override"}};
            // The match is textual (name and parameter spelling): offered as a
            // quick fix, not applied in batch.
            diagnostic.fix_is_safe = false;
            diagnostic.fix_title = "Add 'override' to " + name;
            diagnostics.push_back(std::move(diagnostic));
        }

        std::sort(diagnostics.begin(), diagnostics.end(),
            [](const Diagnostic &a, const Diagnostic &b)
            {
                return a.offset < b.offset;
        });
        return diagnostics;
    }

    namespace
    {

        // Builds diagnostics for the rules below; the line table is built on first use.
        class Reporter
        {
        public:
            explicit Reporter(const SemanticModel &model) : m_model(model) {}

            void Report(RuleId rule, std::string_view code, std::string message, std::size_t offset,
                std::size_t length, std::size_t fix_offset, std::size_t fix_length, std::string replacement,
                bool safe, std::string title)
            {
                if (!m_lines_built)
                {
                    m_lines.Build(m_model.Tree().Source());
                    m_lines_built = true;
                }

                const auto position = m_lines.Lookup(offset);
                Diagnostic diagnostic {rule, Severity::Warning, std::string(code), std::move(message), offset,
                    length, position.line, position.column, true,
                    TextEdit {fix_offset, fix_length, std::move(replacement)}};
                diagnostic.fix_is_safe = safe;
                diagnostic.fix_title = std::move(title);
                m_diagnostics.push_back(std::move(diagnostic));
            }

            std::vector<Diagnostic> Take()
            {
                std::sort(m_diagnostics.begin(), m_diagnostics.end(),
                    [](const Diagnostic &a, const Diagnostic &b)
                    {
                        return a.offset < b.offset;
                    });
                return std::move(m_diagnostics);
            }

        private:
            const SemanticModel &m_model;
            LineTable m_lines;
            bool m_lines_built = false;
            std::vector<Diagnostic> m_diagnostics;
        };

        // The model's tokens, with the helpers every rule below needs.
        class TokenView
        {
        public:
            explicit TokenView(const SemanticModel &model)
            : m_model(model), m_tokens(model.Tree().Tokens()), m_sig(model.Significant())
            {
            }

            std::size_t Size() const
            {
                return m_sig.size();
            }
            std::uint32_t TokenAt(std::size_t position) const
            {
                return m_sig[position];
            }
            Tok At(std::size_t position) const
            {
                return position < m_sig.size() ? m_tokens[m_sig[position]].tok : Tok::None;
            }
            bool IsWord(std::size_t position) const
            {
                return position < m_sig.size() && m_tokens[m_sig[position]].kind == TokenKind::Identifier &&
                    m_tokens[m_sig[position]].tok == Tok::None;
            }
            std::string_view Text(std::size_t position) const
            {
                return position < m_sig.size() ? m_model.Tree().Text(m_tokens[m_sig[position]]) : std::string_view {};
            }
            std::size_t Offset(std::size_t position) const
            {
                return m_tokens[m_sig[position]].offset;
            }
            std::size_t End(std::size_t position) const
            {
                return m_tokens[m_sig[position]].offset + m_tokens[m_sig[position]].length;
            }
            // First position holding raw token `token` or a later one.
            std::size_t PositionOf(std::uint32_t token) const
            {
                return static_cast<std::size_t>(std::lower_bound(m_sig.begin(), m_sig.end(), token) - m_sig.begin());
            }
            // [begin, end) positions of a node's significant tokens.
            std::pair<std::size_t, std::size_t> Range(std::uint32_t node) const
            {
                const auto &n = m_model.Tree().Nodes()[node];
                const auto begin = PositionOf(n.first_token);
                return {begin, std::max(begin, PositionOf(n.first_token + n.token_count))};
            }
            // `0`, `0L`, `0u`, `0UL`...: a null pointer constant.
            bool IsZeroLiteral(std::size_t position) const
            {
                if (position >= m_sig.size() || m_tokens[m_sig[position]].kind != TokenKind::Number)
                {
                    return false;
                }

                const auto text = Text(position);
                return !text.empty() && text[0] == '0' &&
                    text.find_first_not_of("uUlL", 1) == std::string_view::npos;
            }
            // Position of the bracket closing the one at `open`, or `limit`.
            std::size_t Match(std::size_t open, std::size_t limit) const
            {
                const Tok opening = At(open);
                const Tok closing = opening == Tok::LParen ? Tok::RParen : opening == Tok::LBracket ? Tok::RBracket
                                                                                                     : Tok::RBrace;
                std::size_t depth = 0;
                for (std::size_t i = open; i < limit && i < m_sig.size(); ++i)
                {
                    if (At(i) == opening)
                    {
                        ++depth;
                    }
                    else if (At(i) == closing && --depth == 0)
                    {
                        return i;
                    }
                }

                return limit;
            }
            // Position of the `>` closing the `<` at `open`, or `limit`; `>>` is not
            // split, so nested templates ending in `>>` give up.
            std::size_t MatchAngle(std::size_t open, std::size_t limit) const
            {
                std::size_t depth = 0;
                for (std::size_t i = open; i < limit && i < m_sig.size(); ++i)
                {
                    const Tok tok = At(i);
                    if (tok == Tok::Lt)
                    {
                        ++depth;
                    }
                    else if (tok == Tok::Gt && --depth == 0)
                    {
                        return i;
                    }
                    else if (tok == Tok::Shr || tok == Tok::Semi || tok == Tok::LBrace)
                    {
                        break;
                    }
                }

                return limit;
            }
            // The tokens in [begin, end) as one string, for exact comparisons.
            std::string Spell(std::size_t begin, std::size_t end) const
            {
                std::string result;
                for (std::size_t i = begin; i < end; ++i)
                {
                    result += Text(i);
                    result += ' ';
                }

                return result;
            }

        private:
            const SemanticModel &m_model;
            const std::vector<Token> &m_tokens;
            const std::pmr::vector<std::uint32_t> &m_sig;
        };

        bool IsTypeWord(Tok tok)
        {
            switch (tok)
            {
            case Tok::KwConst:
            case Tok::KwVolatile:
            case Tok::KwStruct:
            case Tok::KwClass:
            case Tok::KwUnion:
            case Tok::KwEnum:
            case Tok::KwUnsigned:
            case Tok::KwSigned:
            case Tok::KwInt:
            case Tok::KwLong:
            case Tok::KwShort:
            case Tok::KwChar:
            case Tok::KwBool:
            case Tok::KwFloat:
            case Tok::KwDouble:
            case Tok::KwVoid:
            case Tok::KwWchar:
            case Tok::KwChar8:
            case Tok::KwChar16:
            case Tok::KwChar32:
            case Tok::KwTypename:
                return true;
            default:
                return false;
            }
        }

        // `[first, last)` spell a pointer type: type words, names, `::`, template
        // arguments and `*`, ending in `*` (a trailing cv-qualifier is allowed).
        bool IsPointerType(const TokenView &view, std::size_t first, std::size_t last)
        {
            if (first >= last)
            {
                return false;
            }

            bool star = false;
            std::size_t end = last;
            while (end > first && (view.At(end - 1) == Tok::KwConst || view.At(end - 1) == Tok::KwVolatile))
            {
                --end;
            }

            if (end == first || view.At(end - 1) != Tok::Star)
            {
                return false;
            }

            for (std::size_t i = first; i < last; ++i)
            {
                const Tok tok = view.At(i);
                if (tok == Tok::Star)
                {
                    star = true;
                }
                else if (!(view.IsWord(i) || IsTypeWord(tok) || tok == Tok::ColonColon || tok == Tok::Lt ||
                    tok == Tok::Gt || tok == Tok::Comma))
                {
                    return false;
                }
            }

            return star;
        }

        bool IsNullConstant(const TokenView &view, std::size_t position)
        {
            return view.IsZeroLiteral(position) || (view.IsWord(position) && view.Text(position) == "NULL");
        }

        // Tokens after which `(T*)0` cannot be a cast: a call or a keyword
        // construct that owns the parentheses.
        bool OwnsParentheses(const TokenView &view, std::size_t before)
        {
            if (view.IsWord(before))
            {
                return true;
            }

            switch (view.At(before))
            {
            case Tok::RParen:
            case Tok::RBracket:
            case Tok::Gt:
            case Tok::KwSizeof:
            case Tok::KwAlignof:
            case Tok::KwDecltype:
            case Tok::KwTypeid:
            case Tok::KwNoexcept:
            case Tok::KwOperator:
            case Tok::KwNew:
            case Tok::KwDelete:
            case Tok::KwIf:
            case Tok::KwWhile:
            case Tok::KwFor:
            case Tok::KwSwitch:
            case Tok::KwCatch:
            case Tok::KwRequires:
            case Tok::KwThrow:
            case Tok::KwStaticCast:
            case Tok::KwDynamicCast:
            case Tok::KwConstCast:
            case Tok::KwReinterpretCast:
            case Tok::KwAlignas:
                return true;
            default:
                return false;
            }
        }

        // The cast result is used as an object (`((T*)0)->member`, the classic
        // offsetof idiom): `nullptr` would not compile there.
        bool UsedAsObject(const TokenView &view, std::size_t after)
        {
            while (view.At(after) == Tok::RParen)
            {
                ++after;
            }

            const Tok next = view.At(after);
            return next == Tok::Arrow || next == Tok::Dot || next == Tok::LBracket;
        }

    } // namespace

    std::vector<Diagnostic> SemanticRules::AnalyzeNullptr(const SemanticModel &model)
    {
        Reporter reporter(model);
        const TokenView view(model);
        const auto report = [&](std::size_t first, std::size_t last)
        {
            const auto offset = view.Offset(first);
            reporter.Report(RuleId::ModernizeNullptr, "cpp/modernize-nullptr",
                "use nullptr instead of casting a null constant to a pointer type", offset, view.End(last) - offset,
                offset, view.End(last) - offset, "nullptr", false, "Replace the cast with nullptr");
        };

        for (std::size_t i = 0; i < view.Size(); ++i)
        {
            const Tok tok = view.At(i);
            if (tok == Tok::LParen)
            {
                // (T*)0
                if (i > 0 && OwnsParentheses(view, i - 1))
                {
                    continue;
                }

                std::size_t close = i + 1;
                while (close < view.Size() && close < i + 24 && view.At(close) != Tok::RParen &&
                    view.At(close) != Tok::LParen)
                {
                    ++close;
                }

                if (close >= view.Size() || view.At(close) != Tok::RParen || !IsPointerType(view, i + 1, close) ||
                    !IsNullConstant(view, close + 1) || UsedAsObject(view, close + 2))
                {
                    continue;
                }

                report(i, close + 1);
                i = close + 1;
            }
            else if (tok == Tok::KwStaticCast || tok == Tok::KwReinterpretCast)
            {
                // static_cast<T*>(0)
                if (view.At(i + 1) != Tok::Lt)
                {
                    continue;
                }

                const auto angle = view.MatchAngle(i + 1, view.Size());
                if (angle >= view.Size() || !IsPointerType(view, i + 2, angle) || view.At(angle + 1) != Tok::LParen ||
                    !IsNullConstant(view, angle + 2) || view.At(angle + 3) != Tok::RParen ||
                    UsedAsObject(view, angle + 4))
                {
                    continue;
                }

                report(i, angle + 3);
                i = angle + 3;
            }
        }

        return reporter.Take();
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeZeroAsNull(const SemanticModel &model)
    {
        Reporter reporter(model);
        const TokenView view(model);
        const auto &symbols = model.Symbols();
        const auto &nodes = model.Tree().Nodes();
        const auto report = [&](std::size_t position, std::string_view context)
        {
            const auto offset = view.Offset(position);
            const auto length = view.End(position) - offset;
            reporter.Report(RuleId::NoZeroAsNull, "cpp/no-zero-as-null",
                std::string("use nullptr instead of ") + std::string(view.Text(position)) + " " +
                    std::string(context),
                offset, length, offset, length, "nullptr", true,
                "Replace " + std::string(view.Text(position)) + " with nullptr");
        };
        const auto is_pointer = [&](SymbolId symbol)
        {
            return symbol != kNone && (symbols.flags[symbol] & SymbolFlag::Pointer) != 0 &&
                (symbols.kind[symbol] == SymbolKind::Variable || symbols.kind[symbol] == SymbolKind::Parameter);
        };

        // `T* p = 0` and `void f(T* p = 0)`.
        std::vector<std::uint32_t> returning_pointer;
        for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
        {
            if (symbols.kind[symbol] == SymbolKind::Function &&
                (symbols.flags[symbol] & SymbolFlag::ReturnsPointer) != 0 &&
                (symbols.flags[symbol] & SymbolFlag::Definition) != 0)
            {
                returning_pointer.push_back(symbols.decl_node[symbol]);
            }

            if (!is_pointer(symbol))
            {
                continue;
            }

            const auto name = view.PositionOf(symbols.decl_token[symbol]);
            if (view.At(name + 1) != Tok::Eq || !view.IsZeroLiteral(name + 2))
            {
                continue;
            }

            const Tok after = view.At(name + 3);
            if (after == Tok::Semi || after == Tok::Comma ||
                (symbols.kind[symbol] == SymbolKind::Parameter && after == Tok::RParen))
            {
                report(name + 2, "to initialize a pointer");
            }
        }

        std::sort(returning_pointer.begin(), returning_pointer.end());
        for (std::uint32_t node = 1; node < nodes.size(); ++node)
        {
            if (nodes[node].kind == GrammarKind::BinaryExpression)
            {
                // `p = 0`, `p == 0`, `p != 0`, `0 == p`
                const auto [begin, end] = view.Range(node);
                if (end != begin + 3)
                {
                    continue;
                }

                const Tok op = view.At(begin + 1);
                const bool assignment = op == Tok::Eq;
                const bool comparison = op == Tok::EqEq || op == Tok::BangEq;
                if (view.IsWord(begin) && view.IsZeroLiteral(begin + 2) && (assignment || comparison) &&
                    is_pointer(model.ResolveToken(view.TokenAt(begin))))
                {
                    report(begin + 2, assignment ? "assigned to a pointer" : "compared with a pointer");
                }
                else if (view.IsZeroLiteral(begin) && comparison && view.IsWord(begin + 2) &&
                    is_pointer(model.ResolveToken(view.TokenAt(begin + 2))))
                {
                    report(begin, "compared with a pointer");
                }
            }
            else if (nodes[node].kind == GrammarKind::ReturnStatement)
            {
                // `return 0;` in a function that returns a pointer
                const auto [begin, end] = view.Range(node);
                if (view.At(begin) != Tok::KwReturn || !view.IsZeroLiteral(begin + 1) ||
                    !(end == begin + 2 || (end == begin + 3 && view.At(begin + 2) == Tok::Semi)))
                {
                    continue;
                }

                std::uint32_t owner = nodes[node].parent;
                for (std::size_t steps = 0; steps < nodes.size() && owner < nodes.size() && owner != 0; ++steps)
                {
                    const auto kind = nodes[owner].kind;
                    if (kind == GrammarKind::LambdaExpression)
                    {
                        owner = kNone;
                        break;
                    }

                    if (kind == GrammarKind::FunctionDefinition)
                    {
                        break;
                    }

                    owner = nodes[owner].parent;
                }

                if (owner < nodes.size() && owner != 0 &&
                    std::binary_search(returning_pointer.begin(), returning_pointer.end(), owner))
                {
                    report(begin + 1, "returned from a function that returns a pointer");
                }
            }
        }

        return reporter.Take();
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeAuto(const SemanticModel &model)
    {
        Reporter reporter(model);
        const TokenView view(model);
        const auto &nodes = model.Tree().Nodes();
        const auto &scopes = model.Scopes();

        for (std::uint32_t node = 1; node < nodes.size(); ++node)
        {
            if (nodes[node].kind != GrammarKind::Declaration && nodes[node].kind != GrammarKind::DeclarationStatement)
            {
                continue;
            }

            // Members cannot be declared `auto` (non-static) or defined in class (static).
            if (scopes.kind[model.ScopeOfNode(node)] == ScopeKind::Class)
            {
                continue;
            }

            // One declarator: an InitDeclarator holding a Declarator and an initializer.
            std::uint32_t declarator = kNone;
            std::size_t declarators = 0;
            for (const auto child: model.ChildrenOf(node))
            {
                if (nodes[child].kind == GrammarKind::InitDeclarator || nodes[child].kind == GrammarKind::Declarator)
                {
                    ++declarators;
                    for (const auto inner: model.ChildrenOf(child))
                    {
                        if (nodes[inner].kind == GrammarKind::Declarator)
                        {
                            declarator = inner;
                        }
                    }
                }
            }

            if (declarators != 1 || declarator == kNone)
            {
                continue;
            }

            std::uint32_t name_token = kNone;
            for (const auto inner: model.ChildrenOf(declarator))
            {
                if (nodes[inner].kind == GrammarKind::DeclaredName)
                {
                    name_token = nodes[inner].first_token;
                }
                else if (nodes[inner].kind == GrammarKind::FunctionSuffix ||
                    nodes[inner].kind == GrammarKind::ArraySuffix)
                {
                    name_token = kNone;
                    break;
                }
            }

            if (name_token == kNone)
            {
                continue;
            }

            auto [begin, end] = view.Range(node);
            if (end > begin && view.At(end - 1) == Tok::Semi)
            {
                --end;
            }

            const auto name = view.PositionOf(name_token);
            if (name >= end || view.TokenAt(name) != name_token || view.At(name + 1) != Tok::Eq)
            {
                continue;
            }

            // Leading storage specifiers stay; anything else before the type rules the
            // declaration out.
            std::size_t type_first = begin;
            bool eligible = true;
            for (; type_first < name; ++type_first)
            {
                const Tok tok = view.At(type_first);
                if (tok == Tok::KwStatic || tok == Tok::KwConstexpr || tok == Tok::KwInline || tok == Tok::KwThreadLocal ||
                    tok == Tok::KwRegister)
                {
                    continue;
                }

                if (tok == Tok::KwExtern || tok == Tok::KwTypedef || tok == Tok::KwFriend || tok == Tok::KwVirtual ||
                    tok == Tok::KwExplicit || tok == Tok::KwUsing || tok == Tok::KwMutable)
                {
                    eligible = false;
                }

                break;
            }

            if (!eligible || type_first >= name)
            {
                continue;
            }

            std::size_t trailing_stars = 0;
            for (std::size_t i = type_first; i < name && eligible; ++i)
            {
                switch (view.At(i))
                {
                case Tok::KwConst:
                case Tok::KwVolatile:
                case Tok::Amp:
                case Tok::AmpAmp:
                case Tok::LBracket:
                case Tok::LParen:
                case Tok::KwAuto:
                case Tok::KwDecltype:
                case Tok::Ellipsis:
                    eligible = false;
                    break;
                default:
                    break;
                }
            }

            if (!eligible)
            {
                continue;
            }

            for (std::size_t i = name; i > type_first && view.At(i - 1) == Tok::Star; --i)
            {
                ++trailing_stars;
            }

            const std::string declared = view.Spell(type_first, name);
            const std::size_t init = name + 2;
            bool matches = false;
            if (init >= end)
            {
                continue;
            }

            if (view.At(init) == Tok::KwNew)
            {
                // Foo* p = new Foo(...)  /  new Foo{...}  /  new Foo[n]  /  new Foo
                if (trailing_stars == 0 || view.At(init + 1) == Tok::LParen)
                {
                    continue;
                }

                std::size_t type_end = init + 1;
                while (type_end < end && view.At(type_end) != Tok::LParen && view.At(type_end) != Tok::LBrace &&
                    view.At(type_end) != Tok::LBracket)
                {
                    ++type_end;
                }

                if (type_end > init + 1 && (type_end == end || view.Match(type_end, end) == end - 1))
                {
                    matches = view.Spell(init + 1, type_end) + "* " == declared;
                }
            }
            else if (view.At(init) == Tok::KwStaticCast || view.At(init) == Tok::KwDynamicCast ||
                view.At(init) == Tok::KwReinterpretCast || view.At(init) == Tok::KwConstCast)
            {
                // T x = static_cast<T>(...)
                if (view.At(init + 1) == Tok::Lt)
                {
                    const auto angle = view.MatchAngle(init + 1, end);
                    // Casting a null constant is cpp/modernize-nullptr's: after that fix there is
                    // nothing left for `auto` to deduce from.
                    const bool null_operand = IsNullConstant(view, angle + 2) && view.At(angle + 3) == Tok::RParen;
                    if (angle < end && view.At(angle + 1) == Tok::LParen && view.Match(angle + 1, end) == end - 1 &&
                        !null_operand)
                    {
                        matches = view.Spell(init + 2, angle) == declared;
                    }
                }
            }
            else if (view.IsWord(init) && view.Text(init) == "std" && view.At(init + 1) == Tok::ColonColon &&
                (view.Text(init + 2) == "make_unique" || view.Text(init + 2) == "make_shared"))
            {
                // std::unique_ptr<T> p = std::make_unique<T>(...)
                std::size_t paren = init + 3;
                while (paren < end && view.At(paren) != Tok::LParen)
                {
                    ++paren;
                }

                if (paren < end && view.Match(paren, end) == end - 1)
                {
                    const bool unique = view.Text(init + 2) == "make_unique";
                    std::string head = view.Spell(init, init + 2) +
                        (unique ? "unique_ptr " : "shared_ptr ") + view.Spell(init + 3, paren);
                    matches = head == declared;
                    trailing_stars = 0;
                }
            }

            if (!matches)
            {
                continue;
            }

            const auto offset = view.Offset(type_first);
            const auto length = view.End(name - 1) - offset;
            const std::string replacement = "auto" + std::string(trailing_stars, '*');
            reporter.Report(RuleId::ModernizeAuto, "cpp/modernize-auto",
                "use auto: the type is already spelled in the initializer", offset, length, offset, length,
                replacement, true, "Use " + replacement);
        }

        return reporter.Take();
    }

    std::vector<Diagnostic> SemanticRules::Analyze(const SemanticModel &model)
    {
        auto all = AnalyzeOverride(model);
        for (auto &&part: {AnalyzeNullptr(model), AnalyzeZeroAsNull(model), AnalyzeAuto(model)})
        {
            all.insert(all.end(), part.begin(), part.end());
        }

        std::stable_sort(all.begin(), all.end(),
            [](const Diagnostic &a, const Diagnostic &b)
            {
                return a.offset < b.offset;
            });
        return all;
    }

} // namespace heimdall
