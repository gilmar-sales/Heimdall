#include <Heimdall/LineTable.hpp>
#include <Heimdall/SemanticRules.hpp>

#include "detail/ClassHead.hpp"
#include "detail/RuleSupport.hpp"
#include "detail/TokenView.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace heimdall
{

    namespace
    {

        // Deepest base chain followed; deeper is Unknown, not a stack overflow.
        constexpr std::size_t kMaxDepth = 256;
        constexpr std::uint32_t kVirtualish = SymbolFlag::Virtual | SymbolFlag::Override | SymbolFlag::Final |
            SymbolFlag::Pure;
        constexpr std::uint32_t kNotMember = SymbolFlag::Friend | SymbolFlag::Static | SymbolFlag::Qualified;

        enum class Access : std::uint8_t
        {
            Public,
            Protected,
            Private
        };

        // Three-valued answer: the rules only act on a certain No.
        enum class Tri : std::uint8_t
        {
            No,
            Yes,
            Unknown
        };

        struct ClassBody
        {
            std::size_t open = 0;  // position of `{`
            std::size_t close = 0; // position of `}`
            bool is_class = false; // `class` (private by default) rather than `struct`
        };

        // Shared state of the four rules: token view, line table and the
        // helpers that read a class body.
        class ApiAnalysis
        {
        public:
            explicit ApiAnalysis(const SemanticModel &model) : m_model(model), m_view(model),
              m_symbols(model.Symbols()), m_scopes(model.Scopes()), m_tree(model.Tree())
            {
            }

            const SemanticModel &Model() const
            {
                return m_model;
            }
            const detail::TokenView &View() const
            {
                return m_view;
            }
            const SymbolTable &Symbols() const
            {
                return m_symbols;
            }
            const ScopeTable &Scopes() const
            {
                return m_scopes;
            }

            // The class a member declared directly in `scope` belongs to, or kNone.
            SymbolId OwnerClass(ScopeId scope) const
            {
                return m_scopes.kind[scope] == ScopeKind::Class ? m_scopes.owner[scope] : kNone;
            }

            // Braces and keyword of a struct/class definition; unions, enums and
            // declarations without a body give nothing.
            std::optional<ClassBody> BodyOf(SymbolId klass) const
            {
                if (m_symbols.kind[klass] != SymbolKind::Class)
                {
                    return std::nullopt;
                }

                const auto [begin, end] = m_view.Range(m_symbols.decl_node[klass]);
                const auto name = m_view.PositionOf(m_symbols.decl_token[klass]);
                ClassBody body;
                bool found_keyword = false;
                for (auto i = begin; i < name && i < end; ++i)
                {
                    const Tok tok = m_view.At(i);
                    if (tok == Tok::KwUnion)
                    {
                        return std::nullopt;
                    }

                    if (tok == Tok::KwClass || tok == Tok::KwStruct)
                    {
                        body.is_class = tok == Tok::KwClass;
                        found_keyword = true;
                    }
                }

                if (!found_keyword)
                {
                    return std::nullopt;
                }

                for (body.open = name; body.open < end && m_view.At(body.open) != Tok::LBrace; ++body.open)
                {
                    if (m_view.At(body.open) == Tok::Semi)
                    {
                        return std::nullopt;
                    }
                }

                if (body.open >= end)
                {
                    return std::nullopt;
                }

                body.close = m_view.Match(body.open, m_view.Size());
                return body.close < m_view.Size() ? std::optional<ClassBody>(body) : std::nullopt;
            }

            // Access level in force at `position` among the direct members of `body`.
            Access AccessAt(const ClassBody &body, std::size_t position) const
            {
                Access access = body.is_class ? Access::Private : Access::Public;
                for (auto i = body.open + 1; i < position && i < body.close; ++i)
                {
                    const Tok tok = m_view.At(i);
                    if (tok == Tok::LBrace)
                    {
                        i = m_view.Match(i, m_view.Size());
                    }
                    else if (m_view.At(i + 1) == Tok::Colon && (tok == Tok::KwPublic || tok == Tok::KwProtected ||
                                 tok == Tok::KwPrivate))
                    {
                        access = tok == Tok::KwPublic ? Access::Public
                            : tok == Tok::KwProtected ? Access::Protected
                                                      : Access::Private;
                    }
                }

                return access;
            }

            // First token of the member declaration whose name is at `name`, after
            // any `template<...>` header: where a specifier or a using-declaration
            // can be inserted. `has_template` tells a template header was skipped.
            std::size_t DeclarationStart(const ClassBody &body, std::size_t name, bool &has_template) const
            {
                std::size_t start = name;
                while (start > body.open + 1)
                {
                    const Tok before = m_view.At(start - 1);
                    if (before == Tok::Semi || before == Tok::LBrace || before == Tok::RBrace ||
                        before == Tok::Colon)
                    {
                        break;
                    }

                    --start;
                }

                has_template = false;
                if (m_view.At(start) == Tok::KwTemplate && m_view.At(start + 1) == Tok::Lt)
                {
                    const auto close = m_view.MatchAngle(start + 1, name);
                    if (close < name)
                    {
                        start = close + 1;
                        has_template = true;
                    }
                }

                return start;
            }

            Diagnostic Make(RuleId rule, std::string_view code, std::string message, std::size_t position)
            {
                if (!m_lines_built)
                {
                    m_lines.Build(m_tree.Source());
                    m_lines_built = true;
                }

                const auto offset = m_view.Offset(position);
                const auto position_in_file = m_lines.Lookup(offset);
                return Diagnostic{rule, Severity::Warning, std::string(code), std::move(message), offset,
                    m_view.End(position) - offset, position_in_file.line, position_in_file.column, false, TextEdit{}};
            }

            // The fix rests on what the model could see: a quick fix, never applied in batch.
            static void AddQuickFix(Diagnostic &diagnostic, std::size_t offset, std::string replacement,
                std::string title)
            {
                diagnostic.has_fix = true;
                diagnostic.fix = TextEdit{offset, 0, std::move(replacement)};
                diagnostic.fix_is_safe = false;
                diagnostic.fix_title = std::move(title);
            }

        private:
            const SemanticModel &m_model;
            detail::TokenView m_view;
            const SymbolTable &m_symbols;
            const ScopeTable &m_scopes;
            const ParseTree &m_tree;
            LineTable m_lines;
            bool m_lines_built = false;
        };

        bool IsVirtualish(const SymbolTable &symbols, SymbolId function)
        {
            return (symbols.flags[function] & kVirtualish) != 0;
        }

        // ---------------------------------------------------------------------
        // api/virtual-destructor

        class VirtualDestructor
        {
        public:
            explicit VirtualDestructor(ApiAnalysis &analysis) : m_analysis(analysis),
              m_destructor(analysis.Symbols().Size(), kNone), m_introduces_virtual(analysis.Symbols().Size(), 0)
            {
                const auto &symbols = analysis.Symbols();
                for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
                {
                    if (symbols.kind[symbol] != SymbolKind::Function)
                    {
                        continue;
                    }

                    const auto klass = analysis.OwnerClass(symbols.scope[symbol]);
                    if (klass == kNone)
                    {
                        continue;
                    }

                    const auto flags = symbols.flags[symbol];
                    if ((flags & SymbolFlag::Destructor) != 0)
                    {
                        m_destructor[klass] = symbol;
                    }
                    else if ((flags & (SymbolFlag::Virtual | SymbolFlag::Pure)) != 0 &&
                        (flags & SymbolFlag::Constructor) == 0)
                    {
                        m_introduces_virtual[klass] = 1;
                    }
                }
            }

            std::vector<Diagnostic> Run()
            {
                std::vector<Diagnostic> diagnostics;
                const auto &symbols = m_analysis.Symbols();
                const auto &model = m_analysis.Model();
                for (SymbolId klass = 0; klass < symbols.Size(); ++klass)
                {
                    if (symbols.kind[klass] != SymbolKind::Class || m_introduces_virtual[klass] == 0)
                    {
                        continue;
                    }

                    const auto body = m_analysis.BodyOf(klass);
                    if (!body || detail::ClassHeadIsFinal(model.Tree(), symbols.decl_token[klass]))
                    {
                        continue;
                    }

                    const auto destructor = m_destructor[klass];
                    const auto &view = m_analysis.View();
                    std::size_t name_position = 0;
                    if (destructor != kNone)
                    {
                        name_position = view.PositionOf(symbols.decl_token[destructor]);
                        // A protected or private non-virtual destructor is the
                        // sanctioned way to forbid deleting through the base.
                        if (m_analysis.AccessAt(*body, name_position) != Access::Public)
                        {
                            continue;
                        }
                    }

                    if (HasVirtualDestructor(klass, 0) != Tri::No)
                    {
                        continue;
                    }

                    const auto name = view.PositionOf(symbols.decl_token[klass]);
                    const std::string class_name(model.Names().Text(symbols.name[klass]));
                    auto diagnostic = m_analysis.Make(RuleId::ApiVirtualDestructor, "api/virtual-destructor",
                        "class '" + class_name + "' has virtual functions but its destructor is not virtual; "
                        "deleting a derived object through a '" + class_name + "' pointer is undefined behavior",
                        name);
                    if (destructor != kNone && name_position > 0 && view.At(name_position - 1) == Tok::Tilde)
                    {
                        ApiAnalysis::AddQuickFix(diagnostic, view.Offset(name_position - 1), "virtual ",
                            "Make the destructor of " + class_name + " virtual");
                    }

                    diagnostics.push_back(std::move(diagnostic));
                }

                return diagnostics;
            }

        private:
            // Yes when the destructor is virtual (written, or inherited from a base
            // whose destructor is); Unknown when a base does not resolve.
            Tri HasVirtualDestructor(SymbolId klass, std::size_t depth) const
            {
                const auto &symbols = m_analysis.Symbols();
                if (depth > kMaxDepth)
                {
                    return Tri::Unknown;
                }

                const auto destructor = m_destructor[klass];
                if (destructor != kNone && IsVirtualish(symbols, destructor))
                {
                    return Tri::Yes;
                }

                const auto &bases = m_analysis.Model().Bases();
                Tri result = Tri::No;
                for (std::uint32_t i = 0; i < symbols.base_count[klass]; ++i)
                {
                    const auto target = bases.target[symbols.first_base[klass] + i];
                    const Tri base = target == kNone ? Tri::Unknown : HasVirtualDestructor(target, depth + 1);
                    if (base == Tri::Yes)
                    {
                        return Tri::Yes;
                    }

                    if (base == Tri::Unknown)
                    {
                        result = Tri::Unknown;
                    }
                }

                return result;
            }

            ApiAnalysis &m_analysis;
            std::vector<SymbolId> m_destructor;
            std::vector<std::uint8_t> m_introduces_virtual;
        };

        // ---------------------------------------------------------------------
        // api/explicit-constructor

        // The parameters of the list that opens at `open`: [begin, end) positions
        // of each one. False when the list does not close.
        bool SplitParameters(const detail::TokenView &view, std::size_t open,
            std::vector<std::pair<std::size_t, std::size_t>> &parameters)
        {
            const auto close = view.Match(open, view.Size());
            if (close >= view.Size())
            {
                return false;
            }

            std::size_t begin = open + 1;
            for (std::size_t i = open + 1; i < close; ++i)
            {
                const Tok tok = view.At(i);
                if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace)
                {
                    i = view.Match(i, close);
                }
                else if (tok == Tok::Lt)
                {
                    const auto angle = view.MatchAngle(i, close);
                    i = angle < close ? angle : i;
                }
                else if (tok == Tok::Comma)
                {
                    parameters.emplace_back(begin, i);
                    begin = i + 1;
                }
            }

            if (begin < close)
            {
                parameters.emplace_back(begin, close);
            }

            return true;
        }

        bool HasToken(const detail::TokenView &view, std::pair<std::size_t, std::size_t> range, Tok tok)
        {
            for (auto i = range.first; i < range.second; ++i)
            {
                if (view.At(i) == tok)
                {
                    return true;
                }
            }

            return false;
        }

        // The parameter refers to the class itself by reference (copy/move
        // constructor) or is a braced-list type: conversions there are intended.
        bool IsCopyMoveOrListParameter(const detail::TokenView &view, std::pair<std::size_t, std::size_t> range,
            std::string_view class_name)
        {
            bool saw_class = false;
            for (auto i = range.first; i < range.second; ++i)
            {
                if (view.IsWord(i) && view.Text(i) == "initializer_list")
                {
                    return true;
                }

                if (view.IsWord(i) && view.Text(i) == class_name)
                {
                    saw_class = true;
                }
                else if (saw_class && (view.At(i) == Tok::Amp || view.At(i) == Tok::AmpAmp))
                {
                    return true;
                }
            }

            return false;
        }

        std::vector<Diagnostic> ExplicitConstructor(ApiAnalysis &analysis)
        {
            std::vector<Diagnostic> diagnostics;
            const auto &symbols = analysis.Symbols();
            const auto &view = analysis.View();
            const auto &model = analysis.Model();
            std::vector<std::pair<std::size_t, std::size_t>> parameters;
            for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
            {
                constexpr std::uint32_t skipped = SymbolFlag::Friend | SymbolFlag::Defaulted | SymbolFlag::Qualified;
                if (symbols.kind[symbol] != SymbolKind::Function ||
                    (symbols.flags[symbol] & SymbolFlag::Constructor) == 0 ||
                    (symbols.flags[symbol] & skipped) != 0)
                {
                    continue;
                }

                const auto klass = analysis.OwnerClass(symbols.scope[symbol]);
                const auto body = klass == kNone ? std::nullopt : analysis.BodyOf(klass);
                if (!body)
                {
                    continue;
                }

                const auto name = view.PositionOf(symbols.decl_token[symbol]);
                if (view.At(name + 1) != Tok::LParen)
                {
                    continue;
                }

                parameters.clear();
                if (!SplitParameters(view, name + 1, parameters) || parameters.empty())
                {
                    continue;
                }

                const auto &first = parameters.front();
                const std::string_view class_name = model.Names().Text(symbols.name[klass]);
                bool callable_with_one = true;
                for (std::size_t i = 1; i < parameters.size(); ++i)
                {
                    callable_with_one = callable_with_one && HasToken(view, parameters[i], Tok::Eq);
                }

                const bool plain_void = first.second == first.first + 1 && view.At(first.first) == Tok::KwVoid;
                if (!callable_with_one || plain_void || HasToken(view, first, Tok::Ellipsis) ||
                    IsCopyMoveOrListParameter(view, first, class_name))
                {
                    continue;
                }

                bool has_template = false;
                const auto start = analysis.DeclarationStart(*body, name, has_template);
                bool already_explicit = false;
                for (auto i = start; i < name; ++i)
                {
                    already_explicit = already_explicit || view.At(i) == Tok::KwExplicit;
                }

                if (already_explicit)
                {
                    continue;
                }

                auto diagnostic = analysis.Make(RuleId::ApiExplicitConstructor, "api/explicit-constructor",
                    "constructor of '" + std::string(class_name) + "' callable with one argument is not 'explicit'; "
                    "it allows accidental implicit conversions",
                    name);
                ApiAnalysis::AddQuickFix(diagnostic, view.Offset(start), "explicit ", "Add 'explicit'");
                diagnostics.push_back(std::move(diagnostic));
            }

            return diagnostics;
        }

        // ---------------------------------------------------------------------
        // api/overload-hiding

        // The class body names `using ...::name;`.
        bool HasUsingFor(const detail::TokenView &view, const ClassBody &body, std::string_view name)
        {
            for (auto i = body.open + 1; i < body.close; ++i)
            {
                if (view.At(i) != Tok::KwUsing)
                {
                    continue;
                }

                auto end = i + 1;
                while (end < body.close && view.At(end) != Tok::Semi)
                {
                    ++end;
                }

                if (end > i + 2 && view.Text(end - 1) == name && view.At(end - 2) == Tok::ColonColon)
                {
                    return true;
                }

                i = end;
            }

            return false;
        }

        std::vector<Diagnostic> OverloadHiding(ApiAnalysis &analysis)
        {
            std::vector<Diagnostic> diagnostics;
            const auto &symbols = analysis.Symbols();
            const auto &view = analysis.View();
            const auto &model = analysis.Model();
            const auto &bases = model.Bases();
            std::unordered_set<std::uint64_t> seen;
            std::vector<SymbolId> pending;
            std::unordered_set<SymbolId> visited;
            for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
            {
                constexpr std::uint32_t skipped = kNotMember | SymbolFlag::Constructor | SymbolFlag::Destructor |
                    SymbolFlag::Operator | SymbolFlag::Template;
                if (symbols.kind[symbol] != SymbolKind::Function || (symbols.flags[symbol] & skipped) != 0 ||
                    symbols.name[symbol] == kNone)
                {
                    continue;
                }

                const auto klass = analysis.OwnerClass(symbols.scope[symbol]);
                if (klass == kNone || symbols.base_count[klass] == 0 ||
                    !seen.insert(std::uint64_t{klass} << 32 | symbols.name[symbol]).second)
                {
                    continue;
                }

                const auto body = analysis.BodyOf(klass);
                const auto name_text = model.Names().Text(symbols.name[symbol]);
                if (!body || HasUsingFor(view, *body, name_text))
                {
                    continue;
                }

                // The derived overloads; one with an unknown signature makes the
                // comparison meaningless.
                std::vector<std::uint64_t> derived_signatures;
                bool known = true;
                for (auto member = model.LookupLocal(symbols.member_scope[klass], symbols.name[symbol]);
                    member != kNone; member = symbols.next_same_name[member])
                {
                    if (symbols.kind[member] == SymbolKind::Function)
                    {
                        known = known && symbols.signature[member] != 0;
                        derived_signatures.push_back(symbols.signature[member]);
                    }
                }

                if (!known)
                {
                    continue;
                }

                SymbolId hidden_in = kNone;
                pending.clear();
                visited.clear();
                visited.insert(klass);
                const auto push_bases = [&](SymbolId derived)
                {
                    for (std::uint32_t i = 0; i < symbols.base_count[derived]; ++i)
                    {
                        if (const auto target = bases.target[symbols.first_base[derived] + i]; target != kNone)
                        {
                            pending.push_back(target);
                        }
                    }
                };

                push_bases(klass);
                while (!pending.empty() && hidden_in == kNone && visited.size() < kMaxDepth * kMaxDepth)
                {
                    const auto base = pending.back();
                    pending.pop_back();
                    if (!visited.insert(base).second)
                    {
                        continue;
                    }

                    for (auto member = model.LookupLocal(symbols.member_scope[base], symbols.name[symbol]);
                        member != kNone; member = symbols.next_same_name[member])
                    {
                        const bool candidate = symbols.kind[member] == SymbolKind::Function &&
                            IsVirtualish(symbols, member) && (symbols.flags[member] & kNotMember) == 0 &&
                            symbols.signature[member] != 0;
                        if (candidate && std::find(derived_signatures.begin(), derived_signatures.end(),
                                symbols.signature[member]) == derived_signatures.end())
                        {
                            hidden_in = base;
                            break;
                        }
                    }

                    push_bases(base);
                }

                if (hidden_in == kNone)
                {
                    continue;
                }

                const auto name = view.PositionOf(symbols.decl_token[symbol]);
                const std::string base_name(model.Names().Text(symbols.name[hidden_in]));
                auto diagnostic = analysis.Make(RuleId::ApiOverloadHiding, "api/overload-hiding",
                    "'" + std::string(name_text) + "' hides the virtual overloads of '" + base_name +
                    "'; add 'using " + base_name + "::" + std::string(name_text) + ";'",
                    name);
                bool has_template = false;
                const auto start = analysis.DeclarationStart(*body, name, has_template);
                if (!has_template)
                {
                    ApiAnalysis::AddQuickFix(diagnostic, view.Offset(start),
                        "using " + base_name + "::" + std::string(name_text) + "; ",
                        "Add 'using " + base_name + "::" + std::string(name_text) + ";'");
                }

                diagnostics.push_back(std::move(diagnostic));
            }

            return diagnostics;
        }

        // ---------------------------------------------------------------------
        // api/virtual-call-in-constructor

        class VirtualCall
        {
        public:
            explicit VirtualCall(ApiAnalysis &analysis) : m_analysis(analysis) {}

            std::vector<Diagnostic> Run()
            {
                std::vector<Diagnostic> diagnostics;
                const auto &symbols = m_analysis.Symbols();
                const auto &model = m_analysis.Model();
                const auto &view = m_analysis.View();
                for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
                {
                    constexpr std::uint32_t special = SymbolFlag::Constructor | SymbolFlag::Destructor;
                    if (symbols.kind[symbol] != SymbolKind::Function || (symbols.flags[symbol] & special) == 0 ||
                        (symbols.flags[symbol] & SymbolFlag::Definition) == 0)
                    {
                        continue;
                    }

                    const auto klass = m_analysis.OwnerClass(symbols.scope[symbol]);
                    if (klass == kNone || detail::ClassHeadIsFinal(model.Tree(), symbols.decl_token[klass]))
                    {
                        continue;
                    }

                    const auto body = BodyOfFunction(symbols.decl_node[symbol]);
                    if (!body)
                    {
                        continue;
                    }

                    const bool constructor = (symbols.flags[symbol] & SymbolFlag::Constructor) != 0;
                    for (auto q = body->first + 1; q < body->second; ++q)
                    {
                        const auto callee = VirtualCallee(symbols.decl_node[symbol], q);
                        if (callee == kNone || InLambda(q))
                        {
                            continue;
                        }

                        diagnostics.push_back(m_analysis.Make(RuleId::ApiVirtualCallInConstructor,
                            "api/virtual-call-in-constructor",
                            "virtual function '" + std::string(view.Text(q)) + "' called from a " +
                            (constructor ? "constructor" : "destructor") + " of '" +
                            std::string(model.Names().Text(symbols.name[klass])) +
                            "' does not dispatch to derived classes",
                            q));
                    }
                }

                return diagnostics;
            }

        private:
            std::optional<std::pair<std::size_t, std::size_t>> BodyOfFunction(std::uint32_t node) const
            {
                for (const auto child: m_analysis.Model().ChildrenOf(node))
                {
                    if (m_analysis.Model().Tree().Nodes()[child].kind == GrammarKind::CompoundStatement)
                    {
                        return m_analysis.View().Range(child);
                    }
                }

                return std::nullopt;
            }

            // The virtual member function that the unqualified call at `q` names,
            // or kNone when `q` is not such a call.
            SymbolId VirtualCallee(std::uint32_t function_node, std::size_t q) const
            {
                const auto &view = m_analysis.View();
                const auto &model = m_analysis.Model();
                const auto &symbols = m_analysis.Symbols();
                if (!view.IsWord(q) || view.At(q + 1) != Tok::LParen)
                {
                    return kNone;
                }

                const Tok before = q > 0 ? view.At(q - 1) : Tok::None;
                const bool via_this = before == Tok::Arrow && q > 1 && view.At(q - 2) == Tok::KwThis;
                if (before == Tok::Dot || before == Tok::ColonColon || before == Tok::Gt ||
                    (before == Tok::Arrow && !via_this) || (q > 0 && view.IsWord(q - 1)))
                {
                    return kNone;
                }

                const auto name = model.Names().Find(view.Text(q));
                if (name == kNone)
                {
                    return kNone;
                }

                const auto found = model.Lookup(model.ScopeOfNode(function_node), name, view.TokenAt(q));
                if (found == kNone || symbols.kind[found] != SymbolKind::Function ||
                    m_analysis.OwnerClass(symbols.scope[found]) == kNone)
                {
                    return kNone;
                }

                for (auto member = found; member != kNone; member = symbols.next_same_name[member])
                {
                    if (symbols.kind[member] == SymbolKind::Function && IsVirtualish(symbols, member) &&
                        (symbols.flags[member] & (SymbolFlag::Static | SymbolFlag::Final)) == 0)
                    {
                        return member;
                    }
                }

                return kNone;
            }

            // A call inside a lambda runs when the lambda does, not while the
            // object is being built.
            bool InLambda(std::size_t position)
            {
                if (!m_lambdas_built)
                {
                    const auto &nodes = m_analysis.Model().Tree().Nodes();
                    for (std::uint32_t node = 0; node < nodes.size(); ++node)
                    {
                        if (nodes[node].kind == GrammarKind::LambdaExpression)
                        {
                            m_lambdas.push_back(m_analysis.View().Range(node));
                        }
                    }

                    m_lambdas_built = true;
                }

                return std::any_of(m_lambdas.begin(), m_lambdas.end(),
                    [position](const std::pair<std::size_t, std::size_t> &range)
                    {
                        return position >= range.first && position < range.second;
                    });
            }

            ApiAnalysis &m_analysis;
            std::vector<std::pair<std::size_t, std::size_t>> m_lambdas;
            bool m_lambdas_built = false;
        };

    } // namespace

    std::vector<Diagnostic> SemanticRules::AnalyzeVirtualDestructor(const SemanticModel &model)
    {
        ApiAnalysis analysis(model);
        return VirtualDestructor(analysis).Run();
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeExplicitConstructor(const SemanticModel &model)
    {
        ApiAnalysis analysis(model);
        return ExplicitConstructor(analysis);
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeOverloadHiding(const SemanticModel &model)
    {
        ApiAnalysis analysis(model);
        return OverloadHiding(analysis);
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeVirtualCallInConstructor(const SemanticModel &model)
    {
        ApiAnalysis analysis(model);
        return VirtualCall(analysis).Run();
    }

} // namespace heimdall
