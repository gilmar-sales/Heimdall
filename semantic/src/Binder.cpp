#include <Heimdall/SemanticModel.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace heimdall
{

    namespace
    {

        constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
        constexpr std::uint64_t kFnvPrime = 1099511628211ull;

        std::uint64_t HashBytes(std::uint64_t hash, std::string_view text)
        {
            for (const char c: text)
            {
                hash ^= static_cast<unsigned char>(c);
                hash *= kFnvPrime;
            }

            return hash;
        }

        std::uint64_t HashByte(std::uint64_t hash, unsigned char byte)
        {
            hash ^= byte;
            return hash * kFnvPrime;
        }

        std::uint64_t HashU64(std::uint64_t hash, std::uint64_t value)
        {
            for (int shift = 0; shift < 64; shift += 8)
            {
                hash = HashByte(hash, static_cast<unsigned char>(value >> shift));
            }

            return hash;
        }

        bool IsDefiningKind(GrammarKind kind)
        {
            switch (kind)
            {
            case GrammarKind::NamespaceDefinition:
            case GrammarKind::RecordDefinition:
            case GrammarKind::FunctionDefinition:
            case GrammarKind::CompoundStatement:
            case GrammarKind::LoopStatement:
            case GrammarKind::IfStatement:
            case GrammarKind::SwitchStatement:
            case GrammarKind::LambdaExpression:
                return true;
            default:
                return false;
            }
        }

        // Everything the binder derives from a function-like declaration by
        // reading its tokens: the grammar does not name constructors, destructors
        // or operators reliably, so the head is scanned instead.
        struct FunctionHead
        {
            std::uint32_t name_token = kNone;      // last token of the name
            std::uint32_t name_first = kNone;      // first token of the name (`~`, `operator`)
            std::uint32_t open = kNone;            // `(` of the parameter list
            std::uint32_t close = kNone;           // matching `)`
            std::uint32_t qualifier_first = kNone; // first token of `A::B::` or kNone
            bool destructor = false;
            bool is_operator = false;
        };

    } // namespace

    class BinderImpl
    {
    public:
        explicit BinderImpl(SemanticModel &model, const ParseTree &tree) : m_model(model), m_tree(tree) {}

        void Run();

    private:
        // ---- token helpers -------------------------------------------------
        const std::vector<Token> & Tokens() const
        {
            return m_tree.Tokens();
        }
        std::string_view Text(std::uint32_t token) const
        {
            return m_tree.Text(Tokens()[token]);
        }
        Tok TokOf(std::uint32_t token) const
        {
            return Tokens()[token].tok;
        }
        bool IsIdent(std::uint32_t token) const
        {
            return token < Tokens().size() && Tokens()[token].kind == TokenKind::Identifier &&
                Tokens()[token].tok == Tok::None;
        }
        bool Is(std::uint32_t token, Tok tok) const
        {
            return token < Tokens().size() && Tokens()[token].tok == tok;
        }

        // Significant (non-trivia, non-directive) tokens, ascending.
        void BuildSignificant();
        // First significant position at or after raw token `token`.
        std::size_t SigAtOrAfter(std::uint32_t token) const;
        std::uint32_t PrevSig(std::uint32_t token) const;
        std::uint32_t NextSig(std::uint32_t token) const;
        // Significant token positions of a node as [begin, end) in m_sig.
        std::pair<std::size_t, std::size_t> NodeSig(std::uint32_t node) const;
        // Position of the sig token matching the bracket at sig position `pos`.
        std::size_t MatchSig(std::size_t pos, std::size_t end) const;

        // ---- node helpers --------------------------------------------------
        void BuildChildren();
        std::pair<std::uint32_t, std::uint32_t> Children(std::uint32_t node) const
        {
            return {m_child_begin[node], m_child_begin[node + 1]};
        }
        std::uint32_t FindChild(std::uint32_t node, GrammarKind kind) const;
        GrammarKind KindOf(std::uint32_t node) const
        {
            return m_tree.Nodes()[node].kind;
        }
        std::uint32_t DeclaredNameToken(std::uint32_t declarator) const;

        // ---- scopes --------------------------------------------------------
        ScopeId ScopeFor(std::uint32_t node);
        ScopeId ParentScopeFor(std::uint32_t node);
        ScopeId CreateScope(std::uint32_t node, ScopeId parent);
        ScopeId AddScope(ScopeId parent, ScopeKind kind, SymbolId owner, std::uint32_t node);
        ScopeId CreateNamespace(std::uint32_t node, ScopeId parent);
        ScopeId CreateRecord(std::uint32_t node, ScopeId parent);
        ScopeId CreateFunction(std::uint32_t node, ScopeId parent);

        // ---- symbols -------------------------------------------------------
        SymbolId Declare(NameId name, SymbolKind kind, ScopeId scope, std::uint32_t flags,
            std::uint32_t token, std::uint32_t node, ScopeId member_scope, bool register_name);
        void DeclareNode(std::uint32_t node);
        void DeclareDeclaration(std::uint32_t node);
        void DeclareEnumerators(std::uint32_t node);
        void DeclareParameter(std::uint32_t node);
        void DeclareFunction(std::uint32_t node);
        void DeclareUsing(std::uint32_t node);
        FunctionHead ScanHead(std::uint32_t node) const;
        std::uint32_t FlagsFromHead(std::uint32_t node, const FunctionHead &head,
            std::uint64_t & signature) const;
        std::uint64_t ParameterSignature(std::uint32_t node, const FunctionHead &head) const;
        NameId HeadName(const FunctionHead &head);
        bool QualifierSegments(std::uint32_t first, std::uint32_t last_exclusive,
            std::vector<std::uint32_t> & out) const;
        SymbolId ResolveQualifier(std::uint32_t first, std::uint32_t name_token, ScopeId from,
            bool &global) const;

        // ---- resolution ----------------------------------------------------
        void ResolveBases();
        void ResolveRefs();
        SymbolId ResolvePath(std::uint32_t first_token, ScopeId from, std::uint32_t before,
            bool &template_id,
            std::uint32_t & last_token) const;

        SemanticModel &m_model;
        const ParseTree &m_tree;
        std::vector<std::uint32_t> m_sig;
        std::vector<std::uint32_t> m_child_begin;
        std::vector<std::uint32_t> m_child_list;
        std::vector<ScopeId> m_own_scope;
        // class symbols whose bases are still to be recorded: (symbol, node)
        std::vector<std::pair<SymbolId, std::uint32_t>> m_pending_bases;
        std::vector<std::uint32_t> m_scratch;
    };

    // ---------------------------------------------------------------------
    void BinderImpl::BuildSignificant()
    {
        const auto &tokens = Tokens();
        const auto &directives = m_tree.Directives();
        std::size_t directive = 0;
        m_sig.reserve(tokens.size() / 2);
        for (std::size_t i = 0; i < tokens.size(); ++i)
        {
            const auto &token = tokens[i];
            if (token.kind == TokenKind::Whitespace || token.kind == TokenKind::LineComment ||
                token.kind == TokenKind::BlockComment)
            {
                continue;
            }

            while (directive < directives.size() &&
                directives[directive].offset + directives[directive].length <= token.offset)
            {
                ++directive;
            }

            if (directive < directives.size() && directives[directive].offset <= token.offset)
            {
                continue;
            }

            if (m_tree.IsDecorationToken(i))
            {
                continue;
            }

            m_sig.push_back(static_cast<std::uint32_t>(i));
        }
    }

    std::size_t BinderImpl::SigAtOrAfter(std::uint32_t token) const
    {
        return static_cast<std::size_t>(std::lower_bound(m_sig.begin(), m_sig.end(),
            token) - m_sig.begin());
    }

    std::uint32_t BinderImpl::PrevSig(std::uint32_t token) const
    {
        const auto pos = SigAtOrAfter(token);
        return pos == 0 ? kNone : m_sig[pos - 1];
    }

    std::uint32_t BinderImpl::NextSig(std::uint32_t token) const
    {
        const auto pos = std::upper_bound(m_sig.begin(), m_sig.end(), token) - m_sig.begin();
        return static_cast<std::size_t>(pos) < m_sig.size() ? m_sig[static_cast<std::size_t>(pos)] : kNone;
    }

    std::pair<std::size_t, std::size_t> BinderImpl::NodeSig(std::uint32_t node) const
    {
        const auto &n = m_tree.Nodes()[node];
        const auto begin = SigAtOrAfter(n.first_token);
        const auto end = SigAtOrAfter(n.first_token + n.token_count);
        return {begin, std::max(begin, end)};
    }

    std::size_t BinderImpl::MatchSig(std::size_t pos, std::size_t end) const
    {
        const Tok open = TokOf(m_sig[pos]);
        const Tok close = open == Tok::LParen ? Tok::RParen : open == Tok::LBracket ? Tok::RBracket : Tok::RBrace;
        std::size_t depth = 0;
        for (std::size_t i = pos; i < end; ++i)
        {
            const Tok t = TokOf(m_sig[i]);
            if (t == open)
            {
                ++depth;
            }
            else if (t == close && --depth == 0)
            {
                return i;
            }
        }

        return end;
    }

    void BinderImpl::BuildChildren()
    {
        const auto &nodes = m_tree.Nodes();
        const std::size_t count = nodes.size();
        m_child_begin.assign(count + 1, 0);
        for (std::size_t i = 1; i < count; ++i)
        {
            const auto parent = nodes[i].parent;
            if (parent < count && parent != i)
            {
                ++m_child_begin[parent + 1];
            }
        }

        for (std::size_t i = 0; i < count; ++i)
        {
            m_child_begin[i + 1] += m_child_begin[i];
        }

        m_child_list.assign(m_child_begin[count], 0);
        std::vector<std::uint32_t> cursor(m_child_begin.begin(), m_child_begin.end() - 1);
        for (std::size_t i = 1; i < count; ++i)
        {
            const auto parent = nodes[i].parent;
            if (parent < count && parent != i)
            {
                m_child_list[cursor[parent] ++] = static_cast<std::uint32_t>(i);
            }
        }
    }

    std::uint32_t BinderImpl::FindChild(std::uint32_t node, GrammarKind kind) const
    {
        const auto[begin, end] = Children(node);
        for (auto i = begin; i < end; ++i)
        {
            if (KindOf(m_child_list[i]) == kind)
            {
                return m_child_list[i];
            }
        }

        return kNone;
    }

    std::uint32_t BinderImpl::DeclaredNameToken(std::uint32_t declarator) const
    {
        if (declarator == kNone)
        {
            return kNone;
        }

        const auto name = FindChild(declarator, GrammarKind::DeclaredName);
        if (name == kNone)
        {
            return kNone;
        }

        const auto token = m_tree.Nodes()[name].first_token;
        return IsIdent(token) ? token : kNone;
    }

    // ---- scopes ---------------------------------------------------------
    ScopeId BinderImpl::AddScope(ScopeId parent, ScopeKind kind, SymbolId owner, std::uint32_t node)
    {
        auto &scopes = m_model.m_scopes;
        scopes.parent.push_back(parent);
        scopes.kind.push_back(kind);
        scopes.owner.push_back(owner);
        scopes.node.push_back(node);
        return static_cast<ScopeId>(scopes.Size() - 1);
    }

    ScopeId BinderImpl::ScopeFor(std::uint32_t node)
    {
        const auto &nodes = m_tree.Nodes();
        std::vector<std::uint32_t> pending;
        ScopeId base = SemanticModel::TranslationUnitScope;
        std::uint32_t index = node;
        for (std::size_t steps = 0; steps <= nodes.size(); ++steps)
        {
            if (m_own_scope[index] != kNone)
            {
                base = m_own_scope[index];
                break;
            }

            if (IsDefiningKind(nodes[index].kind))
            {
                pending.push_back(index);
            }

            if (index == 0)
            {
                break;
            }

            index = nodes[index].parent < nodes.size() ? nodes[index].parent : 0;
        }

        for (auto it = pending.rbegin(); it != pending.rend(); ++it)
        {
            base = CreateScope(*it, base);
            m_own_scope[*it] = base;
        }

        return base;
    }

    ScopeId BinderImpl::ParentScopeFor(std::uint32_t node)
    {
        const auto parent = m_tree.Nodes()[node].parent;
        return node == 0 || parent >= m_tree.Nodes().size() ? SemanticModel::TranslationUnitScope : ScopeFor(parent);
    }

    ScopeId BinderImpl::CreateScope(std::uint32_t node, ScopeId parent)
    {
        switch (KindOf(node))
        {
        case GrammarKind::NamespaceDefinition:
            return CreateNamespace(node, parent);
        case GrammarKind::RecordDefinition:
            return CreateRecord(node, parent);
        case GrammarKind::FunctionDefinition:
            return CreateFunction(node, parent);
        case GrammarKind::LambdaExpression:
            return AddScope(parent, ScopeKind::Function, kNone, node);
        default:
            return AddScope(parent, ScopeKind::Block, kNone, node);
        }
    }

    ScopeId BinderImpl::CreateNamespace(std::uint32_t node, ScopeId parent)
    {
        const auto[begin, end] = NodeSig(node);
        std::size_t i = begin;
        while (i < end && !Is(m_sig[i], Tok::KwNamespace))
        {
            ++i;
        }

        ++i;
        ScopeId scope = parent;
        bool any = false;
        for (; i < end && !Is(m_sig[i], Tok::LBrace); ++i)
        {
            const auto token = m_sig[i];
            if (Is(token, Tok::LBracket))
            {
                i = MatchSig(i, end);
                continue;
            }

            if (!IsIdent(token))
            {
                continue;
            }

            any = true;
            const NameId name = m_model.m_names.Intern(Text(token));
            SymbolId existing = m_model.LookupLocal(scope, name);
            while (existing != kNone && m_model.m_symbols.kind[existing] != SymbolKind::Namespace)
            {
                existing = m_model.m_symbols.next_same_name[existing];
            }

            if (existing != kNone)
            {
                scope = m_model.m_symbols.member_scope[existing];
                continue;
            }

            const auto symbol = Declare(name, SymbolKind::Namespace, scope, 0, token, node, kNone, true);
            scope = AddScope(scope, ScopeKind::Namespace, symbol, node);
            m_model.m_symbols.member_scope[symbol] = scope;
        }

        return any ? scope : AddScope(parent, ScopeKind::Namespace, kNone, node);
    }

    ScopeId BinderImpl::CreateRecord(std::uint32_t node, ScopeId parent)
    {
        const auto[begin, end] = NodeSig(node);
        if (begin >= end)
        {
            return AddScope(parent, ScopeKind::Class, kNone, node);
        }

        std::size_t brace = begin;
        while (brace < end && !Is(m_sig[brace], Tok::LBrace))
        {
            if (Is(m_sig[brace], Tok::LParen) || Is(m_sig[brace], Tok::LBracket))
            {
                brace = MatchSig(brace, end);
            }

            ++brace;
        }

        std::size_t colon = brace;
        for (std::size_t i = begin; i < brace; ++i)
        {
            if (Is(m_sig[i], Tok::LParen) || Is(m_sig[i], Tok::LBracket))
            {
                i = MatchSig(i, brace);
            }
            else if (Is(m_sig[i], Tok::Colon))
            {
                colon = i;
                break;
            }
        }

        const bool is_enum = Is(m_sig[begin], Tok::KwEnum);
        bool scoped = false;
        std::uint32_t name_token = kNone;
        bool specialization = false;
        int angle = 0;
        for (std::size_t i = begin + 1; i < colon; ++i)
        {
            const auto token = m_sig[i];
            if (Is(token, Tok::LParen) || Is(token, Tok::LBracket))
            {
                i = MatchSig(i, colon);
                continue;
            }

            if (is_enum && (Is(token, Tok::KwClass) || Is(token, Tok::KwStruct)))
            {
                scoped = true;
            }
            else if (Is(token, Tok::Lt))
            {
                if (name_token != kNone && angle == 0)
                {
                    specialization = true;
                }

                ++angle;
            }
            else if (Is(token, Tok::Gt))
            {
                angle = std::max(0, angle - 1);
            }
            else if (Is(token, Tok::Shr))
            {
                angle = std::max(0, angle - 2);
            }
            else if (angle == 0 && IsIdent(token))
            {
                name_token = token;
            }
        }

        const bool template_parent = KindOf(m_tree.Nodes()[node].parent < m_tree.Nodes().size() ?
            m_tree.Nodes()[node].parent : 0) == GrammarKind::TemplateDeclaration;
        SymbolId symbol = kNone;
        if (name_token != kNone && !specialization)
        {
            symbol = Declare(m_model.m_names.Intern(Text(name_token)),
                is_enum ? SymbolKind::Enum : SymbolKind::Class, parent, template_parent ? SymbolFlag::Template : 0,
                name_token, node, kNone, true);
        }

        if (is_enum && !scoped)
        {
            // Unscoped enumerators live in the enclosing scope; no scope of
            // their own. Returning `parent` makes the node transparent.
            return parent;
        }

        const auto scope = AddScope(parent, ScopeKind::Class, symbol, node);
        if (symbol != kNone)
        {
            m_model.m_symbols.member_scope[symbol] = scope;
            if (!is_enum && colon < brace)
            {
                m_pending_bases.emplace_back(symbol, node);
            }
        }

        return scope;
    }

    ScopeId BinderImpl::CreateFunction(std::uint32_t node, ScopeId parent)
    {
        // Out-of-class definitions (`void A::f() {}`) see the members of `A`.
        const auto head = ScanHead(node);
        ScopeId lookup_parent = parent;
        if (head.qualifier_first != kNone && head.name_first != kNone)
        {
            bool global = false;
            const auto owner = ResolveQualifier(head.qualifier_first, head.name_first, parent, global);
            if (owner != kNone && m_model.m_symbols.member_scope[owner] != kNone)
            {
                lookup_parent = m_model.m_symbols.member_scope[owner];
            }
        }

        return AddScope(lookup_parent, ScopeKind::Function, kNone, node);
    }

    // ---- symbols --------------------------------------------------------
    SymbolId BinderImpl::Declare(NameId name, SymbolKind kind, ScopeId scope, std::uint32_t flags,
        std::uint32_t token, std::uint32_t node, ScopeId member_scope, bool register_name)
    {
        auto &symbols = m_model.m_symbols;
        const auto id = static_cast<SymbolId>(symbols.Size());
        symbols.name.push_back(name);
        symbols.scope.push_back(scope);
        symbols.kind.push_back(kind);
        symbols.flags.push_back(flags);
        symbols.decl_token.push_back(token);
        symbols.decl_node.push_back(node);
        symbols.member_scope.push_back(member_scope);
        symbols.signature.push_back(0);
        symbols.first_base.push_back(0);
        symbols.base_count.push_back(0);
        symbols.next_same_name.push_back(kNone);
        if (register_name && name != kNone)
        {
            const std::uint64_t key = (static_cast<std::uint64_t>(scope) << 32) | name;
            const auto[it, inserted] = m_model.m_declared.emplace(key, id);
            if (!inserted)
            {
                auto tail = it->second;
                while (symbols.next_same_name[tail] != kNone)
                {
                    tail = symbols.next_same_name[tail];
                }

                symbols.next_same_name[tail] = id;
            }
        }

        return id;
    }

    FunctionHead BinderImpl::ScanHead(std::uint32_t node) const
    {
        FunctionHead head;
        auto[begin, end] = NodeSig(node);
        if (KindOf(node) == GrammarKind::FunctionDefinition)
        {
            const auto body = FindChild(node, GrammarKind::CompoundStatement);
            if (body != kNone)
            {
                end = std::min(end, SigAtOrAfter(m_tree.Nodes()[body].first_token));
            }
        }

        // First `(` that starts a parameter list: skip attributes and
        // keyword-introduced parenthesized groups.
        std::size_t open = end;
        for (std::size_t i = begin; i < end; ++i)
        {
            const auto token = m_sig[i];
            if (Is(token, Tok::LBracket))
            {
                i = MatchSig(i, end);
                continue;
            }

            if (Is(token, Tok::LBrace) || Is(token, Tok::Semi))
            {
                break;
            }

            if (!Is(token, Tok::LParen))
            {
                continue;
            }

            const auto previous = i > begin ? m_sig[i - 1] : kNone;
            const bool group = previous != kNone &&
                (Is(previous, Tok::KwDecltype) || Is(previous, Tok::KwAlignas) || Is(previous, Tok::KwNoexcept) ||
                Is(previous, Tok::KwRequires) || Is(previous, Tok::KwSizeof) || Is(previous, Tok::KwTypeid) ||
                Is(previous, Tok::KwAsm) || Text(previous) == "__attribute__" || Text(previous) == "__declspec");
            if (group)
            {
                i = MatchSig(i, end);
                continue;
            }

            open = i;
            break;
        }

        if (open >= end || open == begin)
        {
            return head;
        }

        std::size_t name_pos = open - 1;
        if (Is(m_sig[name_pos], Tok::KwOperator))
        {
            // `operator()` has its own parentheses: `operator ( ) ( params )`.
            const auto inner = MatchSig(open, end);
            if (inner + 1 >= end ||!Is(m_sig[inner + 1], Tok::LParen))
            {
                return head;
            }

            head.is_operator = true;
            head.name_first = m_sig[name_pos];
            head.name_token = m_sig[inner];
            open = inner + 1;
        }
        else
        {
            // Operator names: `operator` followed by up to a few tokens.
            std::size_t op = name_pos + 1;
            while (op > begin && !Is(m_sig[op - 1], Tok::KwOperator) && name_pos + 1 - op < 4)
            {
                --op;
            }

            if (op > begin && Is(m_sig[op - 1], Tok::KwOperator))
            {
                head.is_operator = true;
                head.name_first = m_sig[op - 1];
                head.name_token = m_sig[name_pos];
            }
            else if (IsIdent(m_sig[name_pos]))
            {
                head.name_token = m_sig[name_pos];
                head.name_first = head.name_token;
                if (name_pos > begin && Is(m_sig[name_pos - 1], Tok::Tilde))
                {
                    head.destructor = true;
                    head.name_first = m_sig[name_pos - 1];
                }
            }
            else
            {
                return head;
            }
        }

        head.open = m_sig[open];
        const auto close = MatchSig(open, end);
        head.close = close < end ? m_sig[close] : kNone;

        // `A::B::name`: walk back over `ident ::` pairs.
        std::size_t first = SigAtOrAfter(head.name_first);
        std::size_t cursor = first;
        while (cursor >= begin + 2 && Is(m_sig[cursor - 1], Tok::ColonColon) && IsIdent(m_sig[cursor - 2]))
        {
            cursor -= 2;
        }

        if (cursor > begin && Is(m_sig[cursor - 1], Tok::ColonColon))
        {
            --cursor;
        }

        if (cursor < first)
        {
            head.qualifier_first = m_sig[cursor];
        }

        return head;
    }

    NameId BinderImpl::HeadName(const FunctionHead &head)
    {
        if (head.name_token == kNone)
        {
            return kNone;
        }

        if (head.destructor)
        {
            std::string text = "~";
            text += Text(head.name_token);
            return m_model.m_names.InternCopy(text);
        }

        if (head.is_operator)
        {
            std::string text;
            bool previous_word = false;
            for (auto i = SigAtOrAfter(head.name_first); i < m_sig.size() && m_sig[i] <= head.name_token; ++i)
            {
                const bool word = Tokens()[m_sig[i]].kind == TokenKind::Identifier;
                if (word && previous_word)
                {
                    text += ' ';
                }

                text += Text(m_sig[i]);
                previous_word = word;
            }

            return m_model.m_names.InternCopy(text);
        }

        return m_model.m_names.Intern(Text(head.name_token));
    }

    std::uint64_t BinderImpl::ParameterSignature(std::uint32_t node, const FunctionHead &head) const
    {
        const auto declarator = FindChild(node, GrammarKind::Declarator);
        std::uint32_t suffix = declarator == kNone ? kNone : FindChild(declarator,
            GrammarKind::FunctionSuffix);
        if (suffix == kNone || m_tree.Nodes()[suffix].first_token != head.open)
        {
            return 0;
        }

        std::uint64_t hash = kFnvOffset;
        std::size_t parameters = 0;
        const auto[begin, end] = Children(suffix);
        for (auto i = begin; i < end; ++i)
        {
            const auto parameter = m_child_list[i];
            if (KindOf(parameter) != GrammarKind::ParameterDeclaration)
            {
                continue;
            }

            const auto name = DeclaredNameToken(FindChild(parameter, GrammarKind::Declarator));
            const auto[pb, pe] = NodeSig(parameter);
            std::uint64_t part = kFnvOffset;
            std::size_t kept = 0;
            std::string_view only;
            for (auto k = pb; k < pe; ++k)
            {
                const auto token = m_sig[k];
                if (Is(token, Tok::Eq))
                {
                    break;
                }

                if (token == name)
                {
                    continue;
                }

                part = HashByte(HashBytes(part, Text(token)), 0x1F);
                only = Text(token);
                ++kept;
            }

            if (kept == 1 && only == "void")
            {
                continue;
            }

            ++parameters;
            hash = HashByte(HashU64(hash, part), 0x1E);
        }

        return HashU64(hash, parameters);
    }

    std::uint32_t BinderImpl::FlagsFromHead(std::uint32_t node, const FunctionHead &head,
        std::uint64_t & signature) const
    {
        std::uint32_t flags = 0;
        auto[begin, end] = NodeSig(node);
        bool has_body = false;
        if (KindOf(node) == GrammarKind::FunctionDefinition)
        {
            const auto body = FindChild(node, GrammarKind::CompoundStatement);
            if (body != kNone)
            {
                has_body = true;
                end = std::min(end, SigAtOrAfter(m_tree.Nodes()[body].first_token));
            }
        }

        if (has_body)
        {
            flags |= SymbolFlag::Definition;
        }

        const auto name_pos = SigAtOrAfter(head.name_first);
        for (std::size_t i = begin; i < name_pos && i < end; ++i)
        {
            const auto token = m_sig[i];
            switch (TokOf(token))
            {
            case Tok::KwVirtual:
                flags |= SymbolFlag::Virtual;
                break;
            case Tok::KwStatic:
                flags |= SymbolFlag::Static;
                break;
            case Tok::KwFriend:
                flags |= SymbolFlag::Friend;
                break;
            default:
                break;
            }
        }

        if (head.destructor)
        {
            flags |= SymbolFlag::Destructor;
        }

        if (head.is_operator)
        {
            flags |= SymbolFlag::Operator;
        }

        std::uint64_t qualifiers = 0;
        const auto close_pos = head.close == kNone ? end : SigAtOrAfter(head.close) + 1;
        for (std::size_t i = close_pos; i < end; ++i)
        {
            const auto token = m_sig[i];
            const Tok tok = TokOf(token);
            if (tok == Tok::Semi || tok == Tok::LBrace || tok == Tok::Colon || tok == Tok::Arrow ||
                tok == Tok::KwRequires || tok == Tok::KwTry)
            {
                break;
            }

            if (tok == Tok::Eq)
            {
                const auto next = i + 1 < end ? m_sig[i + 1] : kNone;
                if (next != kNone && Text(next) == "0")
                {
                    flags |= SymbolFlag::Pure;
                }
                else if (next != kNone && (Is(next, Tok::KwDefault) || Is(next, Tok::KwDelete)))
                {
                    flags |= SymbolFlag::Defaulted;
                }

                break;
            }

            switch (tok)
            {
            case Tok::KwConst:
                flags |= SymbolFlag::Const;
                qualifiers |= 1;
                break;
            case Tok::KwVolatile:
                qualifiers |= 2;
                break;
            case Tok::Amp:
                flags |= SymbolFlag::RefQualified;
                qualifiers |= 4;
                break;
            case Tok::AmpAmp:
                flags |= SymbolFlag::RefQualified;
                qualifiers |= 8;
                break;
            case Tok::KwOverride:
                flags |= SymbolFlag::Override;
                break;
            case Tok::KwFinal:
                flags |= SymbolFlag::Final;
                break;
            case Tok::KwNoexcept:
                if (i + 1 < end && Is(m_sig[i + 1], Tok::LParen))
                {
                    i = MatchSig(i + 1, end);
                }

                break;
            default:
                break;
            }
        }

        const auto parameters = ParameterSignature(node, head);
        signature = parameters == 0 ? 0 : HashU64(parameters, qualifiers);
        return flags;
    }

    bool BinderImpl::QualifierSegments(std::uint32_t first, std::uint32_t last_exclusive,
        std::vector<std::uint32_t> & out) const
    {
        out.clear();
        for (auto i = SigAtOrAfter(first); i < m_sig.size() && m_sig[i] < last_exclusive; ++i)
        {
            const auto token = m_sig[i];
            if (Is(token, Tok::ColonColon))
            {
                continue;
            }

            if (!IsIdent(token))
            {
                return false;
            }

            out.push_back(token);
        }

        return true;
    }

    SymbolId BinderImpl::ResolveQualifier(std::uint32_t first, std::uint32_t name_token, ScopeId from,
        bool &global) const
    {
        std::vector<std::uint32_t> segments;
        global = Is(first, Tok::ColonColon);
        if (!QualifierSegments(first, name_token, segments) || segments.empty())
        {
            return kNone;
        }

        SymbolId current = kNone;
        for (std::size_t i = 0; i < segments.size(); ++i)
        {
            const auto name = m_model.m_names.Find(Text(segments[i]));
            if (name == kNone)
            {
                return kNone;
            }

            if (i == 0)
            {
                current = global ? m_model.LookupLocal(SemanticModel::TranslationUnitScope, name)
                : m_model.Lookup(from, name, segments[i]);
            }
            else
            {
                current = current == kNone ? kNone : m_model.LookupMember(current, name);
            }

            if (current == kNone)
            {
                return kNone;
            }

            const auto kind = m_model.m_symbols.kind[current];
            if (kind != SymbolKind::Namespace && kind != SymbolKind::Class && kind != SymbolKind::Enum)
            {
                return kNone;
            }
        }

        return current;
    }

    void BinderImpl::DeclareFunction(std::uint32_t node)
    {
        const auto head = ScanHead(node);
        if (head.name_token == kNone)
        {
            return;
        }

        const auto scope = ParentScopeFor(node);
        std::uint64_t signature = 0;
        std::uint32_t flags = FlagsFromHead(node, head, signature);
        const auto parent = m_tree.Nodes()[node].parent;
        if (parent < m_tree.Nodes().size() && KindOf(parent) == GrammarKind::TemplateDeclaration)
        {
            flags |= SymbolFlag::Template;
        }

        const bool qualified = head.qualifier_first != kNone;
        if (qualified)
        {
            flags |= SymbolFlag::Qualified;
        }

        // Constructors repeat the class name (the symbol of the owning scope).
        if (!head.destructor && !head.is_operator && !qualified && m_model.m_scopes.kind[scope] == ScopeKind::Class)
        {
            const auto owner = m_model.m_scopes.owner[scope];
            if (owner != kNone && m_model.m_symbols.name[owner] == m_model.m_names.Find(Text(head.name_token)))
            {
                flags |= SymbolFlag::Constructor;
            }
        }

        if ((flags & (SymbolFlag::Constructor | SymbolFlag::Destructor)) != 0)
        {
            signature = 0;
        }

        const auto id = Declare(HeadName(head), SymbolKind::Function, scope, flags, head.name_token, node,
            kNone,
            !qualified);
        m_model.m_symbols.signature[id] = signature;
    }

    void BinderImpl::DeclareParameter(std::uint32_t node)
    {
        // Only parameters of a definition are visible (in the function's scope).
        std::uint32_t current = m_tree.Nodes()[node].parent;
        for (std::size_t steps = 0; steps < 8 && current < m_tree.Nodes().size(); ++steps)
        {
            const auto kind = KindOf(current);
            if (kind == GrammarKind::FunctionSuffix || kind == GrammarKind::Declarator)
            {
                current = m_tree.Nodes()[current].parent;
                continue;
            }

            if (kind != GrammarKind::FunctionDefinition && kind != GrammarKind::LambdaExpression)
            {
                return;
            }

            const auto name = DeclaredNameToken(FindChild(node, GrammarKind::Declarator));
            if (name == kNone)
            {
                return;
            }

            Declare(m_model.m_names.Intern(Text(name)), SymbolKind::Parameter, ScopeFor(current), 0, name, node,
                kNone, true);
            return;
        }
    }

    void BinderImpl::DeclareDeclaration(std::uint32_t node)
    {
        const auto[first, last] = NodeSig(node);
        const bool typedef_declaration = first < last && Is(m_sig[first], Tok::KwTypedef);
        std::uint32_t flags = 0;
        for (auto i = first; i < last; ++i)
        {
            if (Is(m_sig[i], Tok::KwStatic))
            {
                flags |= SymbolFlag::Static;
            }
        }

        const auto scope = ScopeFor(node);
        const auto[begin, end] = Children(node);
        for (auto i = begin; i < end; ++i)
        {
            const auto child = m_child_list[i];
            std::uint32_t declarator = kNone;
            if (KindOf(child) == GrammarKind::InitDeclarator)
            {
                declarator = FindChild(child, GrammarKind::Declarator);
            }
            else if (KindOf(child) == GrammarKind::Declarator)
            {
                declarator = child;
            }

            if (declarator == kNone || FindChild(declarator, GrammarKind::FunctionSuffix) != kNone)
            {
                continue;
            }

            const auto name = DeclaredNameToken(declarator);
            if (name == kNone)
            {
                continue;
            }

            Declare(m_model.m_names.Intern(Text(name)),
                typedef_declaration ? SymbolKind::TypeAlias : SymbolKind::Variable, scope, flags, name, node,
                kNone, true);
        }
    }

    void BinderImpl::DeclareEnumerators(std::uint32_t node)
    {
        const auto name = DeclaredNameToken(node);
        const auto scope = ScopeFor(node);
        if (name != kNone)
        {
            Declare(m_model.m_names.Intern(Text(name)), SymbolKind::Enumerator, scope, 0, name, node, kNone,
                true);
            return;
        }
    }

    void BinderImpl::DeclareUsing(std::uint32_t node)
    {
        const auto[begin, end] = NodeSig(node);
        // `using Name = ...;` (but not `using namespace` or `using A::b;`).
        if (begin + 2 < end && Is(m_sig[begin], Tok::KwUsing) && IsIdent(m_sig[begin + 1]) &&
            Is(m_sig[begin + 2], Tok::Eq))
        {
            const auto token = m_sig[begin + 1];
            Declare(m_model.m_names.Intern(Text(token)), SymbolKind::TypeAlias, ScopeFor(node), 0, token, node,
                kNone, true);
        }
    }

    void BinderImpl::DeclareNode(std::uint32_t node)
    {
        switch (KindOf(node))
        {
        case GrammarKind::FunctionDeclaration:
        case GrammarKind::FunctionDefinition:
            DeclareFunction(node);
            break;
        case GrammarKind::Declaration:
        case GrammarKind::DeclarationStatement:
            DeclareDeclaration(node);
            break;
        case GrammarKind::ParameterDeclaration:
            DeclareParameter(node);
            break;
        case GrammarKind::Enumerator:
            DeclareEnumerators(node);
            break;
        case GrammarKind::UsingDeclaration:
            DeclareUsing(node);
            break;
        default:
            break;
        }
    }

    // ---- resolution -----------------------------------------------------
    // Reads `[::] a [:: b ...]` starting at `first_token` and resolves it.
    // `template_id` is set when the path continues with `<` (a template-id).
    SymbolId BinderImpl::ResolvePath(std::uint32_t first_token, ScopeId from, std::uint32_t before,
        bool &template_id, std::uint32_t & last_token) const
    {
        template_id = false;
        std::size_t pos = SigAtOrAfter(first_token);
        const bool global = pos < m_sig.size() && Is(m_sig[pos], Tok::ColonColon);
        if (global)
        {
            ++pos;
        }

        SymbolId current = kNone;
        bool first = true;
        while (pos < m_sig.size() && IsIdent(m_sig[pos]))
        {
            const auto token = m_sig[pos];
            last_token = token;
            const auto name = m_model.m_names.Find(Text(token));
            SymbolId found = kNone;
            if (name != kNone)
            {
                if (first)
                {
                    found = global ? m_model.LookupLocal(SemanticModel::TranslationUnitScope, name)
                    : m_model.Lookup(from, name, before);
                }
                else if (current != kNone)
                {
                    found = m_model.LookupMember(current, name);
                }
            }

            current = found;
            first = false;
            ++pos;
            if (pos < m_sig.size() && Is(m_sig[pos], Tok::Lt))
            {
                template_id = true;
                return kNone;
            }

            if (pos < m_sig.size() && Is(m_sig[pos], Tok::ColonColon))
            {
                ++pos;
                continue;
            }

            break;
        }

        return current;
    }

    void BinderImpl::ResolveBases()
    {
        auto &bases = m_model.m_bases;
        auto &symbols = m_model.m_symbols;
        for (const auto &[symbol, node]: m_pending_bases)
        {
            const auto[begin, end] = NodeSig(node);
            std::size_t brace = begin;
            while (brace < end && !Is(m_sig[brace], Tok::LBrace))
            {
                if (Is(m_sig[brace], Tok::LParen) || Is(m_sig[brace], Tok::LBracket))
                {
                    brace = MatchSig(brace, end);
                }

                ++brace;
            }

            std::size_t colon = begin;
            while (colon < brace && !Is(m_sig[colon], Tok::Colon))
            {
                if (Is(m_sig[colon], Tok::LParen) || Is(m_sig[colon], Tok::LBracket))
                {
                    colon = MatchSig(colon, brace);
                }

                ++colon;
            }

            symbols.first_base[symbol] = static_cast<std::uint32_t>(bases.derived.size());
            std::size_t i = colon + 1;
            while (i < brace)
            {
                // One base-specifier up to the next top-level comma.
                std::size_t stop = i;
                int angle = 0;
                while (stop < brace && !(angle == 0 && Is(m_sig[stop], Tok::Comma)))
                {
                    if (Is(m_sig[stop], Tok::Lt))
                    {
                        ++angle;
                    }
                    else if (Is(m_sig[stop], Tok::Gt))
                    {
                        angle = std::max(0, angle - 1);
                    }
                    else if (Is(m_sig[stop], Tok::Shr))
                    {
                        angle = std::max(0, angle - 2);
                    }

                    ++stop;
                }

                std::size_t path = i;
                while (path < stop && (Is(m_sig[path], Tok::KwPublic) || Is(m_sig[path], Tok::KwPrivate) ||
                    Is(m_sig[path], Tok::KwProtected) || Is(m_sig[path], Tok::KwVirtual)))
                {
                    ++path;
                }

                if (path < stop && (IsIdent(m_sig[path]) || Is(m_sig[path], Tok::ColonColon)))
                {
                    bool template_id = false;
                    std::uint32_t last = kNone;
                    auto target = ResolvePath(m_sig[path], symbols.scope[symbol], symbols.decl_token[symbol],
                        template_id, last);
                    if (template_id || target == kNone || symbols.kind[target] != SymbolKind::Class ||
                        target == symbol)
                    {
                        target = kNone;
                    }

                    bases.derived.push_back(symbol);
                    bases.name.push_back(last == kNone ? kNone : m_model.m_names.Intern(Text(last)));
                    bases.token.push_back(m_sig[path]);
                    bases.target.push_back(target);
                }

                i = stop + 1;
            }

            symbols.base_count[symbol] =
                static_cast<std::uint32_t>(bases.derived.size()) - symbols.first_base[symbol];
        }
    }

    void BinderImpl::ResolveRefs()
    {
        const auto &nodes = m_tree.Nodes();
        std::vector<std::pair<std::uint32_t, SymbolId>> found;
        for (std::uint32_t node = 0; node < nodes.size(); ++node)
        {
            if (nodes[node].kind != GrammarKind::IdentifierExpression)
            {
                continue;
            }

            const auto[begin, end] = NodeSig(node);
            if (end != begin + 1 ||!IsIdent(m_sig[begin]))
            {
                continue;
            }

            const auto token = m_sig[begin];
            const auto name = m_model.m_names.Find(Text(token));
            SymbolId target = kNone;
            const auto previous = begin > 0 ? m_sig[begin - 1] : kNone;
            if (name != kNone && !(previous != kNone &&
                (Is(previous, Tok::Dot) || Is(previous, Tok::Arrow) || Is(previous, Tok::DotStar) ||
                Is(previous, Tok::ArrowStar))))
            {
                if (previous != kNone && Is(previous, Tok::ColonColon))
                {
                    // Qualified use: find where the path starts.
                    std::size_t first = begin;
                    while (first >= 2 && Is(m_sig[first - 1], Tok::ColonColon) && IsIdent(m_sig[first - 2]))
                    {
                        first -= 2;
                    }

                    if (first >= 1 && Is(m_sig[first - 1], Tok::ColonColon))
                    {
                        --first;
                    }

                    bool template_id = false;
                    std::uint32_t last = kNone;
                    target = ResolvePath(m_sig[first], ScopeFor(node), token, template_id, last);
                    if (last != token)
                    {
                        target = kNone;
                    }
                }
                else
                {
                    target = m_model.Lookup(ScopeFor(node), name, token);
                }
            }

            found.emplace_back(token, target);
        }

        std::sort(found.begin(), found.end());
        found.erase(std::unique(found.begin(), found.end(),
            [](const auto &a, const auto &b)
            {
                return a.first == b.first;
        }),
            found.end());
        auto &refs = m_model.m_refs;
        refs.token.reserve(found.size());
        refs.target.reserve(found.size());
        for (const auto &[token, target]: found)
        {
            refs.token.push_back(token);
            refs.target.push_back(target);
        }
    }

    void BinderImpl::Run()
    {
        const auto &nodes = m_tree.Nodes();
        BuildSignificant();
        BuildChildren();
        m_own_scope.assign(nodes.size(), kNone);

        // Scope 0 is the translation unit.
        AddScope(kNone, ScopeKind::TranslationUnit, kNone, 0);
        m_own_scope[0] = SemanticModel::TranslationUnitScope;

        // Namespaces, classes and function scopes first, so declarations can
        // find their scopes in any node order.
        for (std::uint32_t node = 1; node < nodes.size(); ++node)
        {
            if (IsDefiningKind(nodes[node].kind))
            {
                ScopeFor(node);
            }
        }

        for (std::uint32_t node = 1; node < nodes.size(); ++node)
        {
            DeclareNode(node);
        }

        ResolveBases();
        ResolveRefs();
    }

    SemanticModel Binder::Bind(const ParseTree &tree)
    {
        const auto nodes = tree.Nodes().size();
        SemanticModel model(tree, std::max<std::size_t>(64 * 1024, nodes * 96));
        auto &symbols = model.m_symbols;
        const auto expected = nodes / 6 + 8;
        symbols.name.reserve(expected);
        symbols.scope.reserve(expected);
        symbols.kind.reserve(expected);
        symbols.flags.reserve(expected);
        symbols.decl_token.reserve(expected);
        symbols.decl_node.reserve(expected);
        symbols.member_scope.reserve(expected);
        symbols.signature.reserve(expected);
        symbols.first_base.reserve(expected);
        symbols.base_count.reserve(expected);
        symbols.next_same_name.reserve(expected);
        model.m_scopes.parent.reserve(expected);
        model.m_scopes.kind.reserve(expected);
        model.m_scopes.owner.reserve(expected);
        model.m_scopes.node.reserve(expected);
        BinderImpl(model, tree).Run();
        return model;
    }

} // namespace heimdall
