#include <Heimdall/LineTable.hpp>
#include <Heimdall/SemanticRules.hpp>

#include "detail/ClassHead.hpp"
#include "detail/RuleSupport.hpp"
#include "detail/TokenView.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace heimdall
{

    namespace
    {

        // Deepest base chain followed; deeper is Unknown, not a stack overflow.
        constexpr std::size_t   kMaxDepth = 256;
        constexpr std::uint32_t kVirtualish =
            SymbolFlag::Virtual | SymbolFlag::Override | SymbolFlag::Final | SymbolFlag::Pure;
        constexpr std::uint32_t kNotMember =
            SymbolFlag::Friend | SymbolFlag::Static | SymbolFlag::Qualified;

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
            std::size_t open     = 0;     // position of `{`
            std::size_t close    = 0;     // position of `}`
            bool        is_class = false; // `class` (private by default) rather than `struct`
        };

        // Shared state of the four rules: token view, line table and the
        // helpers that read a class body.
        class ApiAnalysis
        {
          public:
            explicit ApiAnalysis(const SemanticModel& model) :
                m_model(model), m_view(model), m_symbols(model.Symbols()), m_scopes(model.Scopes()),
                m_tree(model.Tree())
            {
            }

            const SemanticModel& Model() const { return m_model; }

            const detail::TokenView& View() const { return m_view; }

            const SymbolTable& Symbols() const { return m_symbols; }

            const ScopeTable& Scopes() const { return m_scopes; }

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
                const auto name         = m_view.PositionOf(m_symbols.decl_token[klass]);
                ClassBody  body;
                bool       found_keyword = false;
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

                for (body.open = name; body.open < end && m_view.At(body.open) != Tok::LBrace;
                     ++body.open)
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
            Access AccessAt(const ClassBody& body, std::size_t position) const
            {
                Access access = body.is_class ? Access::Private : Access::Public;
                for (auto i = body.open + 1; i < position && i < body.close; ++i)
                {
                    const Tok tok = m_view.At(i);
                    if (tok == Tok::LBrace)
                    {
                        i = m_view.Match(i, m_view.Size());
                    }
                    else if (m_view.At(i + 1) == Tok::Colon &&
                             (tok == Tok::KwPublic || tok == Tok::KwProtected ||
                              tok == Tok::KwPrivate))
                    {
                        access = tok == Tok::KwPublic      ? Access::Public
                                 : tok == Tok::KwProtected ? Access::Protected
                                                           : Access::Private;
                    }
                }

                return access;
            }

            // First token of the member declaration whose name is at `name`, after
            // any `template<...>` header: where a specifier or a using-declaration
            // can be inserted. `has_template` tells a template header was skipped.
            std::size_t DeclarationStart(const ClassBody& body, std::size_t name,
                                         bool& has_template) const
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
                        start        = close + 1;
                        has_template = true;
                    }
                }

                return start;
            }

            Diagnostic Make(
                RuleId rule, std::string_view code, std::string message, std::size_t position)
            {
                if (!m_lines_built)
                {
                    m_lines.Build(m_tree.Source());
                    m_lines_built = true;
                }

                const auto offset           = m_view.Offset(position);
                const auto position_in_file = m_lines.Lookup(offset);
                return Diagnostic {
                    rule,
                    Severity::Warning,
                    std::string(code),
                    std::move(message),
                    offset,
                    m_view.End(position) - offset,
                    position_in_file.line,
                    position_in_file.column,
                    false,
                    TextEdit {}
                };
            }

            // The fix rests on what the model could see: a quick fix, never applied in batch.
            static void AddQuickFix(Diagnostic& diagnostic,
                                    std::size_t offset,
                                    std::string replacement,
                                    std::string title)
            {
                diagnostic.has_fix     = true;
                diagnostic.fix         = TextEdit { offset, 0, std::move(replacement) };
                diagnostic.fix_is_safe = false;
                diagnostic.fix_title   = std::move(title);
            }

          private:
            const SemanticModel& m_model;
            detail::TokenView    m_view;
            const SymbolTable&   m_symbols;
            const ScopeTable&    m_scopes;
            const ParseTree&     m_tree;
            LineTable            m_lines;
            bool                 m_lines_built = false;
        };

        bool IsVirtualish(const SymbolTable& symbols, SymbolId function)
        {
            return (symbols.flags[function] & kVirtualish) != 0;
        }

        // ---------------------------------------------------------------------
        // api/virtual-destructor

        class VirtualDestructor
        {
          public:
            explicit VirtualDestructor(ApiAnalysis& analysis) :
                m_analysis(analysis), m_destructor(analysis.Symbols().Size(), kNone),
                m_introduces_virtual(analysis.Symbols().Size(), 0),
                m_declares_virtual(analysis.Symbols().Size(), 0)
            {
                const auto& symbols = analysis.Symbols();
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
                    else if ((flags & kVirtualish) != 0 && (flags & SymbolFlag::Constructor) == 0)
                    {
                        m_introduces_virtual[klass] = 1;
                        if ((flags & (SymbolFlag::Virtual | SymbolFlag::Pure)) != 0)
                        {
                            m_declares_virtual[klass] = 1;
                        }
                    }
                }
            }

            std::vector<Diagnostic> Run()
            {
                std::vector<Diagnostic> diagnostics;
                const auto&             symbols = m_analysis.Symbols();
                const auto&             model   = m_analysis.Model();
                for (SymbolId klass = 0; klass < symbols.Size(); ++klass)
                {
                    if (symbols.kind[klass] != SymbolKind::Class ||
                        m_introduces_virtual[klass] == 0)
                    {
                        continue;
                    }

                    // `override`/`final` with no bases and no written `virtual`
                    // proves nothing: the override is ill-formed without a base.
                    if (m_declares_virtual[klass] == 0 && symbols.base_count[klass] == 0)
                    {
                        continue;
                    }

                    const auto body = m_analysis.BodyOf(klass);
                    if (!body || detail::ClassHeadIsFinal(model.Tree(), symbols.decl_token[klass]))
                    {
                        continue;
                    }

                    const auto  destructor    = m_destructor[klass];
                    const auto& view          = m_analysis.View();
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

                    const auto        name = view.PositionOf(symbols.decl_token[klass]);
                    const std::string class_name(model.Names().Text(symbols.name[klass]));
                    auto              diagnostic = m_analysis.Make(
                        RuleId::ApiVirtualDestructor, "api/virtual-destructor",
                        "class '" + class_name +
                            "' has virtual functions but its destructor is not virtual; "
                            "deleting a derived object through a '" +
                            class_name + "' pointer is undefined behavior",
                        name);
                    if (destructor != kNone && name_position > 0 &&
                        view.At(name_position - 1) == Tok::Tilde)
                    {
                        ApiAnalysis::AddQuickFix(
                            diagnostic, view.Offset(name_position - 1), "virtual ",
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
                const auto& symbols = m_analysis.Symbols();
                if (depth > kMaxDepth)
                {
                    return Tri::Unknown;
                }

                const auto destructor = m_destructor[klass];
                if (destructor != kNone && IsVirtualish(symbols, destructor))
                {
                    return Tri::Yes;
                }

                const auto& bases  = m_analysis.Model().Bases();
                Tri         result = Tri::No;
                for (std::uint32_t i = 0; i < symbols.base_count[klass]; ++i)
                {
                    const auto target = bases.target[symbols.first_base[klass] + i];
                    const Tri  base =
                        target == kNone ? Tri::Unknown : HasVirtualDestructor(target, depth + 1);
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

            ApiAnalysis&              m_analysis;
            std::vector<SymbolId>     m_destructor;
            std::vector<std::uint8_t> m_introduces_virtual;
            std::vector<std::uint8_t> m_declares_virtual;
        };

        // ---------------------------------------------------------------------
        // api/explicit-constructor

        // The parameters of the list that opens at `open`: [begin, end) positions
        // of each one. False when the list does not close.
        bool SplitParameters(const detail::TokenView& view, std::size_t open,
                             std::vector<std::pair<std::size_t, std::size_t>>& parameters)
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
                    i                = angle < close ? angle : i;
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

        bool HasToken(const detail::TokenView& view, std::pair<std::size_t, std::size_t> range,
                      Tok tok)
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
        bool IsCopyMoveOrListParameter(const detail::TokenView&            view,
                                       std::pair<std::size_t, std::size_t> range,
                                       std::string_view                    class_name)
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

        std::vector<Diagnostic> ExplicitConstructor(ApiAnalysis& analysis)
        {
            std::vector<Diagnostic>                          diagnostics;
            const auto&                                      symbols = analysis.Symbols();
            const auto&                                      view    = analysis.View();
            const auto&                                      model   = analysis.Model();
            std::vector<std::pair<std::size_t, std::size_t>> parameters;
            for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
            {
                constexpr std::uint32_t skipped =
                    SymbolFlag::Friend | SymbolFlag::Defaulted | SymbolFlag::Qualified;
                if (symbols.kind[symbol] != SymbolKind::Function ||
                    (symbols.flags[symbol] & SymbolFlag::Constructor) == 0 ||
                    (symbols.flags[symbol] & skipped) != 0)
                {
                    continue;
                }

                const auto klass = analysis.OwnerClass(symbols.scope[symbol]);
                const auto body  = klass == kNone ? std::nullopt : analysis.BodyOf(klass);
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

                const auto&            first             = parameters.front();
                const std::string_view class_name        = model.Names().Text(symbols.name[klass]);
                bool                   callable_with_one = true;
                for (std::size_t i = 1; i < parameters.size(); ++i)
                {
                    callable_with_one = callable_with_one && HasToken(view, parameters[i], Tok::Eq);
                }

                const bool plain_void =
                    first.second == first.first + 1 && view.At(first.first) == Tok::KwVoid;
                if (!callable_with_one || plain_void || HasToken(view, first, Tok::Ellipsis) ||
                    IsCopyMoveOrListParameter(view, first, class_name))
                {
                    continue;
                }

                bool       has_template     = false;
                const auto start            = analysis.DeclarationStart(*body, name, has_template);
                bool       already_explicit = false;
                for (auto i = start; i < name; ++i)
                {
                    already_explicit = already_explicit || view.At(i) == Tok::KwExplicit;
                }

                if (already_explicit)
                {
                    continue;
                }

                auto diagnostic = analysis.Make(
                    RuleId::ApiExplicitConstructor, "api/explicit-constructor",
                    "constructor of '" + std::string(class_name) +
                        "' callable with one argument is not 'explicit'; "
                        "it allows accidental implicit conversions",
                    name);
                ApiAnalysis::AddQuickFix(
                    diagnostic, view.Offset(start), "explicit ", "Add 'explicit'");
                diagnostics.push_back(std::move(diagnostic));
            }

            return diagnostics;
        }

        // ---------------------------------------------------------------------
        // api/overload-hiding

        // The class body names `using ...::name;`.
        bool HasUsingFor(const detail::TokenView& view, const ClassBody& body,
                         std::string_view name)
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

                if (end > i + 2 && view.Text(end - 1) == name &&
                    view.At(end - 2) == Tok::ColonColon)
                {
                    return true;
                }

                i = end;
            }

            return false;
        }

        std::vector<Diagnostic> OverloadHiding(ApiAnalysis& analysis)
        {
            std::vector<Diagnostic>           diagnostics;
            const auto&                       symbols = analysis.Symbols();
            const auto&                       view    = analysis.View();
            const auto&                       model   = analysis.Model();
            const auto&                       bases   = model.Bases();
            std::unordered_set<std::uint64_t> seen;
            std::vector<SymbolId>             pending;
            std::unordered_set<SymbolId>      visited;
            for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
            {
                constexpr std::uint32_t skipped =
                    kNotMember | SymbolFlag::Constructor | SymbolFlag::Destructor |
                    SymbolFlag::Operator | SymbolFlag::Template;
                if (symbols.kind[symbol] != SymbolKind::Function ||
                    (symbols.flags[symbol] & skipped) != 0 || symbols.name[symbol] == kNone)
                {
                    continue;
                }

                const auto klass = analysis.OwnerClass(symbols.scope[symbol]);
                if (klass == kNone || symbols.base_count[klass] == 0 ||
                    !seen.insert(std::uint64_t { klass } << 32 | symbols.name[symbol]).second)
                {
                    continue;
                }

                const auto body      = analysis.BodyOf(klass);
                const auto name_text = model.Names().Text(symbols.name[symbol]);
                if (!body || HasUsingFor(view, *body, name_text))
                {
                    continue;
                }

                // The derived overloads; one with an unknown signature makes the
                // comparison meaningless.
                std::vector<std::uint64_t> derived_signatures;
                bool                       known = true;
                for (auto member =
                         model.LookupLocal(symbols.member_scope[klass], symbols.name[symbol]);
                     member != kNone;
                     member = symbols.next_same_name[member])
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
                const auto push_bases = [&](SymbolId derived) {
                    for (std::uint32_t i = 0; i < symbols.base_count[derived]; ++i)
                    {
                        if (const auto target = bases.target[symbols.first_base[derived] + i];
                            target != kNone)
                        {
                            pending.push_back(target);
                        }
                    }
                };

                push_bases(klass);
                while (!pending.empty() && hidden_in == kNone &&
                       visited.size() < kMaxDepth * kMaxDepth)
                {
                    const auto base = pending.back();
                    pending.pop_back();
                    if (!visited.insert(base).second)
                    {
                        continue;
                    }

                    for (auto member =
                             model.LookupLocal(symbols.member_scope[base], symbols.name[symbol]);
                         member != kNone;
                         member = symbols.next_same_name[member])
                    {
                        const bool candidate =
                            symbols.kind[member] == SymbolKind::Function &&
                            IsVirtualish(symbols, member) &&
                            (symbols.flags[member] & kNotMember) == 0 &&
                            symbols.signature[member] != 0;
                        if (candidate &&
                            std::find(derived_signatures.begin(), derived_signatures.end(),
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

                const auto        name = view.PositionOf(symbols.decl_token[symbol]);
                const std::string base_name(model.Names().Text(symbols.name[hidden_in]));
                auto              diagnostic = analysis.Make(
                    RuleId::ApiOverloadHiding, "api/overload-hiding",
                    "'" + std::string(name_text) + "' hides the virtual overloads of '" +
                        base_name + "'; add 'using " + base_name + "::" + std::string(name_text) +
                        ";'",
                    name);
                bool       has_template = false;
                const auto start        = analysis.DeclarationStart(*body, name, has_template);
                if (!has_template)
                {
                    ApiAnalysis::AddQuickFix(
                        diagnostic, view.Offset(start),
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
            explicit VirtualCall(ApiAnalysis& analysis) : m_analysis(analysis) {}

            std::vector<Diagnostic> Run()
            {
                std::vector<Diagnostic> diagnostics;
                const auto&             symbols = m_analysis.Symbols();
                const auto&             model   = m_analysis.Model();
                const auto&             view    = m_analysis.View();
                for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
                {
                    constexpr std::uint32_t special =
                        SymbolFlag::Constructor | SymbolFlag::Destructor;
                    if (symbols.kind[symbol] != SymbolKind::Function ||
                        (symbols.flags[symbol] & special) == 0 ||
                        (symbols.flags[symbol] & SymbolFlag::Definition) == 0)
                    {
                        continue;
                    }

                    const auto klass = m_analysis.OwnerClass(symbols.scope[symbol]);
                    if (klass == kNone ||
                        detail::ClassHeadIsFinal(model.Tree(), symbols.decl_token[klass]))
                    {
                        continue;
                    }

                    const auto function_node = symbols.decl_node[symbol];
                    const auto body          = BodyOfFunction(function_node);
                    if (!body)
                    {
                        continue;
                    }

                    const bool constructor = (symbols.flags[symbol] & SymbolFlag::Constructor) != 0;
                    const auto report_range = [&](std::size_t first, std::size_t last) {
                        for (auto q = first; q < last; ++q)
                        {
                            const auto callee = VirtualCallee(function_node, q);
                            if (callee == kNone || InLambda(q))
                            {
                                continue;
                            }

                            diagnostics.push_back(m_analysis.Make(
                                RuleId::ApiVirtualCallInConstructor,
                                "api/virtual-call-in-constructor",
                                "virtual function '" + std::string(view.Text(q)) +
                                    "' called from a " +
                                    (constructor ? "constructor" : "destructor") + " of '" +
                                    std::string(model.Names().Text(symbols.name[klass])) +
                                    "' does not dispatch to derived classes",
                                q));
                        }
                    };

                    // The body, plus the constructor's member-initializer list
                    // (`A() : m_(init()) {}`): initializers run while the object
                    // is being built too. Default arguments live before the
                    // `:` and are evaluated by the caller, so they stay out.
                    report_range(body->first + 1, body->second);
                    if (const auto init = MemInitRange(function_node, *body))
                    {
                        report_range(init->first, init->second);
                    }
                }

                return diagnostics;
            }

          private:
            std::optional<std::pair<std::size_t, std::size_t>> BodyOfFunction(
                std::uint32_t node) const
            {
                for (const auto child : m_analysis.Model().ChildrenOf(node))
                {
                    if (m_analysis.Model().Tree().NodesSoA().Kind(child) ==
                        GrammarKind::CompoundStatement)
                    {
                        return m_analysis.View().Range(child);
                    }
                }

                return std::nullopt;
            }

            // Tokens of the member-initializer list (`: m_(...), ...`), or
            // nothing when there is none. Anything before the `:` (return type,
            // parameter list, default arguments) belongs to the caller.
            std::optional<std::pair<std::size_t, std::size_t>> MemInitRange(
                std::uint32_t node, std::pair<std::size_t, std::size_t> body) const
            {
                const auto& view        = m_analysis.View();
                const auto [begin, end] = view.Range(node);
                const std::size_t stop  = std::min(body.first, end);
                std::size_t       depth = 0;
                for (std::size_t i = begin; i < stop; ++i)
                {
                    const Tok tok = view.At(i);
                    if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace ||
                        tok == Tok::Lt)
                    {
                        ++depth;
                    }
                    else if (tok == Tok::RParen || tok == Tok::RBracket || tok == Tok::RBrace ||
                             tok == Tok::Gt)
                    {
                        depth = depth == 0 ? 0 : depth - 1;
                    }
                    else if (tok == Tok::Colon && depth == 0)
                    {
                        return std::pair<std::size_t, std::size_t> { i + 1, stop };
                    }
                }

                return std::nullopt;
            }

            // The virtual member function that the unqualified call at `q` names,
            // or kNone when `q` is not such a call.
            SymbolId VirtualCallee(std::uint32_t function_node, std::size_t q) const
            {
                const auto& view    = m_analysis.View();
                const auto& model   = m_analysis.Model();
                const auto& symbols = m_analysis.Symbols();
                if (!view.IsWord(q) || view.At(q + 1) != Tok::LParen)
                {
                    return kNone;
                }

                const Tok  before = q > 0 ? view.At(q - 1) : Tok::None;
                const bool via_this =
                    before == Tok::Arrow && q > 1 && view.At(q - 2) == Tok::KwThis;
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

                const auto found =
                    model.Lookup(model.ScopeOfNode(function_node), name, view.TokenAt(q));
                if (found == kNone || symbols.kind[found] != SymbolKind::Function ||
                    m_analysis.OwnerClass(symbols.scope[found]) == kNone)
                {
                    return kNone;
                }

                for (auto member = found; member != kNone; member = symbols.next_same_name[member])
                {
                    if (symbols.kind[member] == SymbolKind::Function &&
                        IsVirtualish(symbols, member) &&
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
                    const auto& nodes = m_analysis.Model().Tree().NodesSoA();
                    for (std::uint32_t node = 0; node < nodes.size(); ++node)
                    {
                        if (nodes.Kind(node) == GrammarKind::LambdaExpression)
                        {
                            m_lambdas.push_back(m_analysis.View().Range(node));
                        }
                    }

                    m_lambdas_built = true;
                }

                return std::any_of(m_lambdas.begin(), m_lambdas.end(),
                                   [position](const std::pair<std::size_t, std::size_t>& range) {
                                       return position >= range.first && position < range.second;
                                   });
            }

            ApiAnalysis&                                     m_analysis;
            std::vector<std::pair<std::size_t, std::size_t>> m_lambdas;
            bool                                             m_lambdas_built = false;
        };

    } // namespace

    namespace
    {

        // ---------------------------------------------------------------------
        // Shared helpers for the new api/* rules below.

        // Views (`string_view`, `span`) are cheap to copy: passing them by value
        // is the idiom, not a pessimization.
        bool IsCheapViewHead(std::string_view head)
        {
            return head == "std::string_view" || head == "std::wstring_view" ||
                   head == "std::basic_string_view" || head == "std::span";
        }

        void FindClassesByName(const SemanticModel& model, std::string_view name,
                               std::vector<SymbolId>& out)
        {
            const auto& symbols = model.Symbols();
            for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
            {
                if (symbols.kind[symbol] == SymbolKind::Class && symbols.name[symbol] != kNone &&
                    model.Names().Text(symbols.name[symbol]) == name)
                {
                    out.push_back(symbol);
                }
            }
        }

        // True when `base` is a direct or indirect base of `derived`.
        // Unknown bases (kNone, template-ids) end the walk: the rules only act
        // on what the model knows for certain.
        bool IsBaseOf(const SemanticModel& model, SymbolId base, SymbolId derived)
        {
            if (base == kNone || derived == kNone || base == derived)
            {
                return false;
            }

            const auto&           symbols = model.Symbols();
            const auto&           bases   = model.Bases();
            std::vector<SymbolId> pending;
            std::vector<SymbolId> visited;
            pending.push_back(derived);
            visited.push_back(derived);
            while (!pending.empty() && visited.size() < kMaxDepth * 4)
            {
                const auto current = pending.back();
                pending.pop_back();
                for (std::uint32_t i = 0; i < symbols.base_count[current]; ++i)
                {
                    const auto target = bases.target[symbols.first_base[current] + i];
                    if (target == kNone)
                    {
                        continue;
                    }

                    if (target == base)
                    {
                        return true;
                    }

                    if (std::find(visited.begin(), visited.end(), target) == visited.end())
                    {
                        visited.push_back(target);
                        pending.push_back(target);
                    }
                }
            }

            return false;
        }

        // Range of a function's member-initializer list (`: m_(...), ...`), or
        // nothing. Anything before the `:` (return type, parameter list and its
        // default arguments) is evaluated by the caller and stays out.
        std::optional<std::pair<std::size_t, std::size_t>> MemInitRangeOf(
            const detail::TokenView& view, std::pair<std::size_t, std::size_t> function,
            std::pair<std::size_t, std::size_t> body)
        {
            const std::size_t stop  = std::min(body.first, function.second);
            std::size_t       depth = 0;
            for (std::size_t i = function.first; i < stop; ++i)
            {
                const Tok tok = view.At(i);
                if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace ||
                    tok == Tok::Lt)
                {
                    ++depth;
                }
                else if (tok == Tok::RParen || tok == Tok::RBracket || tok == Tok::RBrace ||
                         tok == Tok::Gt)
                {
                    depth = depth == 0 ? 0 : depth - 1;
                }
                else if (tok == Tok::Colon && depth == 0)
                {
                    return std::pair<std::size_t, std::size_t> { i + 1, stop };
                }
            }

            return std::nullopt;
        }

        // Range of the function body (`{...}`) of the definition `node`.
        std::optional<std::pair<std::size_t, std::size_t>> FunctionBodyOf(
            const SemanticModel& model, const detail::TokenView& view, std::uint32_t node)
        {
            for (const auto child : model.ChildrenOf(node))
            {
                if (model.Tree().NodesSoA().Kind(child) == GrammarKind::CompoundStatement)
                {
                    return view.Range(child);
                }
            }

            return std::nullopt;
        }

        // `std::move(name)` (or a bare `move(name)`) in [first, last): the
        // by-value parameter is intentionally sunk, not pointlessly copied.
        bool IsMovedFrom(const detail::TokenView& view,
                         std::string_view         name,
                         std::size_t              first,
                         std::size_t              last)
        {
            for (auto i = first; i + 3 < last; ++i)
            {
                const bool std_move = view.Text(i) == "std" && view.At(i + 1) == Tok::ColonColon &&
                                      view.Text(i + 2) == "move" && view.At(i + 3) == Tok::LParen &&
                                      view.Text(i + 4) == name && view.At(i + 5) == Tok::RParen;
                const bool bare_move = view.Text(i) == "move" && view.At(i + 1) == Tok::LParen &&
                                       view.Text(i + 2) == name && view.At(i + 3) == Tok::RParen;
                if (std_move || bare_move)
                {
                    return true;
                }
            }

            return false;
        }

        bool IsQueryName(std::string_view name)
        {
            static constexpr std::string_view kPrefixes[] = {
                "get",      "is",     "has",   "have", "can",    "could",  "should",
                "would",    "may",    "empty", "size", "count",  "length", "find",
                "contains", "front",  "back",  "top",  "data",   "at",     "value",
                "make",     "create", "clone", "copy", "exists", "equal",  "compare",
                "starts",   "ends",   "first", "last", "peek"
            };
            std::string lower(name);
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (const auto prefix : kPrefixes)
            {
                if (lower.size() >= prefix.size() && lower.compare(0, prefix.size(), prefix) == 0 &&
                    (lower.size() == prefix.size() ||
                     !(lower[prefix.size()] >= 'a' && lower[prefix.size()] <= 'z')))
                {
                    return true;
                }
            }

            return false;
        }

        // Offset where `[[nodiscard]]` can be inserted for the function whose
        // name is at `name_position`: the front of the declaration when the
        // prefix back from the name is only type material, otherwise right
        // before the name. Mirrors cpp/modernize-attributes.
        std::size_t NodiscardInsert(const detail::TokenView& view, std::size_t name_position,
                                    bool& front)
        {
            std::size_t insert = name_position;
            std::size_t cursor = name_position;
            front              = true;
            while (cursor > 0)
            {
                --cursor;
                const auto text = view.Text(cursor);
                const bool accept =
                    view.IsWord(cursor) || text == "::" || text == "*" || text == "&" ||
                    text == "<" || text == ">" || text == "const" || text == "constexpr" ||
                    text == "static" || text == "inline" || text == "virtual" ||
                    text == "explicit" || text == "friend" || text == "noexcept" ||
                    text == "unsigned" || text == "signed" || text == "int" || text == "char" ||
                    text == "short" || text == "long" || text == "float" || text == "double" ||
                    text == "bool" || text == "void" || text == "wchar_t" || text == "size_t" ||
                    text == "auto";
                if (!accept)
                {
                    front = text == ";" || text == "{" || text == "}" || text == ":" ||
                            text == "public" || text == "private" || text == "protected";
                    break;
                }

                insert = cursor;
            }

            return view.Offset(insert);
        }

        // ---------------------------------------------------------------------
        // api/missing-nodiscard (declaration side; TypeModel for return types)

        std::vector<Diagnostic> MissingNodiscard(const TypeModel& types)
        {
            const auto&             model   = types.Model();
            const auto&             symbols = model.Symbols();
            const detail::TokenView view(model);
            const std::string_view  source = model.Tree().Source();
            std::vector<Diagnostic> diagnostics;
            ApiAnalysis             analysis(model);
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
                // Query-like names are cpp/modernize-attributes': keep the two
                // rules disjoint so one declaration never gets two diagnostics.
                if (name == "main" || IsQueryName(name))
                {
                    continue;
                }

                // Resource-like returns only: pointers, classes, library types
                // and enums. Plain arithmetic returns (`int compute()`) stay
                // silent: whether discarding them is a bug needs intent the
                // engine cannot see.
                const TypeId returned = types.SymbolType(symbol);
                const auto   stripped = types.Types().Strip(returned);
                const auto   kind     = types.Types().Kind(stripped);
                const bool   resource = kind == TypeKind::Pointer || kind == TypeKind::Class ||
                                        kind == TypeKind::External || kind == TypeKind::Enum;
                if (!resource)
                {
                    continue;
                }

                const auto&       name_token = model.Tree().Tokens()[symbols.decl_token[symbol]];
                const std::size_t window_begin =
                    name_token.offset > 300 ? name_token.offset - 300 : 0;
                if (source.substr(window_begin, name_token.offset - window_begin)
                        .find("nodiscard") != std::string_view::npos)
                {
                    continue;
                }

                const std::size_t name_position = view.PositionOf(symbols.decl_token[symbol]);
                bool              front         = true;
                const std::size_t fix_offset    = NodiscardInsert(view, name_position, front);
                (void) front;
                auto diagnostic = analysis.Make(
                    RuleId::ApiMissingNodiscard, "api/missing-nodiscard",
                    "'" + name + "' returns '" + types.Spell(stripped) +
                        "' that should not be discarded; add [[nodiscard]]",
                    name_position);
                diagnostic.fix         = TextEdit { fix_offset, 0, "[[nodiscard]] " };
                diagnostic.has_fix     = true;
                diagnostic.fix_is_safe = false;
                diagnostic.fix_title   = "Add [[nodiscard]] to " + name;
                diagnostics.push_back(std::move(diagnostic));
            }

            std::stable_sort(diagnostics.begin(), diagnostics.end(),
                             [](const Diagnostic& left, const Diagnostic& right) {
                                 return left.offset < right.offset;
                             });
            return diagnostics;
        }

        // The named function whose definition contains `node` (walking up from a
        // parameter declaration through the function suffix), or kNone for
        // lambdas and unowned nodes. Function scopes carry no owner in the
        // Binder, so the definition node is mapped back through decl_node.
        SymbolId EnclosingFunction(const SemanticModel&                               model,
                                   const std::unordered_map<std::uint32_t, SymbolId>& function_of,
                                   std::uint32_t                                      node)
        {
            const auto&   nodes   = model.Tree().NodesSoA();
            std::uint32_t current = node;
            for (std::size_t steps = 0; steps <= nodes.size(); ++steps)
            {
                const auto kind = nodes.Kind(current);
                if (kind == GrammarKind::FunctionDefinition)
                {
                    const auto found = function_of.find(current);
                    return found == function_of.end() ? kNone : found->second;
                }

                if (kind == GrammarKind::LambdaExpression)
                {
                    return kNone;
                }

                const auto parent = nodes.Parent(current);
                if (parent >= nodes.size() || parent == current)
                {
                    return kNone;
                }

                current = parent;
            }

            return kNone;
        }

        std::unordered_map<std::uint32_t, SymbolId> FunctionMap(const SymbolTable& symbols)
        {
            std::unordered_map<std::uint32_t, SymbolId> map;
            for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
            {
                if (symbols.kind[symbol] == SymbolKind::Function)
                {
                    map[symbols.decl_node[symbol]] = symbol;
                }
            }

            return map;
        }

        bool IsSignatureSensitive(SymbolId owner, const SymbolTable& symbols)
        {
            return owner != kNone &&
                   (symbols.flags[owner] &
                    (SymbolFlag::Template | SymbolFlag::Virtual | SymbolFlag::Override |
                     SymbolFlag::Final | SymbolFlag::Operator)) != 0;
        }

        // ---------------------------------------------------------------------
        // api/pass-by-value: `const T&` that is copied (sink idiom)

        std::vector<Diagnostic> PassByValue(const TypeModel& types)
        {
            const auto& model   = types.Model();
            const auto& symbols = model.Symbols();
            const auto& scopes  = model.Scopes();
            (void) scopes;
            const detail::TokenView view(model);
            std::vector<Diagnostic> diagnostics;
            ApiAnalysis             analysis(model);
            const auto              function_of = FunctionMap(symbols);
            for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
            {
                if (symbols.kind[symbol] != SymbolKind::Parameter || symbols.name[symbol] == kNone)
                {
                    continue;
                }

                const TypeId param_type = types.SymbolType(symbol);
                if (types.Types().Kind(param_type) != TypeKind::LRef)
                {
                    continue;
                }

                const TypeId inner = types.Types().Arg(param_type);
                if (!types.Types().IsConstQualified(types.Types().Value(param_type)))
                {
                    continue;
                }

                const TypeId value      = types.Types().Strip(inner);
                const auto   value_kind = types.Types().Kind(value);
                if (value_kind != TypeKind::Class && value_kind != TypeKind::External)
                {
                    continue;
                }

                if (value_kind == TypeKind::External && IsCheapViewHead(types.ExternalHead(value)))
                {
                    continue;
                }

                const SymbolId owner =
                    EnclosingFunction(model, function_of, symbols.decl_node[symbol]);
                if (owner == kNone || IsSignatureSensitive(owner, symbols) ||
                    (symbols.flags[owner] & SymbolFlag::Definition) == 0)
                {
                    continue;
                }

                const auto function_range = view.Range(symbols.decl_node[owner]);
                const auto body           = FunctionBodyOf(model, view, symbols.decl_node[owner]);
                if (!body)
                {
                    continue;
                }

                const auto        init = MemInitRangeOf(view, function_range, *body);
                const std::string name(model.Names().Text(symbols.name[symbol]));
                bool              copied = false;
                // `: m_(p)` in the member-initializer list.
                if (init)
                {
                    for (auto i = init->first; i + 2 < init->second && !copied; ++i)
                    {
                        copied = view.At(i) == Tok::LParen && view.Text(i + 1) == name &&
                                 view.At(i + 2) == Tok::RParen;
                    }
                }

                // `x = p;`, `return p;` with nothing else on the right side.
                for (auto i = body->first + 1; i + 1 < body->second && !copied; ++i)
                {
                    if (view.Text(i) != name)
                    {
                        continue;
                    }

                    const Tok  before = view.At(i - 1);
                    const Tok  after  = view.At(i + 1);
                    const bool assigned =
                        before == Tok::Eq &&
                        (after == Tok::Semi || after == Tok::Comma || after == Tok::RParen);
                    const bool returned =
                        before == Tok::KwReturn && (after == Tok::Semi || after == Tok::RParen);
                    copied = assigned || returned;
                }

                if (!copied)
                {
                    continue;
                }

                const std::size_t name_position = view.PositionOf(symbols.decl_token[symbol]);
                auto              diagnostic    = analysis.Make(
                    RuleId::ApiPassByValue, "api/pass-by-value",
                    "parameter '" + name +
                        "' is taken by const reference but copied; "
                        "take '" +
                        types.Spell(value) + "' by value and move it",
                    name_position);
                const auto        param_range = view.Range(symbols.decl_node[symbol]);
                const std::size_t fix_begin   = view.Offset(param_range.first);
                const std::size_t fix_end     = view.End(name_position);
                diagnostic.has_fix            = true;
                diagnostic.fix =
                    TextEdit { fix_begin, fix_end - fix_begin, types.Spell(value) + " " + name };
                diagnostic.fix_is_safe = false;
                diagnostic.fix_title   = "Take '" + name + "' by value";
                diagnostics.push_back(std::move(diagnostic));
            }

            std::stable_sort(diagnostics.begin(), diagnostics.end(),
                             [](const Diagnostic& left, const Diagnostic& right) {
                                 return left.offset < right.offset;
                             });
            return diagnostics;
        }

        // ---------------------------------------------------------------------
        // api/pass-by-const-reference: by-value but only read (needs FlowModel)

        std::vector<Diagnostic> PassByConstReference(const FlowModel& flow)
        {
            const auto&             types   = flow.Types();
            const auto&             model   = types.Model();
            const auto&             symbols = model.Symbols();
            const detail::TokenView view(model);
            std::vector<Diagnostic> diagnostics;
            ApiAnalysis             analysis(model);
            const auto              function_of = FunctionMap(symbols);
            for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
            {
                if (symbols.kind[symbol] != SymbolKind::Parameter || symbols.name[symbol] == kNone)
                {
                    continue;
                }

                const TypeId param_type = types.SymbolType(symbol);
                const TypeId value      = types.Types().Strip(param_type);
                const auto   value_kind = types.Types().Kind(value);
                if (types.Types().Kind(param_type) == TypeKind::LRef ||
                    types.Types().Kind(param_type) == TypeKind::RRef ||
                    types.Types().Kind(param_type) == TypeKind::Pointer ||
                    types.Types().Kind(param_type) == TypeKind::Array)
                {
                    continue;
                }

                if (value_kind != TypeKind::Class && value_kind != TypeKind::External)
                {
                    continue;
                }

                if (value_kind == TypeKind::External && IsCheapViewHead(types.ExternalHead(value)))
                {
                    continue;
                }

                const SymbolId owner =
                    EnclosingFunction(model, function_of, symbols.decl_node[symbol]);
                if (owner == kNone || IsSignatureSensitive(owner, symbols) ||
                    (symbols.flags[owner] & (SymbolFlag::Definition | SymbolFlag::Constructor |
                                             SymbolFlag::Destructor)) == 0)
                {
                    continue;
                }

                // Sink constructors (`: m_(std::move(p))`, `m_ = p`) own their
                // argument: that is api/pass-by-value's, not this rule's.
                if ((symbols.flags[owner] & (SymbolFlag::Constructor | SymbolFlag::Destructor)) !=
                    0)
                {
                    continue;
                }

                const FunctionId function = flow.OwnerOf(symbol);
                if (function == kNone || function >= flow.Functions().complete.size() ||
                    flow.Functions().complete[function] == 0)
                {
                    continue;
                }

                bool read_only = true;
                for (const auto index : flow.EventsOf(symbol))
                {
                    const auto kind = flow.Events().kind[index];
                    if (kind != EventKind::Read && kind != EventKind::Init)
                    {
                        read_only = false;
                        break;
                    }
                }

                if (!read_only)
                {
                    continue;
                }

                const auto body = FunctionBodyOf(model, view, symbols.decl_node[owner]);
                if (body && IsMovedFrom(view, model.Names().Text(symbols.name[symbol]), body->first,
                                        body->second))
                {
                    continue;
                }

                const std::string name(model.Names().Text(symbols.name[symbol]));
                const std::size_t name_position = view.PositionOf(symbols.decl_token[symbol]);
                auto              diagnostic    = analysis.Make(
                    RuleId::ApiPassByConstReference,
                    "api/pass-by-const-reference",
                    "parameter '" + name + "' of type '" + types.Spell(value) +
                        "' is copied by value but never modified; take 'const " +
                        types.Spell(value) + "&'",
                    name_position);
                const auto        param_range = view.Range(symbols.decl_node[symbol]);
                const std::size_t fix_begin   = view.Offset(param_range.first);
                const std::size_t fix_end     = view.End(name_position);
                diagnostic.has_fix            = true;
                diagnostic.fix         = TextEdit { fix_begin, fix_end - fix_begin,
                                                    "const " + types.Spell(value) + "& " + name };
                diagnostic.fix_is_safe = false;
                diagnostic.fix_title   = "Take '" + name + "' by const reference";
                diagnostics.push_back(std::move(diagnostic));
            }

            std::stable_sort(diagnostics.begin(), diagnostics.end(),
                             [](const Diagnostic& left, const Diagnostic& right) {
                                 return left.offset < right.offset;
                             });
            return diagnostics;
        }

        // Const member names the engine trusts on library types.
        bool IsTrustedConstCall(std::string_view name)
        {
            static constexpr std::string_view kConst[] = {
                "size",  "empty",  "length",  "capacity",    "max_size",  "count", "contains",
                "c_str", "substr", "compare", "starts_with", "ends_with", "at",    "data",
                "front", "back",   "begin",   "end",         "cbegin",    "cend",  "find"
            };
            return std::find(std::begin(kConst), std::end(kConst), name) != std::end(kConst);
        }

        bool IsAssignToken(Tok tok)
        {
            switch (tok)
            {
                case Tok::Eq:
                case Tok::PlusEq:
                case Tok::MinusEq:
                case Tok::StarEq:
                case Tok::SlashEq:
                case Tok::PercentEq:
                case Tok::AmpEq:
                case Tok::PipeEq:
                case Tok::CaretEq:
                case Tok::ShlEq:
                case Tok::ShrEq:
                    return true;
                default:
                    return false;
            }
        }

        // The member function `function` of `klass` never observably mutates:
        // no member write, no address of a member escaping, no call to a
        // non-const member, and no call the engine cannot resolve. Bare `this`
        // (other than `this->`) also ends the proof.
        bool MemberFunctionIsPureReader(const SemanticModel&     model,
                                        const detail::TokenView& view,
                                        SymbolId                 klass,
                                        SymbolId                 function,
                                        std::pair<std::size_t, std::size_t>
                                            body)
        {
            const auto& symbols = model.Symbols();
            for (auto i = body.first + 1; i < body.second; ++i)
            {
                if (!view.IsWord(i))
                {
                    // `&member` and `delete` need the author: give up loudly by
                    // staying silent (returning false).
                    if (view.At(i) == Tok::Amp && i + 1 < body.second && view.IsWord(i + 1) &&
                        model.ResolveToken(view.TokenAt(i + 1)) != kNone)
                    {
                        const auto target = model.ResolveToken(view.TokenAt(i + 1));
                        if (target != kNone && symbols.kind[target] == SymbolKind::Variable &&
                            symbols.scope[target] == symbols.member_scope[klass])
                        {
                            return false;
                        }
                    }

                    continue;
                }

                const std::string_view word   = view.Text(i);
                const SymbolId         target = model.ResolveToken(view.TokenAt(i));
                const Tok              after  = view.At(i + 1);
                const Tok              before = i > 0 ? view.At(i - 1) : Tok::None;

                if (word == "this")
                {
                    if (after != Tok::Arrow)
                    {
                        return false;
                    }

                    continue;
                }

                if (target == kNone)
                {
                    // A call the engine cannot see may be a member of an
                    // unresolved base; anything else (a plain read of an
                    // unknown name) is harmless.
                    if (after == Tok::LParen && before != Tok::Dot && before != Tok::Arrow &&
                        before != Tok::ColonColon)
                    {
                        return false;
                    }

                    continue;
                }

                // A write to a member of this class.
                if (symbols.kind[target] == SymbolKind::Variable &&
                    symbols.scope[target] == symbols.member_scope[klass])
                {
                    if (IsAssignToken(after) || after == Tok::PlusPlus || after == Tok::MinusMinus)
                    {
                        return false;
                    }

                    // `m_.push_back(...)`: a call through the member. Only
                    // calls the engine knows to be const are safe.
                    if ((after == Tok::Dot || after == Tok::Arrow) && i + 2 < body.second &&
                        view.At(i + 2) == Tok::LParen)
                    {
                        return false;
                    }

                    if (after == Tok::Dot || after == Tok::Arrow)
                    {
                        const std::string_view member_call = view.Text(i + 2);
                        const bool call = i + 3 < body.second && view.At(i + 3) == Tok::LParen;
                        if (call && !IsTrustedConstCall(member_call))
                        {
                            return false;
                        }
                    }

                    continue;
                }

                // A call to a member function of this class.
                if (symbols.kind[target] == SymbolKind::Function && after == Tok::LParen &&
                    before != Tok::Dot && before != Tok::Arrow && before != Tok::ColonColon)
                {
                    const ScopeId  target_scope = symbols.scope[target];
                    const auto&    scopes       = model.Scopes();
                    const SymbolId target_owner =
                        scopes.kind[target_scope] == ScopeKind::Class
                            ? scopes.owner[target_scope]
                            : kNone;
                    if (target_owner == klass || IsBaseOf(model, target_owner, klass) ||
                        IsBaseOf(model, klass, target_owner))
                    {
                        if ((symbols.flags[target] & SymbolFlag::Const) == 0 &&
                            (symbols.flags[target] & SymbolFlag::Static) == 0)
                        {
                            return false;
                        }
                    }

                    continue;
                }

                if (symbols.kind[target] == SymbolKind::Function && before == Tok::Arrow &&
                    i >= 2 && view.Text(i - 2) == "this" && after == Tok::LParen)
                {
                    if ((symbols.flags[target] & SymbolFlag::Const) == 0 &&
                        (symbols.flags[target] & SymbolFlag::Static) == 0)
                    {
                        return false;
                    }
                }
            }

            (void) function;
            return true;
        }

        // ---------------------------------------------------------------------
        // api/const-correctness (needs FlowModel)

        std::vector<Diagnostic> ConstCorrectness(const FlowModel& flow)
        {
            const auto&             types   = flow.Types();
            const auto&             model   = types.Model();
            const auto&             symbols = model.Symbols();
            const auto&             scopes  = model.Scopes();
            const detail::TokenView view(model);
            std::vector<Diagnostic> diagnostics;
            ApiAnalysis             analysis(model);
            const auto              function_of = FunctionMap(symbols);

            // `T&` parameters that nothing modifies.
            for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
            {
                if (symbols.kind[symbol] != SymbolKind::Parameter || symbols.name[symbol] == kNone)
                {
                    continue;
                }

                const TypeId param_type = types.SymbolType(symbol);
                if (types.Types().Kind(param_type) != TypeKind::LRef ||
                    types.Types().IsConstQualified(param_type))
                {
                    continue;
                }

                const TypeId value = types.Types().Strip(types.Types().Value(param_type));
                if (!types.Types().IsKnown(value))
                {
                    continue;
                }

                const SymbolId owner =
                    EnclosingFunction(model, function_of, symbols.decl_node[symbol]);
                if (owner == kNone || IsSignatureSensitive(owner, symbols) ||
                    (symbols.flags[owner] & SymbolFlag::Definition) == 0)
                {
                    continue;
                }

                const FunctionId function = flow.OwnerOf(symbol);
                if (function == kNone || function >= flow.Functions().complete.size() ||
                    flow.Functions().complete[function] == 0)
                {
                    continue;
                }

                bool read_only = true;
                bool seen      = false;
                for (const auto index : flow.EventsOf(symbol))
                {
                    seen            = true;
                    const auto kind = flow.Events().kind[index];
                    if (kind != EventKind::Read && kind != EventKind::Init)
                    {
                        read_only = false;
                        break;
                    }
                }

                if (!seen || !read_only)
                {
                    continue;
                }

                const std::string name(model.Names().Text(symbols.name[symbol]));
                const std::size_t name_position = view.PositionOf(symbols.decl_token[symbol]);
                auto              diagnostic    = analysis.Make(
                    RuleId::ApiConstCorrectness, "api/const-correctness",
                    "parameter '" + name + "' is never modified; make it 'const " +
                        types.Spell(value) + "&'",
                    name_position);
                const auto        param_range = view.Range(symbols.decl_node[symbol]);
                const std::size_t fix_begin   = view.Offset(param_range.first);
                const std::size_t fix_end     = view.End(name_position);
                diagnostic.has_fix            = true;
                diagnostic.fix         = TextEdit { fix_begin, fix_end - fix_begin,
                                                    "const " + types.Spell(value) + "& " + name };
                diagnostic.fix_is_safe = false;
                diagnostic.fix_title   = "Make '" + name + "' const";
                diagnostics.push_back(std::move(diagnostic));
            }

            // Member functions that could be `const`.
            for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
            {
                constexpr std::uint32_t kSkipped =
                    SymbolFlag::Static | SymbolFlag::Constructor | SymbolFlag::Destructor |
                    SymbolFlag::Operator | SymbolFlag::Template | SymbolFlag::Friend |
                    SymbolFlag::Qualified | SymbolFlag::Virtual | SymbolFlag::Override |
                    SymbolFlag::Final | SymbolFlag::Pure | SymbolFlag::Defaulted |
                    SymbolFlag::Const;
                if (symbols.kind[symbol] != SymbolKind::Function ||
                    (symbols.flags[symbol] & kSkipped) != 0 ||
                    (symbols.flags[symbol] & SymbolFlag::Definition) == 0 ||
                    symbols.name[symbol] == kNone)
                {
                    continue;
                }

                const ScopeId scope = symbols.scope[symbol];
                if (scopes.kind[scope] != ScopeKind::Class || scopes.owner[scope] == kNone)
                {
                    continue;
                }

                const SymbolId klass = scopes.owner[scope];
                const auto     body  = FunctionBodyOf(model, view, symbols.decl_node[symbol]);
                if (!body || !MemberFunctionIsPureReader(model, view, klass, symbol, *body))
                {
                    continue;
                }

                const std::size_t name_position = view.PositionOf(symbols.decl_token[symbol]);
                std::size_t       open          = name_position;
                while (open < view.Size() && view.At(open) != Tok::LParen)
                {
                    ++open;
                }

                const std::size_t close =
                    open < view.Size() ? view.Match(open, view.Size()) : view.Size();
                if (open >= view.Size() || close >= view.Size())
                {
                    continue;
                }

                const std::string name(model.Names().Text(symbols.name[symbol]));
                auto              diagnostic = analysis.Make(
                    RuleId::ApiConstCorrectness, "api/const-correctness",
                    "member function '" + name + "' does not modify the object; make it 'const'",
                    name_position);
                ApiAnalysis::AddQuickFix(
                    diagnostic, view.End(close), " const", "Make '" + name + "' const");
                diagnostics.push_back(std::move(diagnostic));
            }

            std::stable_sort(diagnostics.begin(), diagnostics.end(),
                             [](const Diagnostic& left, const Diagnostic& right) {
                                 return left.offset < right.offset;
                             });
            return diagnostics;
        }

        // ---------------------------------------------------------------------
        // api/unsafe-downcast (TypeModel for operand types)

        std::vector<Diagnostic> UnsafeDowncast(const TypeModel& types)
        {
            const auto&             model   = types.Model();
            const auto&             symbols = model.Symbols();
            const detail::TokenView view(model);
            std::vector<Diagnostic> diagnostics;
            ApiAnalysis             analysis(model);
            std::vector<SymbolId>   candidates;
            for (std::size_t i = 0; i < view.Size(); ++i)
            {
                if (view.At(i) != Tok::KwStaticCast || view.At(i + 1) != Tok::Lt)
                {
                    continue;
                }

                const auto angle = view.MatchAngle(i + 1, view.Size());
                if (angle >= view.Size() || view.At(angle + 1) != Tok::LParen)
                {
                    continue;
                }

                // The target must read `Name*` / `Name&` (`const` allowed).
                std::size_t anchor       = view.Size();
                bool        is_pointer   = false;
                bool        is_reference = false;
                for (auto k = i + 2; k < angle; ++k)
                {
                    if (view.At(k) == Tok::Star)
                    {
                        anchor     = k;
                        is_pointer = true;
                    }
                    else if (view.At(k) == Tok::Amp || view.At(k) == Tok::AmpAmp)
                    {
                        anchor       = k;
                        is_reference = true;
                    }
                }

                if (anchor >= angle || (is_pointer && is_reference))
                {
                    continue;
                }

                std::string target_name;
                for (auto k = anchor; k > i + 2;)
                {
                    --k;
                    if (view.IsWord(k))
                    {
                        target_name = std::string(view.Text(k));
                        break;
                    }

                    if (view.At(k) != Tok::ColonColon)
                    {
                        break;
                    }
                }

                if (target_name.empty())
                {
                    continue;
                }

                const auto close_paren = view.Match(angle + 1, view.Size());
                if (close_paren >= view.Size() || close_paren != angle + 3)
                {
                    // Only a single identifier operand is modeled.
                    continue;
                }

                if (!view.IsWord(angle + 2))
                {
                    continue;
                }

                const SymbolId source_symbol = model.ResolveToken(view.TokenAt(angle + 2));
                if (source_symbol == kNone ||
                    (symbols.kind[source_symbol] != SymbolKind::Variable &&
                     symbols.kind[source_symbol] != SymbolKind::Parameter))
                {
                    continue;
                }

                const TypeId source_type  = types.Types().Strip(types.SymbolType(source_symbol));
                const auto   source_kind  = types.Types().Kind(source_type);
                TypeId       source_class = TypeTable::Unknown;
                if (is_pointer && source_kind == TypeKind::Pointer)
                {
                    source_class = types.Types().Strip(types.Types().Arg(source_type));
                }
                else if (is_reference &&
                         (source_kind == TypeKind::LRef || source_kind == TypeKind::RRef))
                {
                    source_class = types.Types().Strip(types.Types().Arg(source_type));
                }

                if (types.Types().Kind(source_class) != TypeKind::Class)
                {
                    continue;
                }

                const SymbolId source_id = static_cast<SymbolId>(types.Types().Arg(source_class));
                candidates.clear();
                FindClassesByName(model, target_name, candidates);
                if (candidates.size() != 1)
                {
                    continue;
                }

                const SymbolId target_id = candidates.front();
                if (!IsBaseOf(model, source_id, target_id))
                {
                    continue;
                }

                const std::string source_name(model.Names().Text(symbols.name[source_id]));
                diagnostics.push_back(analysis.Make(
                    RuleId::ApiUnsafeDowncast, "api/unsafe-downcast",
                    "static_cast from base '" + source_name + "' to derived '" + target_name +
                        "' is unchecked; a wrong dynamic type is undefined behavior, consider "
                        "dynamic_cast",
                    i));
            }

            std::stable_sort(diagnostics.begin(), diagnostics.end(),
                             [](const Diagnostic& left, const Diagnostic& right) {
                                 return left.offset < right.offset;
                             });
            return diagnostics;
        }

        // The initializing expression of the variable `symbol`, or kNone.
        std::uint32_t InitializerOf(const SemanticModel& model, SymbolId symbol)
        {
            const auto& symbols = model.Symbols();
            const auto  node    = symbols.decl_node[symbol];
            for (const auto child : model.ChildrenOf(node))
            {
                if (model.Tree().NodesSoA().Kind(child) != GrammarKind::InitDeclarator)
                {
                    continue;
                }

                std::uint32_t declarator = kNone;
                for (const auto inner : model.ChildrenOf(child))
                {
                    if (model.Tree().NodesSoA().Kind(inner) == GrammarKind::Declarator)
                    {
                        declarator = inner;
                    }
                }

                if (declarator == kNone)
                {
                    continue;
                }

                bool ours = false;
                for (const auto inner : model.ChildrenOf(declarator))
                {
                    if (model.Tree().NodesSoA().Kind(inner) == GrammarKind::DeclaredName &&
                        model.Tree().NodesSoA().FirstToken(inner) == symbols.decl_token[symbol])
                    {
                        ours = true;
                    }
                }

                if (!ours)
                {
                    continue;
                }

                for (const auto inner : model.ChildrenOf(child))
                {
                    const auto kind = model.Tree().NodesSoA().Kind(inner);
                    if (kind != GrammarKind::Declarator && kind != GrammarKind::TypeSpecifier)
                    {
                        return inner;
                    }
                }
            }

            return kNone;
        }

        // ---------------------------------------------------------------------
        // api/slicing (TypeModel for value types)

        std::vector<Diagnostic> Slicing(const TypeModel& types)
        {
            const auto&             model   = types.Model();
            const auto&             symbols = model.Symbols();
            const auto&             nodes   = model.Tree().NodesSoA();
            const detail::TokenView view(model);
            std::vector<Diagnostic> diagnostics;
            ApiAnalysis             analysis(model);

            // `Base b = derived;`: the derived part is lost in the copy.
            for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
            {
                if (symbols.kind[symbol] != SymbolKind::Variable || symbols.name[symbol] == kNone)
                {
                    continue;
                }

                // By value only: references and pointers do not slice. A
                // top-level const still stores a copy.
                const TypeId raw_declared = types.SymbolType(symbol);
                const auto   raw_kind     = types.Types().Kind(raw_declared);
                if (raw_kind == TypeKind::LRef || raw_kind == TypeKind::RRef ||
                    raw_kind == TypeKind::Pointer || raw_kind == TypeKind::Array)
                {
                    continue;
                }

                TypeId declared_value = raw_declared;
                if (types.Types().Kind(declared_value) == TypeKind::Const)
                {
                    declared_value = types.Types().Arg(declared_value);
                }

                if (types.Types().Kind(declared_value) != TypeKind::Class)
                {
                    continue;
                }

                const std::uint32_t init = InitializerOf(model, symbol);
                if (init == kNone)
                {
                    continue;
                }

                const TypeId given = types.Types().Strip(types.NodeType(init));
                if (types.Types().Kind(given) != TypeKind::Class)
                {
                    continue;
                }

                const auto base_id    = static_cast<SymbolId>(types.Types().Arg(declared_value));
                const auto derived_id = static_cast<SymbolId>(types.Types().Arg(given));
                if (!IsBaseOf(model, base_id, derived_id))
                {
                    continue;
                }

                const std::size_t name_position = view.PositionOf(symbols.decl_token[symbol]);
                diagnostics.push_back(analysis.Make(
                    RuleId::ApiSlicing, "api/slicing",
                    "object of derived type '" +
                        std::string(model.Names().Text(symbols.name[derived_id])) +
                        "' is stored by value as its base '" +
                        std::string(model.Names().Text(symbols.name[base_id])) +
                        "'; the derived part is sliced away",
                    name_position));
            }

            // `b = derived;` where `b` is a base object.
            // `return derived;` from a function returning the base by value.
            std::unordered_map<std::uint32_t, SymbolId> function_of;
            for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
            {
                if (symbols.kind[symbol] == SymbolKind::Function)
                {
                    function_of[symbols.decl_node[symbol]] = symbol;
                }
            }

            for (std::uint32_t node = 1; node < nodes.size(); ++node)
            {
                if (nodes.Kind(node) == GrammarKind::BinaryExpression)
                {
                    const auto kids = model.ChildrenOf(node);
                    if (kids.size() != 2)
                    {
                        continue;
                    }

                    const auto [begin, end] = view.Range(node);
                    if (end != begin + 3 || view.At(begin + 1) != Tok::Eq)
                    {
                        continue;
                    }

                    const TypeId left  = types.Types().Strip(types.NodeType(kids[0]));
                    const TypeId right = types.Types().Strip(types.NodeType(kids[1]));
                    if (types.Types().Kind(left) != TypeKind::Class ||
                        types.Types().Kind(right) != TypeKind::Class)
                    {
                        continue;
                    }

                    const auto base_id    = static_cast<SymbolId>(types.Types().Arg(left));
                    const auto derived_id = static_cast<SymbolId>(types.Types().Arg(right));
                    if (!IsBaseOf(model, base_id, derived_id))
                    {
                        continue;
                    }

                    diagnostics.push_back(analysis.Make(
                        RuleId::ApiSlicing, "api/slicing",
                        "object of derived type '" +
                            std::string(model.Names().Text(symbols.name[derived_id])) +
                            "' is assigned by value to its base '" +
                            std::string(model.Names().Text(symbols.name[base_id])) +
                            "'; the derived part is sliced away",
                        begin + 1));
                }
                else if (nodes.Kind(node) == GrammarKind::ReturnStatement)
                {
                    const auto kids = model.ChildrenOf(node);
                    if (kids.empty())
                    {
                        continue;
                    }

                    const std::uint32_t value_node = kids.back();
                    const TypeId        given = types.Types().Strip(types.NodeType(value_node));
                    if (types.Types().Kind(given) != TypeKind::Class)
                    {
                        continue;
                    }

                    std::uint32_t owner = nodes.Parent(node);
                    for (std::size_t steps = 0;
                         steps < nodes.size() && owner < nodes.size() && owner != 0;
                         ++steps)
                    {
                        const auto kind = nodes.Kind(owner);
                        if (kind == GrammarKind::LambdaExpression ||
                            kind == GrammarKind::RecordDefinition)
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

                    const auto found = function_of.find(owner);
                    if (found == function_of.end())
                    {
                        continue;
                    }

                    const TypeId returned = types.SymbolType(found->second);
                    // By value only (a top-level const still stores a copy):
                    // references and pointers do not slice.
                    TypeId returned_value = types.Types().Value(returned);
                    if (types.Types().Kind(returned_value) == TypeKind::Const)
                    {
                        returned_value = types.Types().Arg(returned_value);
                    }

                    if (types.Types().Kind(returned_value) != TypeKind::Class)
                    {
                        // By reference or pointer: no slice.
                        continue;
                    }

                    const auto base_id = static_cast<SymbolId>(types.Types().Arg(returned_value));
                    const auto derived_id = static_cast<SymbolId>(types.Types().Arg(given));
                    if (!IsBaseOf(model, base_id, derived_id))
                    {
                        continue;
                    }

                    const auto [begin, end] = view.Range(value_node);
                    diagnostics.push_back(analysis.Make(
                        RuleId::ApiSlicing, "api/slicing",
                        "object of derived type '" +
                            std::string(model.Names().Text(symbols.name[derived_id])) +
                            "' is returned by value as its base '" +
                            std::string(model.Names().Text(symbols.name[base_id])) +
                            "'; the derived part is sliced away",
                        begin));
                }
            }

            std::stable_sort(diagnostics.begin(), diagnostics.end(),
                             [](const Diagnostic& left, const Diagnostic& right) {
                                 return left.offset < right.offset;
                             });
            return diagnostics;
        }

        bool IsOperatorPunctuation(std::string_view rest)
        {
            if (rest.empty())
            {
                return true;
            }

            const char first = rest.front();
            if ((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '_' ||
                first == ':')
            {
                static constexpr std::string_view kExcluded[] = {
                    "new",   "delete", "and",    "and_eq", "bitand", "bitor", "or",
                    "or_eq", "xor",    "xor_eq", "not",    "not_eq", "compl"
                };
                return std::find(std::begin(kExcluded), std::end(kExcluded), rest) !=
                       std::end(kExcluded);
            }

            return true;
        }

        // ---------------------------------------------------------------------
        // api/implicit-conversion: non-explicit conversion operators

        std::vector<Diagnostic> ImplicitConversion(ApiAnalysis& analysis)
        {
            const auto&             model   = analysis.Model();
            const auto&             symbols = analysis.Symbols();
            const auto&             view    = analysis.View();
            std::vector<Diagnostic> diagnostics;
            for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
            {
                if (symbols.kind[symbol] != SymbolKind::Function ||
                    (symbols.flags[symbol] & SymbolFlag::Operator) == 0 ||
                    symbols.name[symbol] == kNone)
                {
                    continue;
                }

                if ((symbols.flags[symbol] &
                     (SymbolFlag::Friend | SymbolFlag::Qualified | SymbolFlag::Template)) != 0)
                {
                    continue;
                }

                const std::string          name(model.Names().Text(symbols.name[symbol]));
                constexpr std::string_view kPrefix = "operator";
                if (name.size() <= kPrefix.size() || name.compare(0, kPrefix.size(), kPrefix) != 0)
                {
                    continue;
                }

                std::string_view rest(name);
                rest.remove_prefix(kPrefix.size());
                while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t'))
                {
                    rest.remove_prefix(1);
                }

                // Overloaded operators (`operator+`, `operator()`, `operator new`)
                // are not conversions.
                if (IsOperatorPunctuation(rest))
                {
                    continue;
                }

                const SymbolId klass = analysis.OwnerClass(symbols.scope[symbol]);
                const auto     body  = klass == kNone ? std::nullopt : analysis.BodyOf(klass);
                if (!body)
                {
                    continue;
                }

                const auto position     = view.PositionOf(symbols.decl_token[symbol]);
                bool       has_template = false;
                const auto start        = analysis.DeclarationStart(*body, position, has_template);
                bool       already_explicit = false;
                for (auto i = start; i < position; ++i)
                {
                    already_explicit = already_explicit || view.At(i) == Tok::KwExplicit;
                }

                if (already_explicit)
                {
                    continue;
                }

                diagnostics.push_back(analysis.Make(
                    RuleId::ApiImplicitConversion, "api/implicit-conversion",
                    "conversion operator '" + name +
                        "' is implicit; it allows accidental conversions, "
                        "consider 'explicit'",
                    position));
            }

            std::stable_sort(diagnostics.begin(), diagnostics.end(),
                             [](const Diagnostic& left, const Diagnostic& right) {
                                 return left.offset < right.offset;
                             });
            return diagnostics;
        }

    } // namespace

    std::vector<Diagnostic> SemanticRules::AnalyzeVirtualDestructor(const SemanticModel& model)
    {
        ApiAnalysis analysis(model);
        return VirtualDestructor(analysis).Run();
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeExplicitConstructor(const SemanticModel& model)
    {
        ApiAnalysis analysis(model);
        return ExplicitConstructor(analysis);
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeOverloadHiding(const SemanticModel& model)
    {
        ApiAnalysis analysis(model);
        return OverloadHiding(analysis);
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeVirtualCallInConstructor(
        const SemanticModel& model)
    {
        ApiAnalysis analysis(model);
        return VirtualCall(analysis).Run();
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeMissingNodiscard(const TypeModel& types)
    {
        return MissingNodiscard(types);
    }

    std::vector<Diagnostic> SemanticRules::AnalyzePassByValue(const TypeModel& types)
    {
        return PassByValue(types);
    }

    std::vector<Diagnostic> SemanticRules::AnalyzePassByConstReference(const FlowModel& flow)
    {
        return PassByConstReference(flow);
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeConstCorrectness(const FlowModel& flow)
    {
        return ConstCorrectness(flow);
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeUnsafeDowncast(const TypeModel& types)
    {
        return UnsafeDowncast(types);
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeSlicing(const TypeModel& types)
    {
        return Slicing(types);
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeImplicitConversion(const SemanticModel& model)
    {
        ApiAnalysis analysis(model);
        return ImplicitConversion(analysis);
    }

} // namespace heimdall
