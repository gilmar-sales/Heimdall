#include <Heimdall/FlowModel.hpp>

#include "detail/TokenView.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace heimdall
{

    FunctionTable::FunctionTable(std::pmr::memory_resource *resource)
    : node(resource), body(resource), symbol(resource), entry(resource), exit(resource), first_block(resource),
        block_count(resource), complete(resource) {}

    BlockTable::BlockTable(std::pmr::memory_resource *resource)
    : function(resource), first_event(resource), event_count(resource), first_succ(resource), succ_count(resource),
        returns(resource) {}

    EventTable::EventTable(std::pmr::memory_resource *resource)
    : kind(resource), symbol(resource), token(resource), block(resource) {}

    FlowModel::FlowModel(const TypeModel &types, std::size_t arena_hint)
    : m_arena(std::make_unique<Arena>(arena_hint)), m_types(&types), m_functions(m_arena->Resource()),
        m_blocks(m_arena->Resource()), m_events(m_arena->Resource()), m_successors(m_arena->Resource()),
        m_owner(m_arena->Resource()), m_symbol_begin(m_arena->Resource()), m_symbol_events(m_arena->Resource()),
        m_node_function(m_arena->Resource()) {}

    FunctionId FlowModel::FunctionOfNode(std::uint32_t node) const noexcept
    {
        return node < m_node_function.size() ? m_node_function[node] : kNone;
    }

    std::vector<std::uint8_t> FlowModel::ReachableSet(FunctionId function) const
    {
        std::vector<std::uint8_t> seen;
        if (function >= m_functions.Size())
        {
            return seen;
        }

        const auto first = m_functions.first_block[function];
        seen.assign(m_functions.block_count[function], 0);
        std::vector<BlockId> pending {m_functions.entry[function]};
        seen[m_functions.entry[function] - first] = 1;
        while (!pending.empty())
        {
            const auto current = pending.back();
            pending.pop_back();
            for (const auto next: Successors(current))
            {
                if (next >= first && next - first < seen.size() && seen[next - first] == 0)
                {
                    seen[next - first] = 1;
                    pending.push_back(next);
                }
            }
        }

        return seen;
    }

    bool FlowModel::IsReachable(FunctionId function, BlockId block) const
    {
        const auto seen = ReachableSet(function);
        const auto first = function < m_functions.Size() ? m_functions.first_block[function] : 0;
        return block >= first && block - first < seen.size() && seen[block - first] != 0;
    }

    bool FlowModel::ExitReachable(FunctionId function) const
    {
        return function < m_functions.Size() && IsReachable(function, m_functions.exit[function]);
    }

    bool FlowModel::HasReachableReturn(FunctionId function) const
    {
        const auto seen = ReachableSet(function);
        const auto first = function < m_functions.Size() ? m_functions.first_block[function] : 0;
        for (std::size_t i = 0; i < seen.size(); ++i)
        {
            if (seen[i] != 0 && m_blocks.returns[first + i] != 0)
            {
                return true;
            }
        }

        return false;
    }

    bool FlowModel::IsNeverModified(SymbolId symbol) const
    {
        const auto owner = OwnerOf(symbol);
        if (owner == kNone || m_functions.complete[owner] == 0)
        {
            return false;
        }

        std::size_t inits = 0;
        for (const auto index: EventsOf(symbol))
        {
            switch (m_events.kind[index])
            {
            case EventKind::Init:
                ++inits;
                break;
            case EventKind::Read:
                break;
            default:
                return false;
            }
        }

        return inits == 1;
    }

    // ---------------------------------------------------------------------

    class FlowBuilder
    {
    public:
        explicit FlowBuilder(FlowModel &out)
        : m(out), types(out.Types()), table(out.Types().Types()), model(out.Model()),
          nodes(out.Model().Tree().NodesSoA()), symbols(out.Model().Symbols()), view(out.Model())
        {
        }

        void Run()
        {
            FindFunctions();
            m.m_owner.assign(symbols.Size(), kNone);
            AssignOwners();
            for (FunctionId function = 0; function < m.m_functions.Size(); ++function)
            {
                BuildFunction(function);
            }

            Finish();
        }

    private:
        struct RawEvent
        {
            EventKind kind;
            SymbolId symbol;
            std::uint32_t token;
            BlockId block;
        };

        static constexpr std::size_t kMaxDepth = 128;
        static constexpr std::uint32_t kUnset = ~0u - 1;

        using Range = std::pair<std::size_t, std::size_t>;

        // ---- indexing ---------------------------------------------------
        bool IsFunctionLike(std::uint32_t node) const
        {
            return nodes.Kind(node) == GrammarKind::FunctionDefinition ||
                nodes.Kind(node) == GrammarKind::LambdaExpression;
        }

        std::uint32_t BodyOf(std::uint32_t node) const
        {
            std::uint32_t body = kNone;
            for (const auto child: model.ChildrenOf(node))
            {
                if (nodes.Kind(child) == GrammarKind::CompoundStatement &&
                    (body == kNone || nodes.FirstToken(child) > nodes.FirstToken(body)))
                {
                    body = child;
                }
            }

            return body;
        }

        void FindFunctions()
        {
            std::unordered_map<std::uint32_t, SymbolId> by_node;
            for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
            {
                if (symbols.kind[symbol] == SymbolKind::Function && (symbols.flags[symbol] & SymbolFlag::Definition) != 0)
                {
                    by_node.emplace(symbols.decl_node[symbol], symbol);
                }
            }

            auto &functions = m.m_functions;
            m.m_node_function.assign(nodes.size(), kUnset);
            for (std::uint32_t node = 0; node < nodes.size(); ++node)
            {
                if (!IsFunctionLike(node))
                {
                    continue;
                }

                const auto body = BodyOf(node);
                if (body == kNone)
                {
                    continue;
                }

                const auto found = by_node.find(node);
                m.m_node_function[node] = static_cast<FunctionId>(functions.Size());
                functions.node.push_back(node);
                functions.body.push_back(body);
                functions.symbol.push_back(found == by_node.end() ? kNone : found->second);
                functions.entry.push_back(kNone);
                functions.exit.push_back(kNone);
                functions.first_block.push_back(0);
                functions.block_count.push_back(0);
                functions.complete.push_back(1);
            }
        }

        // Innermost function-like node at or above `node` that has a body.
        FunctionId EnclosingFunction(std::uint32_t node)
        {
            std::vector<std::uint32_t> path;
            std::uint32_t current = node;
            FunctionId result = kNone;
            for (std::size_t steps = 0; steps < nodes.size() + 1; ++steps)
            {
                if (current >= nodes.size())
                {
                    break;
                }

                if (m.m_node_function[current] != kUnset)
                {
                    result = m.m_node_function[current];
                    break;
                }

                path.push_back(current);
                if (nodes.Parent(current) == current)
                {
                    break;
                }

                current = nodes.Parent(current);
            }

            for (const auto visited: path)
            {
                m.m_node_function[visited] = result;
            }

            return result;
        }

        void AssignOwners()
        {
            const auto &scopes = model.Scopes();
            for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
            {
                const auto kind = symbols.kind[symbol];
                if (kind != SymbolKind::Variable && kind != SymbolKind::Parameter)
                {
                    continue;
                }

                const auto scope_kind = scopes.kind[symbols.scope[symbol]];
                if (scope_kind != ScopeKind::Function && scope_kind != ScopeKind::Block)
                {
                    continue;
                }

                if (symbols.decl_node[symbol] < nodes.size())
                {
                    m.m_owner[symbol] = EnclosingFunction(symbols.decl_node[symbol]);
                }

                m_symbol_at.emplace(symbols.decl_token[symbol], symbol);
                m_locals_by_name[symbols.name[symbol]].push_back(symbol);
            }

        }

        // ---- blocks and edges --------------------------------------------
        BlockId NewBlock()
        {
            m_block_function.push_back(m_function);
            m_block_returns.push_back(0);
            return static_cast<BlockId>(m_block_function.size() - 1);
        }

        void Edge(BlockId from, BlockId to)
        {
            if (from != kNone && to != kNone)
            {
                m_edges.emplace_back(from, to);
            }
        }

        void Emit(EventKind kind, SymbolId symbol, std::uint32_t token)
        {
            m_events.push_back({kind, symbol, token, m_current});
        }

        void Incomplete()
        {
            m.m_functions.complete[m_function] = 0;
        }

        // ---- node helpers -------------------------------------------------
        std::vector<std::uint32_t> Sorted(std::uint32_t node) const
        {
            const auto children = model.ChildrenOf(node);
            std::vector<std::uint32_t> result(children.begin(), children.end());
            std::stable_sort(result.begin(), result.end(),
                [&](std::uint32_t a, std::uint32_t b)
                {
                    return nodes.FirstToken(a) < nodes.FirstToken(b);
                });
            return result;
        }

        std::uint32_t FindChild(std::uint32_t node, GrammarKind kind) const
        {
            for (const auto child: model.ChildrenOf(node))
            {
                if (nodes.Kind(child) == kind)
                {
                    return child;
                }
            }

            return kNone;
        }

        SymbolId SymbolAt(std::uint32_t token) const
        {
            const auto found = m_symbol_at.find(token);
            return found == m_symbol_at.end() ? kNone : found->second;
        }

        // `(` ... `)` of an if/for/while/switch header: positions of both.
        bool Header(std::uint32_t node, std::size_t &open, std::size_t &close) const
        {
            const auto begin = view.Range(node).first;
            const auto end = view.Range(node).second;
            for (open = begin + 1; open < end && open < begin + 4; ++open)
            {
                if (view.At(open) == Tok::LParen)
                {
                    close = view.Match(open, end);
                    return close < end;
                }
            }

            return false;
        }

        bool IsAssignment(Tok tok) const
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

        // ---- type shape of a variable -----------------------------------
        struct Shape
        {
            bool scalar = false;  // arithmetic, enum or pointer: no members, no overloaded operators
            bool pointer = false;
            bool array = false;
            bool external = false; // library type
            bool known = false;
            TypeId value = TypeTable::Unknown;
        };

        Shape ShapeOf(SymbolId symbol) const
        {
            Shape shape;
            shape.value = table.Strip(types.SymbolType(symbol));
            shape.known = table.IsKnown(shape.value);
            shape.pointer = table.IsPointer(shape.value);
            shape.array = table.IsArray(shape.value);
            shape.external = table.Kind(shape.value) == TypeKind::External;
            shape.scalar = table.IsArithmetic(shape.value) || table.Kind(shape.value) == TypeKind::Enum || shape.pointer;
            return shape;
        }

        // ---- classification of one use ------------------------------------
        EventKind Classify(std::uint32_t node, SymbolId symbol)
        {
            const auto shape = ShapeOf(symbol);
            bool nested = false; // reached through `x[i]` or `x.m`: the object is changed, not replaced
            const auto finish = [&](EventKind kind)
            {
                return nested && kind == EventKind::Write ? EventKind::Modify : kind;
            };

            std::uint32_t current = node;
            for (std::size_t steps = 0; steps < 64; ++steps)
            {
                const auto parent = nodes.Parent(current);
                if (parent >= nodes.size() || parent == current)
                {
                    return EventKind::Escape;
                }

                const auto kids = Sorted(parent);
                const auto [begin, end] = view.Range(parent);
                switch (nodes.Kind(parent))
                {
                case GrammarKind::ParenthesizedExpression:
                    current = parent;
                    continue;
                case GrammarKind::UnaryExpression:
                {
                    const Tok first = view.At(begin);
                    const Tok last = end > begin ? view.At(end - 1) : Tok::None;
                    if (first == Tok::PlusPlus || first == Tok::MinusMinus ||
                        (kids.size() == 1 && kids[0] == current && view.Range(current).first == begin &&
                            (last == Tok::PlusPlus || last == Tok::MinusMinus)))
                    {
                        return EventKind::Modify;
                    }

                    if (first == Tok::Amp)
                    {
                        return EventKind::Escape;
                    }

                    if (first == Tok::Star && shape.array)
                    {
                        nested = true;
                        current = parent;
                        continue;
                    }

                    if (first == Tok::Star || first == Tok::Plus || first == Tok::Minus || first == Tok::Bang ||
                        first == Tok::Tilde || first == Tok::KwThrow)
                    {
                        return shape.scalar || shape.external ? EventKind::Read : EventKind::Escape;
                    }

                    return EventKind::Escape;
                }
                case GrammarKind::BinaryExpression:
                {
                    if (kids.size() != 2)
                    {
                        return EventKind::Escape;
                    }

                    const auto op = view.Range(kids[0]).second;
                    const Tok tok = view.At(op);
                    if (kids[0] == current)
                    {
                        if (tok == Tok::Eq)
                        {
                            return finish(EventKind::Write);
                        }

                        if (IsAssignment(tok))
                        {
                            return EventKind::Modify;
                        }

                        if (tok == Tok::DotStar || tok == Tok::ArrowStar)
                        {
                            return EventKind::Escape;
                        }

                        if ((tok == Tok::Shl || tok == Tok::Shr) && !shape.scalar)
                        {
                            return EventKind::Modify; // `out << x` writes to the stream
                        }

                        return EventKind::Read;
                    }

                    if (tok == Tok::Comma)
                    {
                        current = parent;
                        continue;
                    }

                    if (tok == Tok::Shr && !(table.IsArithmetic(table.Strip(types.NodeType(kids[0])))))
                    {
                        return EventKind::Modify; // `in >> x` writes to x
                    }

                    return EventKind::Read;
                }
                case GrammarKind::ConditionalExpression:
                    if (!kids.empty() && kids[0] == current)
                    {
                        return EventKind::Read;
                    }

                    current = parent;
                    continue;
                case GrammarKind::SubscriptExpression:
                    if (kids.size() >= 2 && kids[1] == current)
                    {
                        return EventKind::Read;
                    }

                    if (shape.pointer)
                    {
                        return EventKind::Read;
                    }

                    nested = true;
                    current = parent;
                    continue;
                case GrammarKind::MemberExpression:
                {
                    if (kids.size() != 2 || kids[0] != current)
                    {
                        return EventKind::Escape;
                    }

                    const Tok tok = view.At(view.Range(kids[0]).second);
                    if (tok == Tok::Arrow)
                    {
                        return shape.pointer ? EventKind::Read : EventKind::Escape;
                    }

                    if (tok != Tok::Dot || shape.scalar)
                    {
                        return EventKind::Escape;
                    }

                    // `x.f(...)`: whether f is a const member decides.
                    const auto grand = nodes.Parent(parent);
                    if (grand < nodes.size() && nodes.Kind(grand) == GrammarKind::CallExpression)
                    {
                        const auto call_kids = Sorted(grand);
                        if (!call_kids.empty() && call_kids[0] == parent)
                        {
                            return IsConstMethod(shape, view.Text(view.Range(kids[1]).first)) ? EventKind::Read
                                                                                              : EventKind::Modify;
                        }
                    }

                    nested = true;
                    current = parent;
                    continue;
                }
                case GrammarKind::CallExpression:
                {
                    if (kids.empty())
                    {
                        return EventKind::Escape;
                    }

                    if (kids[0] == current)
                    {
                        return shape.pointer ? EventKind::Read : EventKind::Escape;
                    }

                    std::size_t index = 0;
                    while (index < kids.size() && kids[index] != current)
                    {
                        ++index;
                    }

                    return index < kids.size() ? ArgumentContext(kids[0], index - 1, shape) : EventKind::Escape;
                }
                case GrammarKind::ExpressionStatement:
                case GrammarKind::IfStatement:
                case GrammarKind::SwitchStatement:
                case GrammarKind::DoStatement:
                case GrammarKind::CaseLabel:
                    return EventKind::Read;
                case GrammarKind::LoopStatement:
                {
                    // The container of `for (auto e : c)` is iterated by reference.
                    const auto first = view.Range(current).first;
                    return first > 0 && view.At(first - 1) == Tok::Colon ? EventKind::Escape : EventKind::Read;
                }
                case GrammarKind::ReturnStatement:
                    return ReturnContext(shape);
                case GrammarKind::InitDeclarator:
                    return InitializerContext(parent);
                default:
                    return EventKind::Escape;
                }
            }

            return EventKind::Escape;
        }

        // Const members the engine trusts to leave the object alone.
        bool IsConstMethod(const Shape &shape, std::string_view name) const
        {
            if (shape.external)
            {
                static constexpr std::string_view kConst[] = {"size", "empty", "length", "capacity", "max_size",
                    "count", "contains", "c_str", "substr", "compare", "starts_with", "ends_with"};
                return std::find(std::begin(kConst), std::end(kConst), name) != std::end(kConst);
            }

            if (table.Kind(shape.value) != TypeKind::Class)
            {
                return false;
            }

            const auto id = model.Names().Find(name);
            const auto klass = static_cast<SymbolId>(table.Arg(shape.value));
            const auto first = id == kNone || klass >= symbols.Size() ? kNone
                                                                     : model.LookupMember(klass, id);
            if (first == kNone || symbols.kind[first] != SymbolKind::Function)
            {
                return false;
            }

            const auto scope = symbols.scope[first];
            for (auto overload = model.LookupLocal(scope, id); overload != kNone; overload = symbols.next_same_name[overload])
            {
                if (symbols.kind[overload] != SymbolKind::Function || (symbols.flags[overload] & SymbolFlag::Const) == 0)
                {
                    return false;
                }
            }

            return true;
        }

        EventKind ReturnContext(const Shape &shape) const
        {
            // Returning a class object by name can be a move; `const` would turn it into a copy.
            if (!shape.scalar)
            {
                return EventKind::Escape;
            }

            const auto symbol = m.m_functions.symbol[m_function];
            if (symbol != kNone)
            {
                const auto kind = table.Kind(types.SymbolType(symbol));
                if (kind == TypeKind::LRef || kind == TypeKind::RRef)
                {
                    return EventKind::Escape;
                }
            }

            return EventKind::Read;
        }

        // What `T v = x;` does to x, by the declared type of v.
        EventKind InitializerContext(std::uint32_t init_declarator) const
        {
            const auto declarator = FindChild(init_declarator, GrammarKind::Declarator);
            const auto name = declarator == kNone ? kNone : FindChild(declarator, GrammarKind::DeclaredName);
            const auto symbol = name == kNone ? kNone : SymbolAt(nodes.FirstToken(name));
            if (symbol == kNone)
            {
                return EventKind::Escape;
            }

            const auto type = types.SymbolType(symbol);
            switch (table.Kind(type))
            {
            case TypeKind::Unknown:
            case TypeKind::RRef:
                return EventKind::Escape;
            case TypeKind::LRef:
                return table.Kind(table.Arg(type)) == TypeKind::Const ? EventKind::Read : EventKind::Escape;
            default:
                return EventKind::Read;
            }
        }

        // Library functions that take their arguments by value or const reference.
        static bool TakesByValue(std::string_view name)
        {
            static constexpr std::string_view kNames[] = {"printf", "std::printf", "puts", "putchar", "sqrt", "std::sqrt",
                "pow", "std::pow", "floor", "std::floor", "ceil", "std::ceil", "fabs", "std::fabs", "std::min",
                "std::max", "std::to_string", "std::format", "std::print", "std::println"};
            return std::find(std::begin(kNames), std::end(kNames), name) != std::end(kNames);
        }

        EventKind ArgumentContext(std::uint32_t callee, std::size_t index, const Shape &shape)
        {
            const auto range = view.Range(callee);
            if (nodes.Kind(callee) == GrammarKind::IdentifierExpression)
            {
                const auto text = view.Text(range.first);
                if (text == "sizeof" || text == "alignof" || text == "noexcept" || text == "decltype")
                {
                    return EventKind::Read;
                }

                const auto target = model.ResolveToken(nodes.FirstToken(callee));
                if (target == kNone)
                {
                    return shape.scalar && TakesByValue(text) ? EventKind::Read : EventKind::Escape;
                }

                return symbols.kind[target] == SymbolKind::Function ? ParameterContext(target, index, shape)
                                                                    : EventKind::Escape;
            }

            // `static_cast<T>(x)` copies a value; `static_cast<T&>(x)` aliases it.
            if (view.At(range.first) == Tok::KwStaticCast)
            {
                for (auto p = range.first; p < range.second; ++p)
                {
                    if (view.At(p) == Tok::Amp || view.At(p) == Tok::AmpAmp)
                    {
                        return EventKind::Escape;
                    }
                }

                return shape.scalar ? EventKind::Read : EventKind::Escape;
            }

            if (nodes.Kind(callee) == GrammarKind::MemberExpression)
            {
                std::string text;
                for (auto i = range.first; i < range.second; ++i)
                {
                    if (view.At(i) != Tok::ColonColon && !view.IsWord(i))
                    {
                        return EventKind::Escape;
                    }

                    text += view.Text(i);
                }

                return shape.scalar && TakesByValue(text) ? EventKind::Read : EventKind::Escape;
            }

            return EventKind::Escape;
        }

        // Every overload that could take the call: by value or `const T&` keeps the
        // argument untouched.
        EventKind ParameterContext(SymbolId function, std::size_t index, const Shape &shape)
        {
            bool viable = false;
            for (auto overload = model.LookupLocal(symbols.scope[function], symbols.name[function]); overload != kNone;
                overload = symbols.next_same_name[overload])
            {
                if (symbols.kind[overload] != SymbolKind::Function || (symbols.flags[overload] & SymbolFlag::Template) != 0)
                {
                    return EventKind::Escape;
                }

                const auto open = view.PositionOf(symbols.decl_token[overload]) + 1;
                if (view.At(open) != Tok::LParen)
                {
                    return EventKind::Escape;
                }

                const auto close = view.Match(open, view.Size());
                // Split the parameter list at top-level commas.
                std::size_t count = 0;
                std::size_t begin = open + 1;
                std::size_t depth = 0;
                bool found = false;
                bool by_const_ref_or_value = false;
                for (std::size_t p = open + 1; p <= close; ++p)
                {
                    const Tok tok = view.At(p);
                    if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace || tok == Tok::Lt)
                    {
                        ++depth;
                    }
                    else if ((tok == Tok::RParen && p != close) || tok == Tok::RBracket || tok == Tok::RBrace ||
                        tok == Tok::Gt)
                    {
                        depth = depth == 0 ? 0 : depth - 1;
                    }
                    else if (tok == Tok::Ellipsis)
                    {
                        return EventKind::Escape;
                    }

                    if ((tok == Tok::Comma && depth == 0) || p == close)
                    {
                        if (p > begin && count == index)
                        {
                            found = true;
                            bool is_const = false;
                            bool is_ref = false;
                            bool is_rref = false;
                            bool is_pointer = false;
                            for (auto q = begin; q < p; ++q)
                            {
                                const Tok part = view.At(q);
                                if (part == Tok::KwConst)
                                {
                                    is_const = true;
                                }
                                else if (part == Tok::Amp)
                                {
                                    is_ref = true;
                                }
                                else if (part == Tok::AmpAmp)
                                {
                                    is_rref = true;
                                }
                                else if (part == Tok::Star)
                                {
                                    is_pointer = true;
                                }
                            }

                            by_const_ref_or_value = is_rref ? false
                                : is_ref ? is_const : (!shape.array || (is_pointer && is_const));
                        }

                        if (p > begin)
                        {
                            ++count;
                        }

                        begin = p + 1;
                    }
                }

                if (!found)
                {
                    continue; // fewer parameters than this call has arguments: not viable
                }

                viable = true;
                if (!by_const_ref_or_value)
                {
                    return EventKind::Escape;
                }
            }

            return viable ? EventKind::Read : EventKind::Escape;
        }

        // ---- expressions: events of every local named inside ----------------
        bool Within(std::uint32_t node, std::uint32_t ancestor) const
        {
            for (std::size_t steps = 0; steps < nodes.size() && node < nodes.size(); ++steps)
            {
                if (node == ancestor)
                {
                    return true;
                }

                if (nodes.Parent(node) == node)
                {
                    return false;
                }

                node = nodes.Parent(node);
            }

            return false;
        }

        // Identifier nodes below `node`, not descending into function-like nodes,
        // which are reported separately through `nested`.
        void CollectRefs(std::uint32_t node, std::vector<std::uint32_t> &refs, std::vector<std::uint32_t> &nested)
        {
            std::vector<std::uint32_t> pending {node};
            while (!pending.empty())
            {
                const auto current = pending.back();
                pending.pop_back();
                switch (nodes.Kind(current))
                {
                case GrammarKind::IdentifierExpression:
                    refs.push_back(current);
                    continue;
                case GrammarKind::LambdaExpression:
                case GrammarKind::FunctionDefinition:
                case GrammarKind::RecordDefinition:
                    nested.push_back(current);
                    continue;
                case GrammarKind::Error:
                case GrammarKind::ErrorExpression:
                    Incomplete();
                    break;
                default:
                    break;
                }

                for (const auto child: model.ChildrenOf(current))
                {
                    pending.push_back(child);
                }
            }
        }

        void Walk(std::uint32_t node)
        {
            std::vector<std::uint32_t> refs;
            std::vector<std::uint32_t> nested;
            CollectRefs(node, refs, nested);
            std::vector<RawEvent> local;
            for (const auto ref: refs)
            {
                const auto token = nodes.FirstToken(ref);
                const auto symbol = model.ResolveToken(token);
                if (symbol != kNone && m.m_owner[symbol] == m_function)
                {
                    // `decltype(x)`: adding `const` to x would change the type.
                    const auto position = view.PositionOf(token);
                    const bool in_decltype = position >= 2 && view.At(position - 1) == Tok::LParen &&
                        view.At(position - 2) == Tok::KwDecltype;
                    local.push_back({in_decltype ? EventKind::Escape : Classify(ref, symbol), symbol, token, m_current});
                }
            }

            // Whatever a nested function or local class names from this body escapes.
            for (const auto inner: nested)
            {
                std::vector<std::uint32_t> inner_refs;
                CollectAll(inner, inner_refs);
                for (const auto ref: inner_refs)
                {
                    const auto token = nodes.FirstToken(ref);
                    const auto symbol = model.ResolveToken(token);
                    if (symbol != kNone && m.m_owner[symbol] == m_function)
                    {
                        local.push_back({EventKind::Escape, symbol, token, m_current});
                    }
                }
            }

            std::sort(local.begin(), local.end(),
                [](const RawEvent &a, const RawEvent &b)
                {
                    return a.token < b.token;
                });
            m_events.insert(m_events.end(), local.begin(), local.end());
        }

        // Every identifier node below `node`, nested functions included.
        void CollectAll(std::uint32_t node, std::vector<std::uint32_t> &refs)
        {
            std::vector<std::uint32_t> pending {node};
            while (!pending.empty())
            {
                const auto current = pending.back();
                pending.pop_back();
                if (nodes.Kind(current) == GrammarKind::IdentifierExpression)
                {
                    refs.push_back(current);
                    continue;
                }

                for (const auto child: model.ChildrenOf(current))
                {
                    pending.push_back(child);
                }
            }
        }

        // ---- statements -----------------------------------------------------
        void Declaration(std::uint32_t node)
        {
            for (const auto child: Sorted(node))
            {
                if (nodes.Kind(child) == GrammarKind::InitDeclarator || nodes.Kind(child) == GrammarKind::Declarator)
                {
                    std::uint32_t name = kNone;
                    const auto declarator = nodes.Kind(child) == GrammarKind::Declarator
                        ? child : FindChild(child, GrammarKind::Declarator);
                    if (declarator != kNone)
                    {
                        const auto declared = FindChild(declarator, GrammarKind::DeclaredName);
                        name = declared == kNone ? kNone : nodes.FirstToken(declared);
                    }
                    else if (const auto spelled = FindChild(child, GrammarKind::TypeSpecifier);
                        spelled != kNone && nodes.TokenCount(spelled) == 1)
                    {
                        name = nodes.FirstToken(spelled); // later declarators of `int a = 0, b = 0;`
                    }

                    const auto symbol = name == kNone ? kNone : SymbolAt(name);
                    if (symbol != kNone && m.m_owner[symbol] == m_function)
                    {
                        auto after = view.PositionOf(name) + 1;
                        while (view.At(after) == Tok::LBracket) // array bounds: `int a[3] = {...}`
                        {
                            const auto close = view.Match(after, view.Size());
                            after = close < view.Size() ? close + 1 : view.Size();
                        }

                        const Tok next = view.At(after);
                        const bool initialized = next == Tok::Eq || next == Tok::LBrace;
                        Emit(initialized ? EventKind::Init : EventKind::Uninit, symbol, name);
                    }
                }

                Walk(child);
            }
        }

        BlockId Statement(std::uint32_t node, BlockId current, std::size_t depth)
        {
            if (depth > kMaxDepth)
            {
                Incomplete();
                return current;
            }

            if (current == kNone)
            {
                current = NewBlock(); // dead code: still analyzed, with no way in
            }

            m_current = current;
            switch (nodes.Kind(node))
            {
            case GrammarKind::CompoundStatement:
                for (const auto child: Sorted(node))
                {
                    current = Statement(child, current, depth + 1);
                }

                return current;
            case GrammarKind::DeclarationStatement:
            case GrammarKind::Declaration:
                Declaration(node);
                return current;
            case GrammarKind::ExpressionStatement:
                Walk(node);
                if (view.At(view.Range(node).first) == Tok::KwThrow)
                {
                    m_block_returns[current] = 1;
                    Edge(current, m_exit);
                    return kNone;
                }

                return current;
            case GrammarKind::ReturnStatement:
                Walk(node);
                m_block_returns[current] = 1;
                Edge(current, m_exit);
                return kNone;
            case GrammarKind::JumpStatement:
            {
                const Tok tok = view.At(view.Range(node).first);
                if (tok == Tok::KwBreak && !m_breaks.empty())
                {
                    Edge(current, m_breaks.back());
                }
                else if (tok == Tok::KwContinue && !m_continues.empty())
                {
                    Edge(current, m_continues.back());
                }
                else
                {
                    Incomplete();
                }

                return kNone;
            }
            case GrammarKind::EmptyStatement:
                return current;
            case GrammarKind::IfStatement:
                return If(node, current, depth);
            case GrammarKind::LoopStatement:
                return Loop(node, current, depth);
            case GrammarKind::DoStatement:
                return Do(node, current, depth);
            case GrammarKind::SwitchStatement:
                return Switch(node, current, depth);
            case GrammarKind::TryStatement:
                return Try(node, current, depth);
            case GrammarKind::Error:
            case GrammarKind::ErrorExpression:
                Incomplete();
                return current;
            default:
                Walk(node);
                return current;
            }
        }

        bool IsStatement(GrammarKind kind) const
        {
            switch (kind)
            {
            case GrammarKind::CompoundStatement:
            case GrammarKind::DeclarationStatement:
            case GrammarKind::ExpressionStatement:
            case GrammarKind::ReturnStatement:
            case GrammarKind::IfStatement:
            case GrammarKind::LoopStatement:
            case GrammarKind::SwitchStatement:
            case GrammarKind::JumpStatement:
            case GrammarKind::EmptyStatement:
            case GrammarKind::TryStatement:
            case GrammarKind::DoStatement:
                return true;
            default:
                return false;
            }
        }

        // Children of a header, split at the closing parenthesis: the part inside
        // is evaluated, the rest are the bodies.
        void SplitHeader(std::uint32_t node, std::size_t close, std::vector<std::uint32_t> &header,
            std::vector<std::uint32_t> &bodies) const
        {
            for (const auto child: Sorted(node))
            {
                (view.Range(child).first > close ? bodies : header).push_back(child);
            }
        }

        void Evaluate(const std::vector<std::uint32_t> &parts, BlockId current)
        {
            m_current = current;
            for (const auto part: parts)
            {
                if (nodes.Kind(part) == GrammarKind::DeclarationStatement || nodes.Kind(part) == GrammarKind::Declaration)
                {
                    Declaration(part);
                }
                else
                {
                    Walk(part);
                }
            }
        }

        BlockId If(std::uint32_t node, BlockId current, std::size_t depth)
        {
            std::size_t open = 0;
            std::size_t close = 0;
            if (!Header(node, open, close))
            {
                Incomplete();
                Walk(node);
                return current;
            }

            std::vector<std::uint32_t> header;
            std::vector<std::uint32_t> bodies;
            SplitHeader(node, close, header, bodies);
            Evaluate(header, current);

            BlockId then_end = current;
            BlockId else_end = current;
            for (const auto body: bodies)
            {
                const bool is_else = view.At(view.Range(body).first - 1) == Tok::KwElse;
                const auto start = NewBlock();
                Edge(current, start);
                (is_else ? else_end : then_end) = Statement(body, start, depth + 1);
            }

            if (then_end == kNone && else_end == kNone)
            {
                return kNone;
            }

            const auto join = NewBlock();
            Edge(then_end, join);
            Edge(else_end, join);
            return join;
        }

        BlockId Loop(std::uint32_t node, BlockId current, std::size_t depth)
        {
            std::size_t open = 0;
            std::size_t close = 0;
            if (!Header(node, open, close))
            {
                Incomplete();
                Walk(node);
                return current;
            }

            // `for (init; cond; step)`, `for (decl : range)` or `while (cond)`.
            std::size_t semi1 = close;
            std::size_t semi2 = close;
            std::size_t depth_parens = 0;
            std::size_t found = 0;
            for (std::size_t p = open + 1; p < close; ++p)
            {
                const Tok tok = view.At(p);
                if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace)
                {
                    ++depth_parens;
                }
                else if (tok == Tok::RParen || tok == Tok::RBracket || tok == Tok::RBrace)
                {
                    depth_parens = depth_parens == 0 ? 0 : depth_parens - 1;
                }
                else if (tok == Tok::Semi && depth_parens == 0 && found < 2)
                {
                    (found == 0 ? semi1 : semi2) = p;
                    ++found;
                }
            }

            std::vector<std::uint32_t> header;
            std::vector<std::uint32_t> bodies;
            SplitHeader(node, close, header, bodies);

            std::vector<std::uint32_t> init;
            std::vector<std::uint32_t> condition;
            std::vector<std::uint32_t> step;
            const bool is_for = view.At(view.Range(node).first) == Tok::KwFor;
            for (const auto part: header)
            {
                const auto first = view.Range(part).first;
                if (!is_for || found < 2)
                {
                    condition.push_back(part); // `while (c)` and range-for: evaluated each round
                }
                else if (first < semi1)
                {
                    init.push_back(part);
                }
                else if (first < semi2)
                {
                    condition.push_back(part);
                }
                else
                {
                    step.push_back(part);
                }
            }

            Evaluate(init, current);
            const auto test = NewBlock();
            Edge(current, test);
            Evaluate(condition, test);
            const auto body_start = NewBlock();
            const auto after = NewBlock();
            const auto step_block = NewBlock();
            Edge(test, body_start);
            // `for (;;)` leaves only through `break`.
            if (!(is_for && found == 2 && condition.empty()))
            {
                Edge(test, after);
            }

            m_breaks.push_back(after);
            m_continues.push_back(step_block);
            BlockId end = body_start;
            for (const auto body: bodies)
            {
                end = Statement(body, end, depth + 1);
            }

            m_breaks.pop_back();
            m_continues.pop_back();
            Edge(end, step_block);
            Evaluate(step, step_block);
            Edge(step_block, test);
            return after;
        }

        BlockId Do(std::uint32_t node, BlockId current, std::size_t depth)
        {
            const auto body_start = NewBlock();
            const auto condition = NewBlock();
            const auto after = NewBlock();
            Edge(current, body_start);
            m_breaks.push_back(after);
            m_continues.push_back(condition);
            BlockId end = body_start;
            std::vector<std::uint32_t> tail;
            bool have_body = false;
            for (const auto child: Sorted(node))
            {
                if (!have_body && IsStatement(nodes.Kind(child)))
                {
                    have_body = true;
                    end = Statement(child, end, depth + 1);
                }
                else
                {
                    tail.push_back(child);
                }
            }

            if (!have_body)
            {
                Incomplete();
            }

            m_breaks.pop_back();
            m_continues.pop_back();
            Edge(end, condition);
            Evaluate(tail, condition);
            Edge(condition, body_start);
            Edge(condition, after);
            return after;
        }

        BlockId Switch(std::uint32_t node, BlockId current, std::size_t depth)
        {
            std::size_t open = 0;
            std::size_t close = 0;
            if (!Header(node, open, close))
            {
                Incomplete();
                Walk(node);
                return current;
            }

            std::vector<std::uint32_t> header;
            std::vector<std::uint32_t> bodies;
            SplitHeader(node, close, header, bodies);
            Evaluate(header, current);

            const auto after = NewBlock();
            m_breaks.push_back(after);
            bool has_default = false;
            BlockId previous = kNone;
            for (const auto body : bodies)
            {
                if (nodes.Kind(body) != GrammarKind::CompoundStatement)
                {
                    Incomplete();
                    Walk(body);
                    continue;
                }

                for (const auto child: Sorted(body))
                {
                    if (nodes.Kind(child) == GrammarKind::CaseLabel)
                    {
                        const auto label = NewBlock();
                        Edge(current, label);
                        Edge(previous, label);
                        previous = label;
                        m_current = label;
                        Walk(child);
                        has_default = has_default || view.At(view.Range(child).first) == Tok::KwDefault;
                    }
                    else
                    {
                        previous = Statement(child, previous, depth + 1);
                    }
                }
            }

            m_breaks.pop_back();
            Edge(previous, after);
            if (!has_default)
            {
                Edge(current, after);
            }

            return after;
        }

        BlockId Try(std::uint32_t node, BlockId current, std::size_t depth)
        {
            const auto join = NewBlock();
            for (const auto child: Sorted(node))
            {
                if (nodes.Kind(child) != GrammarKind::CompoundStatement)
                {
                    m_current = current;
                    Walk(child);
                    continue;
                }

                // The body, then every handler, which may start anywhere in the body.
                const auto start = NewBlock();
                Edge(current, start);
                Edge(Statement(child, start, depth + 1), join);
            }

            return join;
        }

        // ---- per function -----------------------------------------------------
        void BuildFunction(FunctionId function)
        {
            m_function = function;
            auto &functions = m.m_functions;
            const auto first = static_cast<BlockId>(m_block_function.size());
            const auto entry = NewBlock();
            m_exit = NewBlock();
            functions.entry[function] = entry;
            functions.exit[function] = m_exit;
            functions.first_block[function] = first;
            m_breaks.clear();
            m_continues.clear();
            m_current = entry;

            if (ContainsUnmodeled(functions.body[function]))
            {
                Incomplete();
            }

            const auto end = Statement(functions.body[function], entry, 0);
            Edge(end, m_exit);
            functions.block_count[function] = static_cast<std::uint32_t>(m_block_function.size() - first);
            ScanStray(function);
        }

        bool ContainsUnmodeled(std::uint32_t body) const
        {
            const auto [begin, end] = view.Range(body);
            for (auto p = begin; p < end; ++p)
            {
                switch (view.At(p))
                {
                case Tok::KwGoto:
                case Tok::KwAsm:
                case Tok::KwCoAwait:
                case Tok::KwCoReturn:
                case Tok::KwCoYield:
                    return true;
                default:
                    break;
                }
            }

            return false;
        }

        // Names the grammar did not model as expressions (arguments of a
        // constructor call spelled `T t(x);`, elements of `{x, y}`, macro
        // arguments...) are accounted for as escapes of every local of that name.
        void ScanStray(FunctionId function)
        {
            const auto [begin, end] = view.Range(m.m_functions.body[function]);
            m_current = m.m_functions.entry[function];
            for (auto p = begin; p < end; ++p)
            {
                if (!view.IsWord(p))
                {
                    continue;
                }

                const auto token = view.TokenAt(p);
                if (model.ResolveToken(token) != kNone || m_symbol_at.contains(token))
                {
                    continue;
                }

                if (p > begin && (view.At(p - 1) == Tok::Dot || view.At(p - 1) == Tok::Arrow ||
                        view.At(p - 1) == Tok::ColonColon))
                {
                    continue;
                }

                const auto name = model.Names().Find(view.Text(p));
                const auto found = name == kNone ? m_locals_by_name.end() : m_locals_by_name.find(name);
                if (found == m_locals_by_name.end())
                {
                    continue;
                }

                for (const auto symbol: found->second)
                {
                    if (m.m_owner[symbol] == function && symbols.decl_token[symbol] < token)
                    {
                        m_events.push_back({EventKind::Escape, symbol, token, m.m_functions.entry[function]});
                    }
                }
            }
        }

        // ---- output -------------------------------------------------------------
        void Finish()
        {
            const auto block_count = m_block_function.size();
            auto &blocks = m.m_blocks;
            auto &events = m.m_events;

            // Events grouped by block, in the order they were emitted.
            std::vector<std::uint32_t> counts(block_count + 1, 0);
            for (const auto &event: m_events)
            {
                ++counts[event.block + 1];
            }

            for (std::size_t b = 0; b < block_count; ++b)
            {
                counts[b + 1] += counts[b];
            }

            std::vector<std::uint32_t> order(m_events.size());
            {
                auto cursor = counts;
                for (std::uint32_t i = 0; i < m_events.size(); ++i)
                {
                    order[cursor[m_events[i].block]++] = i;
                }
            }

            std::vector<std::uint32_t> successor_counts(block_count + 1, 0);
            for (const auto &edge: m_edges)
            {
                ++successor_counts[edge.first + 1];
            }

            for (std::size_t b = 0; b < block_count; ++b)
            {
                successor_counts[b + 1] += successor_counts[b];
            }

            m.m_successors.resize(m_edges.size());
            {
                auto cursor = successor_counts;
                for (const auto &edge: m_edges)
                {
                    m.m_successors[cursor[edge.first]++] = edge.second;
                }
            }

            for (std::size_t b = 0; b < block_count; ++b)
            {
                blocks.function.push_back(m_block_function[b]);
                blocks.first_event.push_back(counts[b]);
                blocks.event_count.push_back(counts[b + 1] - counts[b]);
                blocks.first_succ.push_back(successor_counts[b]);
                blocks.succ_count.push_back(successor_counts[b + 1] - successor_counts[b]);
                blocks.returns.push_back(m_block_returns[b]);
            }

            for (const auto index: order)
            {
                const auto &event = m_events[index];
                events.kind.push_back(event.kind);
                events.symbol.push_back(event.symbol);
                events.token.push_back(event.token);
                events.block.push_back(event.block);
            }

            // Per symbol, in token order.
            const auto symbol_count = symbols.Size();
            auto &begin = m.m_symbol_begin;
            begin.assign(symbol_count + 1, 0);
            for (const auto symbol: events.symbol)
            {
                ++begin[symbol + 1];
            }

            for (std::size_t s = 0; s < symbol_count; ++s)
            {
                begin[s + 1] += begin[s];
            }

            m.m_symbol_events.resize(events.Size());
            {
                std::vector<std::uint32_t> cursor(begin.begin(), begin.end());
                for (std::uint32_t i = 0; i < events.Size(); ++i)
                {
                    m.m_symbol_events[cursor[events.symbol[i]]++] = i;
                }
            }

            for (std::size_t s = 0; s < symbol_count; ++s)
            {
                std::stable_sort(m.m_symbol_events.begin() + begin[s], m.m_symbol_events.begin() + begin[s + 1],
                    [&](std::uint32_t a, std::uint32_t b)
                    {
                        return events.token[a] < events.token[b];
                    });
            }

            // Nodes outside any function have no id.
            for (auto &entry: m.m_node_function)
            {
                if (entry == kUnset)
                {
                    entry = kNone;
                }
            }
        }

        FlowModel &m;
        const TypeModel &types;
        const TypeTable &table;
        const SemanticModel &model;
        const GrammarNodeSoA &nodes;
        const SymbolTable &symbols;
        detail::TokenView view;

        FunctionId m_function = kNone;
        BlockId m_current = kNone;
        BlockId m_exit = kNone;
        std::vector<BlockId> m_breaks;
        std::vector<BlockId> m_continues;
        std::vector<FunctionId> m_block_function;
        std::vector<std::uint8_t> m_block_returns;
        std::vector<std::pair<BlockId, BlockId>> m_edges;
        std::vector<RawEvent> m_events;
        std::unordered_map<std::uint32_t, SymbolId> m_symbol_at;
        std::unordered_map<NameId, std::vector<SymbolId>> m_locals_by_name;
    };

    FlowModel Flow::Build(const TypeModel &types)
    {
        FlowModel model(types);
        FlowBuilder(model).Run();
        return model;
    }

} // namespace heimdall
