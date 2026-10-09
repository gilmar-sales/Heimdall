#include <Heimdall/LineTable.hpp>
#include <Heimdall/SemanticRules.hpp>

#include "detail/TokenView.hpp"

#include <algorithm>
#include <cctype>
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

        bool IsTrivia(const Token& token)
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
        SymbolId FindOverridden(
            const SemanticModel& model,
            Visited& visited,
            SymbolId derived,
            NameId name,
            std::uint64_t signature)
        {
            ++visited.epoch;
            const auto& symbols = model.Symbols();
            const auto& bases = model.Bases();
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
        bool OverrideInsertionOffset(const ParseTree& tree, std::uint32_t name_token, std::size_t& offset)
        {
            const auto& tokens = tree.Tokens();
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

    std::vector<Diagnostic> SemanticRules::AnalyzeOverride(const SemanticModel& model)
    {
        std::vector<Diagnostic> diagnostics;
        const auto& symbols = model.Symbols();
        const auto& scopes = model.Scopes();
        const auto& tree = model.Tree();
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

            const auto& name_token = tree.Tokens()[token];
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
            [](const Diagnostic& a, const Diagnostic& b)
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
            explicit Reporter(const SemanticModel& model) : m_model(model) {}

            void Report(
                RuleId rule,
                std::string_view code,
                std::string message,
                std::size_t offset,
                std::size_t length,
                std::size_t fix_offset,
                std::size_t fix_length,
                std::string replacement,
                bool safe,
                std::string title)
            {
                if (!m_lines_built)
                {
                    m_lines.Build(m_model.Tree().Source());
                    m_lines_built = true;
                }

                const auto position = m_lines.Lookup(offset);
                Diagnostic diagnostic{rule, Severity::Warning, std::string(code), std::move(message), offset,
                    length, position.line, position.column, true,
                    TextEdit{fix_offset, fix_length, std::move(replacement)}};
                diagnostic.fix_is_safe = safe;
                diagnostic.fix_title = std::move(title);
                m_diagnostics.push_back(std::move(diagnostic));
            }

            void ReportNoFix(
                RuleId rule,
                std::string_view code,
                std::string message,
                std::size_t offset,
                std::size_t length)
            {
                if (!m_lines_built)
                {
                    m_lines.Build(m_model.Tree().Source());
                    m_lines_built = true;
                }

                const auto position = m_lines.Lookup(offset);
                m_diagnostics.push_back(Diagnostic{rule, Severity::Warning, std::string(code), std::move(message),
                        offset, length, position.line, position.column, false, TextEdit{0, 0, std::string()}});
            }

            std::vector<Diagnostic> Take()
            {
                std::sort(m_diagnostics.begin(), m_diagnostics.end(),
                    [](const Diagnostic& a, const Diagnostic& b)
                    {
                        return a.offset < b.offset;
                });
                return std::move(m_diagnostics);
            }

        private:
            const SemanticModel& m_model;
            LineTable m_lines;
            bool m_lines_built = false;
            std::vector<Diagnostic> m_diagnostics;
        };

        using detail::TokenView;

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
        bool IsPointerType(const TokenView& view, std::size_t first, std::size_t last)
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

        bool IsNullConstant(const TokenView& view, std::size_t position)
        {
            return view.IsZeroLiteral(position) ||(view.IsWord(position) && view.Text(position) == "NULL");
        }

        // Tokens after which `(T*)0` cannot be a cast: a call or a keyword
        // construct that owns the parentheses.
        bool OwnsParentheses(const TokenView& view, std::size_t before)
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
        bool UsedAsObject(const TokenView& view, std::size_t after)
        {
            while (view.At(after) == Tok::RParen)
            {
                ++after;
            }

            const Tok next = view.At(after);
            return next == Tok::Arrow || next == Tok::Dot || next == Tok::LBracket;
        }

    } // namespace

    std::vector<Diagnostic> SemanticRules::AnalyzeNullptr(const SemanticModel& model)
    {
        Reporter reporter(model);
        const TokenView view(model);
        const auto report =[&](std::size_t first, std::size_t last)
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

                if (close >= view.Size() || view.At(close) != Tok::RParen ||!IsPointerType(view, i + 1, close) ||
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
                if (angle >= view.Size() ||!IsPointerType(view, i + 2,
                    angle) || view.At(angle + 1) != Tok::LParen ||
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

    std::vector<Diagnostic> SemanticRules::AnalyzeZeroAsNull(const SemanticModel& model)
    {
        Reporter reporter(model);
        const TokenView view(model);
        const auto& symbols = model.Symbols();
        const auto& nodes = model.Tree().NodesSoA();
        const auto report =[&](std::size_t position, std::string_view context)
        {
            const auto offset = view.Offset(position);
            const auto length = view.End(position) - offset;
            reporter.Report(RuleId::NoZeroAsNull, "cpp/no-zero-as-null",
                std::string("use nullptr instead of ") + std::string(view.Text(position)) + " " +
                std::string(context),
                offset, length, offset, length, "nullptr", true,
                "Replace " + std::string(view.Text(position)) + " with nullptr");
        };
        const auto is_pointer =[&](SymbolId symbol)
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
            if (view.At(name + 1) != Tok::Eq ||!view.IsZeroLiteral(name + 2))
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
            if (nodes.Kind(node) == GrammarKind::BinaryExpression)
            {
                // `p = 0`, `p == 0`, `p != 0`, `0 == p`
                const auto[begin, end] = view.Range(node);
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
            else if (nodes.Kind(node) == GrammarKind::ReturnStatement)
            {
                // `return 0;` in a function that returns a pointer
                const auto[begin, end] = view.Range(node);
                if (view.At(begin) != Tok::KwReturn ||!view.IsZeroLiteral(begin + 1) ||
                    !(end == begin + 2 ||(end == begin + 3 && view.At(begin + 2) == Tok::Semi)))
                {
                    continue;
                }

                std::uint32_t owner = nodes.Parent(node);
                for (std::size_t steps = 0; steps < nodes.size() && owner < nodes.size() && owner != 0; ++steps)
                {
                    const auto kind = nodes.Kind(owner);
                    if (kind == GrammarKind::LambdaExpression)
                    {
                        owner = kNone;
                        break;
                    }

                    if (kind == GrammarKind::FunctionDefinition)
                    {
                        break;
                    }

                    owner = nodes.Parent(owner);
                }

                if (owner < nodes.size() && owner != 0 &&
                    std::binary_search(returning_pointer.begin(), returning_pointer.end(), owner))
                {
                    report(begin + 1, "returned from a function that returns a pointer");
                }
            }
        }

        auto diagnostics = reporter.Take();
        auto designated = AnalyzeDesignatedZeroAsNull(model);
        diagnostics.insert(diagnostics.end(), std::make_move_iterator(designated.begin()),
            std::make_move_iterator(designated.end()));
        std::stable_sort(diagnostics.begin(), diagnostics.end(),
            [](const Diagnostic& a, const Diagnostic& b)
            {
                return a.offset < b.offset;
        });
        return diagnostics;
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeIntegerToPointer(const SemanticModel& model)
    {
        Reporter reporter(model);
        const TokenView view(model);
        const auto& symbols = model.Symbols();
        const auto& nodes = model.Tree().NodesSoA();
        const auto is_integer =[&](std::size_t position)
        {
            if (view.KindAt(position) != TokenKind::Number || view.IsZeroLiteral(position))
            {
                return false;
            }

            const auto text = view.Text(position);
            if (text.size() > 1 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
            {
                return text.find_first_of(".pP") == std::string_view::npos;
            }

            return text.find_first_not_of("0123456789'uUlLzZ") == std::string_view::npos;
        };
        const auto report =[&](std::size_t position)
        {
            const auto offset = view.Offset(position);
            const auto length = view.End(position) - offset;
            reporter.ReportNoFix(RuleId::NoIntegerToPointer, "cpp/no-integer-to-pointer",
                "integer constant " + std::string(view.Text(position)) + " cannot be converted to a pointer",
                offset,
                length);
        };
        const auto is_pointer =[&](SymbolId symbol)
        {
            return symbol != kNone && (symbols.flags[symbol] & SymbolFlag::Pointer) != 0 &&
                (symbols.kind[symbol] == SymbolKind::Variable || symbols.kind[symbol] == SymbolKind::Parameter);
        };

        // `T* p = 20;` and `void f(T* p = 20)`.
        for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
        {
            if (!is_pointer(symbol))
            {
                continue;
            }

            const auto name = view.PositionOf(symbols.decl_token[symbol]);
            const Tok after = view.At(name + 3);
            if (view.At(name + 1) == Tok::Eq && is_integer(name + 2) &&
                (after == Tok::Semi || after == Tok::Comma ||
                (symbols.kind[symbol] == SymbolKind::Parameter && after == Tok::RParen)))
            {
                report(name + 2);
            }
        }

        // `p = 20`
        for (std::uint32_t node = 1; node < nodes.size(); ++node)
        {
            if (nodes.Kind(node) != GrammarKind::BinaryExpression)
            {
                continue;
            }

            const auto[begin, end] = view.Range(node);
            if (end == begin + 3 && view.At(begin + 1) == Tok::Eq && view.IsWord(begin) && is_integer(begin + 2) &&
                is_pointer(model.ResolveToken(view.TokenAt(begin))))
            {
                report(begin + 2);
            }
        }

        auto diagnostics = reporter.Take();
        auto designated = AnalyzeDesignatedIntegerToPointer(model);
        diagnostics.insert(diagnostics.end(), std::make_move_iterator(designated.begin()),
            std::make_move_iterator(designated.end()));
        for (auto& diagnostic : diagnostics)
        {
            diagnostic.severity = Severity::Error;
            diagnostic.has_fix = false;
        }

        std::stable_sort(diagnostics.begin(), diagnostics.end(),
            [](const Diagnostic& a, const Diagnostic& b)
            {
                return a.offset < b.offset;
        });
        return diagnostics;
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeAuto(const SemanticModel& model)
    {
        Reporter reporter(model);
        const TokenView view(model);
        const auto& nodes = model.Tree().NodesSoA();
        const auto& scopes = model.Scopes();

        for (std::uint32_t node = 1; node < nodes.size(); ++node)
        {
            if (nodes.Kind(node) != GrammarKind::Declaration && nodes.Kind(node) != GrammarKind::DeclarationStatement)
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
            for (const auto child : model.ChildrenOf(node))
            {
                if (nodes.Kind(child) == GrammarKind::InitDeclarator || nodes.Kind(child) == GrammarKind::Declarator)
                {
                    ++declarators;
                    for (const auto inner : model.ChildrenOf(child))
                    {
                        if (nodes.Kind(inner) == GrammarKind::Declarator)
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
            for (const auto inner : model.ChildrenOf(declarator))
            {
                if (nodes.Kind(inner) == GrammarKind::DeclaredName)
                {
                    name_token = nodes.FirstToken(inner);
                }
                else if (nodes.Kind(inner) == GrammarKind::FunctionSuffix ||
                    nodes.Kind(inner) == GrammarKind::ArraySuffix)
                {
                    name_token = kNone;
                    break;
                }
            }

            if (name_token == kNone)
            {
                continue;
            }

            auto[begin, end] = view.Range(node);
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

    namespace
    {

        // Child of `parent` whose significant tokens are exactly [begin, end).
        std::uint32_t ChildSpanning(
            const SemanticModel& model,
            const TokenView& view,
            std::uint32_t parent,
            std::size_t begin,
            std::size_t end)
        {
            for (const auto child : model.ChildrenOf(parent))
            {
                const auto[b, e] = view.Range(child);
                if (b == begin && e == end)
                {
                    return child;
                }
            }

            return kNone;
        }

        // The expression a control statement tests, or kNone when the statement is
        // not a plain `if`/`while`/classic `for` or the grammar did not give the
        // condition a node of its own (declaration conditions, C-style casts).
        std::uint32_t ConditionOf(const SemanticModel& model, const TokenView& view, std::uint32_t stmt)
        {
            const auto[begin, end] = view.Range(stmt);
            const Tok keyword = view.At(begin);
            if (keyword != Tok::KwIf && keyword != Tok::KwWhile && keyword != Tok::KwFor)
            {
                return kNone;
            }

            std::size_t open = begin + 1;
            if (keyword == Tok::KwIf && (view.At(open) == Tok::KwConstexpr || view.At(open) == Tok::KwConsteval))
            {
                ++open;
            }

            if (view.At(open) != Tok::LParen)
            {
                return kNone;
            }

            const auto close = view.Match(open, end);
            if (close >= end)
            {
                return kNone;
            }

            std::vector<std::size_t> semis;
            for (std::size_t i = open + 1; i < close; ++i)
            {
                const Tok tok = view.At(i);
                if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace)
                {
                    const auto match = view.Match(i, close);
                    if (match >= close)
                    {
                        return kNone;
                    }

                    i = match;
                }
                else if (tok == Tok::Semi)
                {
                    semis.push_back(i);
                }
            }

            std::size_t first = open + 1;
            std::size_t last = close;
            if (keyword == Tok::KwWhile && semis.empty())
            {
                // the whole parenthesized expression
            }
            else if (keyword == Tok::KwIf && semis.size() <= 1)
            {
                first = semis.empty() ? open + 1 : semis[0] + 1;
            }
            else if (keyword == Tok::KwFor && semis.size() == 2)
            {
                first = semis[0] + 1;
                last = semis[1];
            }
            else
            {
                return kNone;
            }

            return first < last ? ChildSpanning(model, view, stmt, first, last) : kNone;
        }

        bool IsVectorOfBool(const TypeModel& types, TypeId type)
        {
            const auto& table = types.Types();
            return table.Kind(type) == TypeKind::External &&
                types.ExternalNames().Text(table.Arg(type)).starts_with("std::vector<bool");
        }

        // The classic `for (init; cond; inc) body`, as positions in the significant
        // tokens: `kw` is `for`, [open, close] its parentheses, `end` just past the body.
        struct ForLoop
        {
            std::uint32_t node = kNone;
            std::size_t kw = 0;
            std::size_t open = 0;
            std::size_t semi1 = 0;
            std::size_t semi2 = 0;
            std::size_t close = 0;
            std::size_t end = 0;
        };

        bool SplitFor(const TokenView& view, std::uint32_t node, ForLoop& loop)
        {
            const auto[begin, end] = view.Range(node);
            if (view.At(begin) != Tok::KwFor || view.At(begin + 1) != Tok::LParen)
            {
                return false;
            }

            loop.node = node;
            loop.kw = begin;
            loop.open = begin + 1;
            loop.close = view.Match(loop.open, end);
            loop.end = end;
            if (loop.close >= end)
            {
                return false;
            }

            std::vector<std::size_t> semis;
            for (std::size_t i = loop.open + 1; i < loop.close; ++i)
            {
                const Tok tok = view.At(i);
                if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace)
                {
                    const auto match = view.Match(i, loop.close);
                    if (match >= loop.close)
                    {
                        return false;
                    }

                    i = match;
                }
                else if (tok == Tok::Semi)
                {
                    semis.push_back(i);
                }
            }

            if (semis.size() != 2)
            {
                return false;
            }

            loop.semi1 = semis[0];
            loop.semi2 = semis[1];
            return loop.close + 1 < loop.end;
        }

        // `++i`, `i++` or `i += 1` over [begin, end).
        bool IsIncrementOf(
            const SemanticModel& model,
            const TokenView& view,
            std::size_t begin,
            std::size_t end,
            SymbolId variable)
        {
            const auto names =[&](std::size_t position)
            {
                return view.IsWord(position) && model.ResolveToken(view.TokenAt(position)) == variable;
            };
            if (end == begin + 2)
            {
                return (view.At(begin) == Tok::PlusPlus&& names(begin + 1)) ||
                    (names(begin) && view.At(begin + 1) == Tok::PlusPlus);
            }

            return end == begin + 3 && names(begin) && view.At(begin + 1) == Tok::PlusEq && view.Text(begin + 2) == "1";
        }

        // Picks a name for the loop variable that nothing in the loop or the file
        // already uses.
        std::string FreshElementName(const SemanticModel& model, const TokenView& view, const ForLoop& loop)
        {
            for (const std::string_view candidate :
                {
                    "element", "item", "entry", "value"
            })
            {
                bool used = model.Names().Find(candidate) != kNone;
                for (std::size_t i = loop.kw; i < loop.end && !used; ++i)
                {
                    used = view.Text(i) == candidate;
                }

                if (!used)
                {
                    return std::string(candidate);
                }
            }

            return std::string();
        }

        // A `[` that does not subscript anything starts a lambda or an attribute:
        // captures would change meaning when the loop variable changes.
        bool StartsLambda(const TokenView& view, std::size_t position, std::size_t body_begin)
        {
            if (view.At(position) != Tok::LBracket)
            {
                return false;
            }

            if (position == body_begin)
            {
                return true;
            }

            return!(view.IsWord(position - 1) || view.At(position - 1) == Tok::RBracket ||
                view.At(position - 1) == Tok::RParen);
        }

        bool EndsOperand(const TokenView& view, std::size_t position)
        {
            if (view.IsWord(position) || view.IsLiteralToken(position))
            {
                return true;
            }

            const Tok tok = view.At(position);
            return tok == Tok::RParen || tok == Tok::RBracket || tok == Tok::KwThis || tok == Tok::KwTrue ||
                tok == Tok::KwFalse || tok == Tok::KwNullptr;
        }

        struct Edit
        {
            std::size_t first;
            std::size_t last; // positions, inclusive
            std::string text;
        };

        // The loop as a range-based for: `header` replaces everything up to the
        // closing parenthesis and `edits` (ascending, disjoint) rewrite the body.
        Diagnostic BuildLoopDiagnostic(
            const SemanticModel& model,
            const TokenView& view,
            const ForLoop& loop,
            RuleId rule,
            std::string_view code,
            std::string message,
            const std::string& header,
            const std::vector<Edit>& edits,
            LineTable& lines,
            bool& lines_built)
        {
            const auto source = model.Tree().Source();
            const auto body_offset = view.End(loop.close);
            const auto loop_end = view.End(loop.end - 1);
            std::string body(source.substr(body_offset, loop_end - body_offset));
            for (auto it = edits.rbegin(); it != edits.rend(); ++it)
            {
                const auto offset = view.Offset(it->first) - body_offset;
                body.replace(offset, view.End(it->last) - view.Offset(it->first), it->text);
            }

            if (!lines_built)
            {
                lines.Build(source);
                lines_built = true;
            }

            const auto offset = view.Offset(loop.kw);
            const auto position = lines.Lookup(offset);
            Diagnostic diagnostic{rule, Severity::Warning, std::string(code), std::move(message), offset,
                body_offset - offset, position.line, position.column, true,
                TextEdit{offset, loop_end - offset, header + body}};
            // The body is rewritten by pattern: offered as a quick fix, not in batch.
            diagnostic.fix_is_safe = false;
            diagnostic.fix_title = "Convert to a range-based for";
            return diagnostic;
        }

        void SortByOffset(std::vector<Diagnostic>& diagnostics)
        {
            std::sort(diagnostics.begin(), diagnostics.end(),
                [](const Diagnostic& a, const Diagnostic& b)
                {
                    return a.offset < b.offset;
            });
        }

    } // namespace

    std::vector<Diagnostic> SemanticRules::AnalyzeImplicitBool(const TypeModel& types)
    {
        const auto& model = types.Model();
        const auto& table = types.Types();
        const auto& nodes = model.Tree().NodesSoA();
        Reporter reporter(model);
        const TokenView view(model);
        const auto check =[&](std::uint32_t operand, std::string_view context)
        {
            if (operand >= nodes.size() || nodes.Kind(operand) == GrammarKind::LiteralExpression)
            {
                return;
            }

            const auto[begin, end] = view.Range(operand);
            if (begin >= end ||!model.IsCode(view.TokenAt(begin)))
            {
                return;
            }

            const auto type = table.Strip(types.NodeType(operand));
            const char* advice = nullptr;
            if (table.IsInteger(type))
            {
                advice = "compare with 0 explicitly";
            }
            else if (table.IsFloating(type))
            {
                advice = "compare with 0.0 explicitly";
            }
            else if (table.IsPointer(type) || table.IsArray(type))
            {
                advice = "compare with nullptr explicitly";
            }

            if (advice == nullptr)
            {
                return;
            }

            const auto offset = view.Offset(begin);
            reporter.ReportNoFix(RuleId::NoImplicitBoolConversion, "cpp/no-implicit-bool-conversion",
                "implicit conversion of '" + types.Spell(type) + "' to bool in " + std::string(context) + "; " + advice,
                offset, view.End(end - 1) - offset);
        };

        for (std::uint32_t node = 1; node < nodes.size(); ++node)
        {
            const auto kids = model.ChildrenOf(node);
            const auto[begin, end] = view.Range(node);
            if (begin >= end)
            {
                continue;
            }

            switch (nodes.Kind(node))
            {
            case GrammarKind::UnaryExpression:
            {
                if (view.At(begin) != Tok::Bang || kids.empty() || view.Range(kids[0]).first != begin + 1 ||
                    view.Range(kids[0]).second != end)
                {
                    break;
                }

                // `!!x` is the idiom for an explicit conversion.
                const auto parent = nodes.Parent(node);
                if (parent < nodes.size() && nodes.Kind(parent) == GrammarKind::UnaryExpression &&
                    view.Range(parent).first + 1 == begin && view.At(view.Range(parent).first) == Tok::Bang)
                {
                    break;
                }

                check(kids[0], "the operand of '!'");
                break;
            }
            case GrammarKind::BinaryExpression:
            {
                if (kids.size() != 2 || view.Range(kids[0]).first != begin)
                {
                    break;
                }

                const auto op = view.Range(kids[0]).second;
                if (view.At(op) != Tok::AmpAmp && view.At(op) != Tok::PipePipe)
                {
                    break;
                }

                const std::string context = "an operand of '" + std::string(view.Text(op)) + "'";
                check(kids[0], context);
                if (view.Range(kids[1]).first == op + 1 && view.Range(kids[1]).second == end)
                {
                    check(kids[1], context);
                }

                break;
            }
            case GrammarKind::ConditionalExpression:
                if (kids.size() == 3 && view.Range(kids[0]).first == begin &&
                    view.At(view.Range(kids[0]).second) == Tok::Question)
                {
                    check(kids[0], "the condition of '?:'");
                }

                break;
            case GrammarKind::IfStatement:
                if (const auto condition = ConditionOf(model, view, node); condition != kNone)
                {
                    check(condition, "an 'if' condition");
                }

                break;
            case GrammarKind::LoopStatement:
                if (const auto condition = ConditionOf(model, view, node); condition != kNone)
                {
                    check(condition, view.At(begin) == Tok::KwFor ? "a 'for' condition" : "a 'while' condition");
                }

                break;
            default:
                break;
            }
        }

        return reporter.Take();
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeRangeLoop(const TypeModel& types)
    {
        const auto& model = types.Model();
        const auto& table = types.Types();
        const auto& nodes = model.Tree().NodesSoA();
        const auto& symbols = model.Symbols();
        const TokenView view(model);
        std::vector<Diagnostic> diagnostics;
        LineTable lines;
        bool lines_built = false;

        const auto indexable =[&](SymbolId symbol, bool allow_array)
        {
            if (symbol == kNone ||
                (symbols.kind[symbol] != SymbolKind::Variable && symbols.kind[symbol] != SymbolKind::Parameter))
            {
                return false;
            }

            // A member may be changed by any call in the body.
            if (model.Scopes().kind[symbols.scope[symbol]] == ScopeKind::Class)
            {
                return false;
            }

            const auto type = table.Strip(types.SymbolType(symbol));
            if (table.IsArray(type))
            {
                return allow_array;
            }

            return table.Kind(type) == TypeKind::External && !IsVectorOfBool(types, type) &&
                IsStdIndexableHead(types.ExternalHead(type));
        };

        for (std::uint32_t node = 1; node < nodes.size(); ++node)
        {
            ForLoop loop;
            if (nodes.Kind(node) != GrammarKind::LoopStatement ||!SplitFor(view, node, loop) ||
                !model.IsCode(view.TokenAt(loop.kw)))
            {
                continue;
            }

            // init: `T i = 0`
            if (loop.semi1 < loop.open + 5)
            {
                continue;
            }

            const auto name_pos = loop.semi1 - 3;
            if (!view.IsWord(name_pos) || view.At(name_pos + 1) != Tok::Eq ||!view.IsZeroLiteral(name_pos + 2))
            {
                continue;
            }

            bool simple = true;
            for (std::size_t i = loop.open + 1; i < name_pos && simple; ++i)
            {
                simple = view.At(i) != Tok::Comma;
            }

            const auto name_id = model.Names().Find(view.Text(name_pos));
            const auto variable = name_id == kNone ? kNone : model.LookupLocal(model.ScopeOfNode(node),
                name_id);
            if (!simple || variable == kNone || symbols.decl_token[variable] != view.TokenAt(name_pos) ||
                symbols.kind[variable] != SymbolKind::Variable ||
                !table.IsInteger(table.Strip(types.SymbolType(variable))))
            {
                continue;
            }

            // cond: `i < bound`, inc: `++i`
            if (loop.semi2 < loop.semi1 + 4 ||!view.IsWord(loop.semi1 + 1) ||
                model.ResolveToken(view.TokenAt(loop.semi1 + 1)) != variable || view.At(loop.semi1 + 2) != Tok::Lt ||
                !IsIncrementOf(model, view, loop.semi2 + 1, loop.close, variable))
            {
                continue;
            }

            const auto bound = loop.semi1 + 3;
            const auto bound_end = loop.semi2;
            SymbolId container = kNone;
            if (bound_end == bound + 5 && view.IsWord(bound) && view.At(bound + 1) == Tok::Dot &&
                view.Text(bound + 2) == "size" && view.At(bound + 3) == Tok::LParen && view.At(bound + 4) == Tok::RParen)
            {
                container = model.ResolveToken(view.TokenAt(bound));
                if (!indexable(container, false))
                {
                    continue;
                }
            }
            else if (bound_end == bound + 6 && view.Text(bound) == "std" && view.At(bound + 1) == Tok::ColonColon &&
                view.Text(bound + 2) == "size" && view.At(bound + 3) == Tok::LParen && view.IsWord(bound + 4) &&
                view.At(bound + 5) == Tok::RParen)
            {
                container = model.ResolveToken(view.TokenAt(bound + 4));
                if (!indexable(container, true))
                {
                    continue;
                }
            }
            else if (bound_end == bound + 1 && view.IsLiteralToken(bound))
            {
                const auto text = view.Text(bound);
                if (text.empty() || text.size() >= 10 || text.find_first_not_of("0123456789") != std::string_view::npos)
                {
                    continue;
                }

                std::uint64_t literal = 0;
                for (const char c : text)
                {
                    literal = literal * 10 + static_cast<std::uint64_t>(c - '0');
                }

                // The array is the first `x[i]` of the body.
                for (std::size_t p = loop.close + 1; p + 3 < loop.end && container == kNone; ++p)
                {
                    if (view.IsWord(p) && view.At(p + 1) == Tok::LBracket && view.IsWord(p + 2) &&
                        model.ResolveToken(view.TokenAt(p + 2)) == variable && view.At(p + 3) == Tok::RBracket)
                    {
                        container = model.ResolveToken(view.TokenAt(p));
                    }
                }

                const auto array = container == kNone ? TypeTable::Unknown : table.Strip(types.SymbolType(container));
                if (!indexable(container, true) ||!table.IsArray(array) || table.Extent(array) != literal)
                {
                    continue;
                }
            }
            else
            {
                continue;
            }

            // body: every use of `i` and of the container is `c[i]`.
            const auto variable_name = view.Text(name_pos);
            const auto container_name = model.Names().Text(symbols.name[container]);
            std::vector<Edit> edits;
            bool ok = true;
            const auto body_begin = loop.close + 1;
            for (std::size_t p = body_begin; p < loop.end && ok; ++p)
            {
                if (StartsLambda(view, p, body_begin))
                {
                    ok = false;
                }
                else if (!view.IsWord(p))
                {
                    continue;
                }
                else if (view.Text(p) == container_name)
                {
                    const bool pattern = p + 3 < loop.end && view.At(p + 1) == Tok::LBracket && view.IsWord(p + 2) &&
                        model.ResolveToken(view.TokenAt(p + 2)) == variable && view.At(p + 3) == Tok::RBracket &&
                        model.ResolveToken(view.TokenAt(p)) == container;
                    if (!pattern)
                    {
                        ok = false;
                        break;
                    }

                    edits.push_back({p, p + 3, std::string()});
                    p += 3;
                }
                else if (view.Text(p) == variable_name)
                {
                    ok = false; // the use is not inside `c[i]`
                }
            }

            const auto element = FreshElementName(model, view, loop);
            if (!ok || edits.empty() || element.empty())
            {
                continue;
            }

            for (auto& edit : edits)
            {
                edit.text = element;
            }

            const std::string header = std::string("for (") +
                (table.IsConstQualified(types.SymbolType(container)) ? "const auto& " : "auto& ") + element + " : " +
                std::string(container_name) + ")";
            diagnostics.push_back(BuildLoopDiagnostic(model, view, loop, RuleId::ModernizeRangeLoop,
                "cpp/modernize-range-loop",
                "loop over '" + std::string(container_name) + "' by index can be a range-based for", header, edits,
                lines, lines_built));
        }

        SortByOffset(diagnostics);
        return diagnostics;
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeLoopConvert(const TypeModel& types)
    {
        const auto& model = types.Model();
        const auto& table = types.Types();
        const auto& nodes = model.Tree().NodesSoA();
        const auto& symbols = model.Symbols();
        const TokenView view(model);
        std::vector<Diagnostic> diagnostics;
        LineTable lines;
        bool lines_built = false;

        for (std::uint32_t node = 1; node < nodes.size(); ++node)
        {
            ForLoop loop;
            if (nodes.Kind(node) != GrammarKind::LoopStatement ||!SplitFor(view, node, loop) ||
                !model.IsCode(view.TokenAt(loop.kw)))
            {
                continue;
            }

            // init: `auto it = c.begin()` or `auto it = std::begin(c)`
            const auto first = loop.open + 1;
            if (view.At(first) != Tok::KwAuto ||!view.IsWord(first + 1) || view.At(first + 2) != Tok::Eq)
            {
                continue;
            }

            std::size_t container_pos = 0;
            std::string_view begin_name;
            if (loop.semi1 == first + 8 && view.IsWord(first + 3) && view.At(first + 4) == Tok::Dot &&
                view.IsWord(first + 5) && view.At(first + 6) == Tok::LParen && view.At(first + 7) == Tok::RParen)
            {
                container_pos = first + 3;
                begin_name = view.Text(first + 5);
            }
            else if (loop.semi1 == first + 9 && view.Text(first + 3) == "std" && view.At(first + 4) == Tok::ColonColon &&
                view.IsWord(first + 5) && view.At(first + 6) == Tok::LParen && view.IsWord(first + 7) &&
                view.At(first + 8) == Tok::RParen)
            {
                container_pos = first + 7;
                begin_name = view.Text(first + 5);
            }
            else
            {
                continue;
            }

            const bool free_form = container_pos == first + 7;
            if (begin_name != "begin" && begin_name != "cbegin")
            {
                continue;
            }

            const auto name_pos = first + 1;
            const auto name_id = model.Names().Find(view.Text(name_pos));
            const auto variable = name_id == kNone ? kNone : model.LookupLocal(model.ScopeOfNode(node),
                name_id);
            const auto container = model.ResolveToken(view.TokenAt(container_pos));
            if (variable == kNone || container == kNone || symbols.decl_token[variable] != view.TokenAt(name_pos) ||
                symbols.kind[variable] != SymbolKind::Variable ||
                (symbols.kind[container] != SymbolKind::Variable && symbols.kind[container] != SymbolKind::Parameter) ||
                model.Scopes().kind[symbols.scope[container]] == ScopeKind::Class)
            {
                continue;
            }

            const auto container_type = table.Strip(types.SymbolType(container));
            const bool known_container = (table.Kind(container_type) == TypeKind::External &&
                !IsVectorOfBool(types, container_type) &&
                IsStdContainerHead(types.ExternalHead(container_type))) ||
                (free_form && table.IsArray(container_type));
            if (!known_container)
            {
                continue;
            }

            // cond: `it != c.end()` / `it != std::end(c)`, inc: `++it`
            const auto condition = loop.semi1 + 1;
            const std::string_view end_name = begin_name == "begin" ? "end" : "cend";
            const bool cond_shape = free_form
            ? loop.semi2 == condition + 8 && view.Text(condition + 2) == "std" &&
                view.At(condition + 3) == Tok::ColonColon && view.Text(condition + 4) == end_name &&
                view.At(condition + 5) == Tok::LParen && view.IsWord(condition + 6) &&
                view.At(condition + 7) == Tok::RParen && model.ResolveToken(view.TokenAt(condition + 6)) == container
            : loop.semi2 == condition + 7 && view.IsWord(condition + 2) &&
                model.ResolveToken(view.TokenAt(condition + 2)) == container &&
                view.At(condition + 3) == Tok::Dot && view.Text(condition + 4) == end_name &&
                view.At(condition + 5) == Tok::LParen && view.At(condition + 6) == Tok::RParen;
            if (!cond_shape ||!view.IsWord(condition) || model.ResolveToken(view.TokenAt(condition)) != variable ||
                view.At(condition + 1) != Tok::BangEq)
            {
                continue;
            }

            const auto inc_begin = loop.semi2 + 1;
            if (loop.close != inc_begin + 2 ||
                !((view.At(inc_begin) == Tok::PlusPlus && view.IsWord(inc_begin + 1) &&
                model.ResolveToken(view.TokenAt(inc_begin + 1)) == variable) ||
                (view.IsWord(inc_begin) && model.ResolveToken(view.TokenAt(inc_begin)) == variable &&
                view.At(inc_begin + 1) == Tok::PlusPlus)))
            {
                continue;
            }

            // body: the iterator is only dereferenced, the container is not touched.
            const auto iterator_name = view.Text(name_pos);
            const auto container_name = model.Names().Text(symbols.name[container]);
            const auto element = FreshElementName(model, view, loop);
            std::vector<Edit> edits;
            bool ok = true;
            const auto body_begin = loop.close + 1;
            for (std::size_t p = body_begin; p < loop.end && ok; ++p)
            {
                if (StartsLambda(view, p, body_begin))
                {
                    ok = false;
                }
                else if (!view.IsWord(p))
                {
                    continue;
                }
                else if (view.Text(p) == container_name)
                {
                    ok = false;
                }
                else if (view.Text(p) == iterator_name)
                {
                    if (model.ResolveToken(view.TokenAt(p)) != variable)
                    {
                        ok = false;
                    }
                    else if (p > body_begin && view.At(p - 1) == Tok::Star &&
                        (p - 1 == body_begin ||!EndsOperand(view, p - 2)))
                    {
                        edits.push_back({p - 1, p, element}); // `*it`
                    }
                    else if (view.At(p + 1) == Tok::Arrow && p + 1 < loop.end)
                    {
                        edits.push_back({p, p + 1, element + "."}); // `it->`
                    }
                    else
                    {
                        ok = false;
                    }
                }
            }

            if (!ok || edits.empty() || element.empty())
            {
                continue;
            }

            const bool read_only = begin_name == "cbegin" || table.IsConstQualified(types.SymbolType(container));
            const std::string header = std::string("for (") +(read_only ? "const auto& " : "auto& ") + element +
                " : " + std::string(container_name) + ")";
            diagnostics.push_back(BuildLoopDiagnostic(model, view, loop, RuleId::ModernizeLoopConvert,
                "cpp/modernize-loop-convert",
                "iterator loop over '" + std::string(container_name) + "' can be a range-based for", header, edits,
                lines, lines_built));
        }

        SortByOffset(diagnostics);
        return diagnostics;
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeSpan(const TypeModel& types)
    {
        const auto& model = types.Model();
        const auto& symbols = model.Symbols();
        const auto& scopes = model.Scopes();
        const detail::TokenView view(model);
        Reporter reporter(model);

        static constexpr std::string_view kSizeNames[] = {
            "size", "length", "len", "count", "num", "n", "extent",
        };
        auto is_size_name =[](std::string_view name)
        {
            std::string lower(name);
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](unsigned char c)
                {
                    return static_cast<char>(std::tolower(c));
            });
            for (const auto candidate : kSizeNames)
            {
                if (lower == candidate)
                {
                    return true;
                }
            }

            return false;
        };

        // Parameters grouped by function scope, in declaration order.
        std::vector<SymbolId> ordered;
        for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
        {
            if (symbols.kind[symbol] == SymbolKind::Parameter && symbols.name[symbol] != kNone)
            {
                ordered.push_back(symbol);
            }
        }

        std::sort(ordered.begin(), ordered.end(),
            [&](SymbolId left, SymbolId right)
            {
                if (symbols.scope[left] != symbols.scope[right])
                {
                    return symbols.scope[left] < symbols.scope[right];
            }

                return symbols.decl_token[left] < symbols.decl_token[right];
        });
        for (std::size_t i = 0; i + 1 < ordered.size(); ++i)
        {
            const SymbolId first = ordered[i];
            const SymbolId second = ordered[i + 1];
            if (symbols.scope[first] != symbols.scope[second])
            {
                continue;
            }

            if ((symbols.flags[first] & SymbolFlag::Pointer) == 0)
            {
                continue;
            }

            const std::string_view second_name = model.Names().Text(symbols.name[second]);
            if (!is_size_name(second_name))
            {
                continue;
            }

            const TypeId second_type = types.SymbolType(second);
            if (!types.Types().IsInteger(second_type))
            {
                continue;
            }

            // The owning function must be one a span can replace: templates,
            // virtuals and overrides would break callers or overriders, and
            // both types must be model-resolved. Free functions and lambdas
            // (owner kNone) are eligible; anything inside a class is not: a
            // definition fixed without its declaration (or an overrider)
            // breaks the build.
            const ScopeId scope = symbols.scope[first];
            bool inside_class = false;
            bool inside_template = false;
            for (ScopeId ancestor = scope; ancestor < scopes.parent.size() && ancestor != kNone;)
            {
                if (scopes.kind[ancestor] == ScopeKind::Class)
                {
                    inside_class = true;
                    break;
                }

                const ScopeId parent = scopes.parent[ancestor];
                if (parent == ancestor)
                {
                    break;
                }

                ancestor = parent;
            }

            {
                const auto& nodes = model.Tree().NodesSoA();
                std::uint32_t node = scope < scopes.node.size() ? scopes.node[scope] : 0;
                for (std::size_t steps = 0; steps <= nodes.size(); ++steps)
                {
                    if (nodes.Kind(node) == GrammarKind::TemplateDeclaration)
                    {
                        inside_template = true;
                        break;
                    }

                    if (node == 0)
                    {
                        break;
                    }

                    const std::uint32_t parent = nodes.Parent(node);
                    if (parent >= nodes.size() || parent == node)
                    {
                        break;
                    }

                    node = parent;
                }
            }

            const SymbolId owner =
                scope < scopes.owner.size() ? scopes.owner[scope] : kNone;
            if (inside_class || inside_template ||
                (owner != kNone && symbols.kind[owner] == SymbolKind::Function &&
                (symbols.flags[owner] &
                (SymbolFlag::Template | SymbolFlag::Virtual | SymbolFlag::Override | SymbolFlag::Final |
                SymbolFlag::Constructor | SymbolFlag::Destructor)) != 0))
            {
                continue;
            }

            // The pointee spelling: plain names, `::`, cv-qualifiers and
            // arithmetic keywords back from the `*`. (Keywords carry a Tok,
            // so IsWord rejects them; match by text here.) References,
            // templates and anything else get the diagnostic without a fix.
            const std::size_t name_position = view.PositionOf(symbols.decl_token[first]);
            std::size_t cursor = name_position;
            std::size_t star = view.Size();
            while (cursor > 0)
            {
                --cursor;
                if (view.Text(cursor) == "*")
                {
                    star = cursor;
                    break;
                }

                if (!view.IsWord(cursor))
                {
                    break;
                }
            }

            if (star >= view.Size())
            {
                continue;
            }

            auto is_spelling_word =[](std::string_view text)
            {
                return text == "const" || text == "volatile" || text == "unsigned" || text == "signed" ||
                    text == "int" || text == "char" || text == "short" || text == "long" ||
                    text == "float" || text == "double" || text == "bool" || text == "void" ||
                    text == "size_t" || text == "wchar_t";
            };

            std::size_t type_begin = star;
            bool saw_word = false;
            cursor = star;
            while (cursor > 0)
            {
                --cursor;
                const auto text = view.Text(cursor);
                if (view.IsWord(cursor) || is_spelling_word(text))
                {
                    saw_word = true;
                    type_begin = cursor;
                    continue;
                }

                if (text == "::")
                {
                    type_begin = cursor;
                    continue;
                }

                break;
            }

            const bool type_ok = saw_word;
            // A default argument on either parameter would be dropped by the
            // merge: diagnose, but offer no fix.
            bool has_default = false;
            {
                const std::size_t second_end =
                    view.PositionOf(symbols.decl_token[second]) + 1;
                for (std::size_t k = type_begin; k < second_end && k < view.Size(); ++k)
                {
                    if (view.Text(k) == "=")
                    {
                        has_default = true;
                        break;
                    }
                }
            }

            const std::size_t fix_begin = view.Offset(type_begin);
            const std::size_t span_end = view.End(view.PositionOf(symbols.decl_token[second]));
            // Rebuild the pointee spelling from the token range.
            std::string pointee_spelling;
            for (std::size_t k = type_begin; k < star; ++k)
            {
                const auto text = view.Text(k);
                if (text == "*" || text.empty())
                {
                    continue;
                }

                if (!pointee_spelling.empty() && text != "::" && !pointee_spelling.ends_with("::"))
                {
                    pointee_spelling += ' ';
                }

                pointee_spelling += text;
            }

            const std::string first_name(model.Names().Text(symbols.name[first]));
            const bool void_pointee = pointee_spelling == "void";
            if (type_ok && !void_pointee && !has_default && !pointee_spelling.empty())
            {
                reporter.Report(RuleId::ModernizeSpan, "cpp/modernize-span",
                    "pointer and size parameters '" + first_name + "' and '" + std::string(second_name) +
                    "' can be a single std::span",
                    fix_begin, span_end - fix_begin, fix_begin, span_end - fix_begin,
                    "std::span<" + pointee_spelling + "> " + first_name, false,
                    "Replace '" + first_name + "' and '" + std::string(second_name) + "' with std::span");
            }
            else
            {
                reporter.ReportNoFix(RuleId::ModernizeSpan, "cpp/modernize-span",
                    "pointer and size parameters '" + first_name + "' and '" + std::string(second_name) +
                    "' can be a single std::span",
                    fix_begin, span_end - fix_begin);
            }
        }

        return reporter.Take();
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeStringView(const SemanticModel& model)
    {
        const auto& symbols = model.Symbols();
        const auto& scopes = model.Scopes();
        const detail::TokenView view(model);
        Reporter reporter(model);
        for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
        {
            if (symbols.kind[symbol] != SymbolKind::Parameter || symbols.name[symbol] == kNone)
            {
                continue;
            }

            const ScopeId scope = symbols.scope[symbol];
            const SymbolId owner =
                scope < scopes.owner.size() ? scopes.owner[scope] : kNone;
            if (owner != kNone && symbols.kind[owner] == SymbolKind::Function &&
                (symbols.flags[owner] & SymbolFlag::Template) != 0)
            {
                continue;
            }

            // The type slice back from the name: `const`, `std`, `::`,
            // `string` only. Any `*`, `&`, `<` or other token means the
            // parameter is not a by-value string.
            const std::size_t name_position = view.PositionOf(symbols.decl_token[symbol]);
            std::size_t type_begin = name_position;
            bool has_const = false;
            bool has_std = false;
            bool has_scope = false;
            bool has_string = false;
            bool shape_ok = true;
            std::size_t cursor = name_position;
            while (cursor > 0)
            {
                --cursor;
                const auto text = view.Text(cursor);
                if (text == "const")
                {
                    has_const = true;
                    type_begin = cursor;
                    continue;
                }

                if (view.IsWord(cursor))
                {
                    if (text == "std")
                    {
                        has_std = true;
                    }
                    else if (text == "string")
                    {
                        has_string = true;
                    }
                    else
                    {
                        shape_ok = false;
                        break;
                    }

                    type_begin = cursor;
                    continue;
                }

                if (text == "::")
                {
                    has_scope = true;
                    type_begin = cursor;
                    continue;
                }

                break;
            }

            if (!shape_ok ||!has_const ||!has_std ||!has_scope ||!has_string)
            {
                continue;
            }

            // An array parameter (`const std::string s[]`) is not a copy.
            const std::size_t after_name = name_position + 1;
            if (after_name < view.Size() && view.Text(after_name) == "[")
            {
                continue;
            }

            const std::size_t fix_begin = view.Offset(type_begin);
            const std::size_t name_end = view.End(name_position);
            const std::string name(model.Names().Text(symbols.name[symbol]));
            reporter.Report(RuleId::ModernizeStringView, "cpp/modernize-string-view",
                "parameter '" + name + "' copies a const std::string; take std::string_view",
                fix_begin, name_end - fix_begin, fix_begin, name_end - fix_begin, "std::string_view " + name,
                false, "Take std::string_view for '" + name + '\'');
        }

        return reporter.Take();
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeAttributes(const TypeModel& types)
    {
        const auto& model = types.Model();
        const auto& symbols = model.Symbols();
        const detail::TokenView view(model);
        Reporter reporter(model);
        static constexpr std::string_view kQueryPrefixes[] = {
            "get", "is", "has", "have", "can", "could", "should", "would", "may", "empty", "size", "count",
            "length", "find", "contains", "front", "back", "top", "data", "at", "value", "make", "create",
            "clone", "copy", "exists", "equal", "compare", "starts", "ends", "first", "last", "peek",
        };
        auto is_query_name =[](std::string_view name)
        {
            std::string lower(name);
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](unsigned char c)
                {
                    return static_cast<char>(std::tolower(c));
            });
            for (const auto prefix : kQueryPrefixes)
            {
                if (lower.size() >= prefix.size() && lower.compare(0, prefix.size(), prefix) == 0 &&
                    (lower.size() == prefix.size() ||!(lower[prefix.size()] >= 'a' &&
                    lower[prefix.size()] <= 'z')))
                {
                    return true;
                }
            }

            return false;
        };

        const std::string_view source = model.Tree().Source();
        for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
        {
            if (symbols.kind[symbol] != SymbolKind::Function || symbols.name[symbol] == kNone)
            {
                continue;
            }

            if ((symbols.flags[symbol] & (SymbolFlag::Constructor | SymbolFlag::Destructor |
                SymbolFlag::Operator | SymbolFlag::Template)) != 0)
            {
                continue;
            }

            const std::string name(model.Names().Text(symbols.name[symbol]));
            if (name == "main" ||!is_query_name(name))
            {
                continue;
            }

            const TypeId returned = types.SymbolType(symbol);
            if (returned == TypeTable::Unknown ||
                types.Types().IsBuiltin(returned, BuiltinType::Void))
            {
                continue;
            }

            const auto& name_token = model.Tree().Tokens()[symbols.decl_token[symbol]];
            // Already annotated: `[[nodiscard]]` (or the GNU spelling) in the
            // declaration prefix.
            const std::size_t window_begin =
                name_token.offset > 300 ? name_token.offset - 300 : 0;
            if (source.substr(window_begin, name_token.offset - window_begin).find("nodiscard") !=
                std::string_view::npos)
            {
                continue;
            }

            // Front placement (`[[nodiscard]] int f();`) when the declaration
            // prefix back from the name is only type material; otherwise the
            // attribute goes right before the name (`int [[nodiscard]] f()`,
            // also valid). A `,` on the way means a shared declaration
            // (`int a, f();`), where front placement would mistarget.
            const std::size_t name_position = view.PositionOf(symbols.decl_token[symbol]);
            std::size_t insert = name_position;
            std::size_t cursor = name_position;
            bool front = true;
            while (cursor > 0)
            {
                --cursor;
                const auto text = view.Text(cursor);
                const bool accept = view.IsWord(cursor) || text == "::" || text == "*" || text == "&" ||
                    text == "<" || text == ">" || text == "const" || text == "constexpr" ||
                    text == "static" || text == "inline" || text == "virtual" || text == "explicit" ||
                    text == "friend" || text == "noexcept" || text == "unsigned" || text == "signed" ||
                    text == "int" || text == "char" || text == "short" || text == "long" ||
                    text == "float" || text == "double" || text == "bool" || text == "void" ||
                    text == "wchar_t" || text == "size_t" || text == "auto";
                if (!accept)
                {
                    front = text == ";" || text == "{" || text == "}" || text == ":" ||
                        text == "public" || text == "private" || text == "protected";
                    break;
                }

                insert = cursor;
            }

            const std::size_t fix_offset = front ? view.Offset(insert) : name_token.offset;
            reporter.Report(RuleId::ModernizeAttributes, "cpp/modernize-attributes",
                "'" + name + "' returns a value that should not be discarded; add [[nodiscard]]",
                name_token.offset, name_token.length, fix_offset, 0, "[[nodiscard]] ", false,
                "Add [[nodiscard]] to " + name);
        }

        return reporter.Take();
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeConstevalConstexpr(const SemanticModel& model)
    {
        const auto& symbols = model.Symbols();
        const auto& scopes = model.Scopes();
        const detail::TokenView view(model);
        Reporter reporter(model);
        auto is_arithmetic_word =[](std::string_view word)
        {
            return word == "int" || word == "char" || word == "short" || word == "long" ||
                word == "signed" || word == "unsigned" || word == "float" || word == "double" ||
                word == "bool" || word == "size_t";
        };

        for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
        {
            if (symbols.kind[symbol] != SymbolKind::Variable || symbols.name[symbol] == kNone)
            {
                continue;
            }

            const ScopeId scope = symbols.scope[symbol];
            if (scope >= scopes.kind.size() ||
                (scopes.kind[scope] != ScopeKind::TranslationUnit && scopes.kind[scope] != ScopeKind::Namespace))
            {
                continue;
            }

            // Shape: [static|inline] const TYPE name = literal, with TYPE one
            // or two arithmetic words (`unsigned int`). `extern`,
            // `thread_local`, `mutable`, `volatile` and an existing
            // `constexpr` stay out.
            const std::size_t name_position = view.PositionOf(symbols.decl_token[symbol]);
            std::size_t cursor = name_position;
            std::vector<std::size_t> type_words;
            std::size_t const_position = view.Size();
            bool shape_ok = true;
            bool boundary = false;
            while (type_words.size() < 4)
            {
                if (cursor == 0)
                {
                    // The whole prefix was consumed: start of file.
                    boundary = true;
                    break;
                }

                --cursor;
                const auto text = view.Text(cursor);
                if (text == "const")
                {
                    if (const_position < view.Size())
                    {
                        shape_ok = false;
                        break;
                    }

                    const_position = cursor;
                    continue;
                }

                if (text == "static" || text == "inline")
                {
                    continue;
                }

                // Arithmetic keywords (`int`, `unsigned`) carry a Tok, so
                // match by text; an identifier can never spell them.
                if (is_arithmetic_word(text))
                {
                    type_words.push_back(cursor);
                    continue;
                }

                // Anything else ends the declaration prefix; it must be a
                // statement boundary for this to be a plain declaration.
                boundary = text == ";" || text == "{" || text == "}" || text == ":" ||
                    text == "if" || text == "else" || text == "for" || text == "while" || text == "do" ||
                    text == "case" || text == "return";
                break;
            }

            if (!shape_ok || const_position >= view.Size() || type_words.empty() || type_words.size() > 2)
            {
                continue;
            }

            // The token where the walk stopped must be a boundary.
            if (!boundary)
            {
                continue;
            }

            const std::size_t equal = name_position + 1;
            if (equal >= view.Size() || view.Text(equal) != "=")
            {
                continue;
            }

            const std::size_t init = equal + 1;
            if (init >= view.Size())
            {
                continue;
            }

            const auto init_text = view.Text(init);
            const bool literal_init = view.KindAt(init) == TokenKind::Number || init_text == "true" ||
                init_text == "false" || init_text == "nullptr";
            if (!literal_init)
            {
                continue;
            }

            const std::string name(model.Names().Text(symbols.name[symbol]));
            const std::size_t const_offset = view.Offset(const_position);
            const std::size_t const_end = view.End(const_position);
            // Namespace-scope `const` already has internal linkage, and the
            // initializer is a literal: `constexpr` changes neither linkage
            // nor value, so the fix is safe in batch.
            reporter.Report(RuleId::ModernizeConstevalConstexpr, "cpp/modernize-consteval-constexpr",
                "constant '" + name + "' can be constexpr", const_offset, const_end - const_offset,
                const_offset, const_end - const_offset, "constexpr", true,
                "Make '" + name + "' constexpr");
        }

        return reporter.Take();
    }

    std::vector<Diagnostic> SemanticRules::Analyze(const SemanticModel& model)
    {
        return Analyze(model, Typer::Type(model));
    }

    std::vector<Diagnostic> SemanticRules::Analyze(const SemanticModel& model, const TypeModel& types)
    {
        const auto flow = Flow::Build(types);
        auto all = AnalyzeOverride(model);
        for (auto&& part :
            {
                AnalyzeNullptr(model), AnalyzeZeroAsNull(model), AnalyzeAuto(model),
                AnalyzeImplicitBool(types), AnalyzeRangeLoop(types), AnalyzeLoopConvert(types), AnalyzeConst(flow),
                AnalyzeConstexpr(flow), AnalyzeSpan(types), AnalyzeStringView(model), AnalyzeAttributes(types),
                AnalyzeConstevalConstexpr(model), AnalyzeVirtualDestructor(model),
                AnalyzeExplicitConstructor(model),
                AnalyzeOverloadHiding(model), AnalyzeVirtualCallInConstructor(model),
                AnalyzeMissingNodiscard(types), AnalyzePassByValue(types), AnalyzePassByConstReference(flow),
                AnalyzeConstCorrectness(flow), AnalyzeUnsafeDowncast(types), AnalyzeSlicing(types),
                AnalyzeImplicitConversion(model), AnalyzeDesignatedInitOrder(model), AnalyzeIntegerToPointer(model)
        })
        {
            all.insert(all.end(), part.begin(), part.end());
        }

        std::stable_sort(all.begin(), all.end(),
            [](const Diagnostic& a, const Diagnostic& b)
            {
                return a.offset < b.offset;
        });
        return all;
    }

} // namespace heimdall
