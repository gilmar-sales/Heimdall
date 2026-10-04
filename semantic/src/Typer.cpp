#include <Heimdall/TypeModel.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string>

namespace heimdall
{

    // ---- TypeTable ----------------------------------------------------------
    TypeTable::TypeTable(std::pmr::memory_resource *resource)
    : m_kind(resource), m_arg(resource), m_extent(resource), m_index(resource)
    {
        // Id 0 is Unknown.
        m_kind.push_back(TypeKind::Unknown);
        m_arg.push_back(kNone);
        m_extent.push_back(kNone);
    }

    TypeId TypeTable::Intern(TypeKind kind, std::uint32_t arg, std::uint32_t extent)
    {
        const Key key {arg, extent, kind};
        if (const auto found = m_index.find(key); found != m_index.end())
        {
            return found->second;
        }

        const auto id = static_cast<TypeId>(m_kind.size());
        m_kind.push_back(kind);
        m_arg.push_back(arg);
        m_extent.push_back(extent);
        m_index.emplace(key, id);
        return id;
    }

    TypeId TypeTable::Builtin(BuiltinType type)
    {
        return Intern(TypeKind::Builtin, static_cast<std::uint32_t>(type));
    }

    TypeId TypeTable::Class(SymbolId symbol)
    {
        return symbol == kNone ? Unknown : Intern(TypeKind::Class, symbol);
    }

    TypeId TypeTable::Enum(SymbolId symbol)
    {
        return symbol == kNone ? Unknown : Intern(TypeKind::Enum, symbol);
    }

    TypeId TypeTable::External(NameId name)
    {
        return name == kNone ? Unknown : Intern(TypeKind::External, name);
    }

    // A pointer to an unknown type is still known to be a pointer.
    TypeId TypeTable::Pointer(TypeId pointee)
    {
        return Intern(TypeKind::Pointer, pointee);
    }

    TypeId TypeTable::LRef(TypeId referee)
    {
        const auto inner = Value(referee);
        return inner == Unknown ? Unknown : Intern(TypeKind::LRef, inner);
    }

    TypeId TypeTable::RRef(TypeId referee)
    {
        if (Kind(referee) == TypeKind::LRef)
        {
            return referee;
        }

        const auto inner = Value(referee);
        return inner == Unknown ? Unknown : Intern(TypeKind::RRef, inner);
    }

    TypeId TypeTable::Const(TypeId type)
    {
        switch (Kind(type))
        {
        case TypeKind::Unknown:
        case TypeKind::Const:
        case TypeKind::LRef:
        case TypeKind::RRef:
            return type;
        case TypeKind::Array:
            return Array(Const(Arg(type)), Extent(type));
        default:
            return Intern(TypeKind::Const, type);
        }
    }

    TypeId TypeTable::Array(TypeId element, std::uint32_t extent)
    {
        return Intern(TypeKind::Array, element, extent);
    }

    TypeId TypeTable::Value(TypeId type) const noexcept
    {
        while (Kind(type) == TypeKind::LRef || Kind(type) == TypeKind::RRef)
        {
            type = Arg(type);
        }

        return type;
    }

    TypeId TypeTable::Strip(TypeId type) const noexcept
    {
        type = Value(type);
        return Kind(type) == TypeKind::Const ? Arg(type) : type;
    }

    TypeId TypeTable::Decay(TypeId type)
    {
        return Kind(type) == TypeKind::Array ? Pointer(Arg(type)) : type;
    }

    bool TypeTable::IsInteger(TypeId type) const noexcept
    {
        if (Kind(type) != TypeKind::Builtin)
        {
            return false;
        }

        const auto arg = Arg(type);
        return arg >= static_cast<std::uint32_t>(BuiltinType::Char) && arg <= static_cast<std::uint32_t>(BuiltinType::SizeT);
    }

    bool TypeTable::IsFloating(TypeId type) const noexcept
    {
        if (Kind(type) != TypeKind::Builtin)
        {
            return false;
        }

        const auto arg = Arg(type);
        return arg >= static_cast<std::uint32_t>(BuiltinType::Float) && arg <= static_cast<std::uint32_t>(BuiltinType::LongDouble);
    }

    bool TypeTable::IsArithmetic(TypeId type) const noexcept
    {
        return IsBool(type) || IsInteger(type) || IsFloating(type);
    }

    bool TypeTable::IsConstQualified(TypeId type) const noexcept
    {
        type = Value(type);
        for (std::size_t steps = 0; steps < 16; ++steps)
        {
            if (Kind(type) == TypeKind::Const)
            {
                return true;
            }

            if (Kind(type) != TypeKind::Array)
            {
                return false;
            }

            type = Arg(type);
        }

        return false;
    }

    // ---- TypeModel ----------------------------------------------------------
    TypeModel::TypeModel(const SemanticModel &model, std::size_t arena_hint)
    : m_arena(std::make_unique<Arena>(arena_hint)), m_model(&model), m_externals(m_arena->Resource()),
        m_types(m_arena->Resource()), m_symbol_type(m_arena->Resource()), m_node_type(m_arena->Resource()) {}

    std::string TypeModel::Spell(TypeId type) const
    {
        static constexpr std::array<std::string_view, 22> kBuiltin {"void", "decltype(nullptr)", "bool", "char",
            "signed char", "unsigned char", "wchar_t", "char8_t", "char16_t", "char32_t", "short", "unsigned short",
            "int", "unsigned int", "long", "unsigned long", "long long", "unsigned long long", "size_t", "float",
            "double", "long double"};
        switch (m_types.Kind(type))
        {
        case TypeKind::Builtin:
            return std::string(kBuiltin[std::min<std::size_t>(m_types.Arg(type), kBuiltin.size() - 1)]);
        case TypeKind::Class:
        case TypeKind::Enum:
            return std::string(m_model->Names().Text(m_model->Symbols().name[m_types.Arg(type)]));
        case TypeKind::External:
            return std::string(m_externals.Text(m_types.Arg(type)));
        case TypeKind::Pointer:
        {
            // A pointer to an array reads `int(*)[2]`.
            if (m_types.Kind(m_types.Arg(type)) == TypeKind::Array)
            {
                std::string dimensions;
                auto element = m_types.Arg(type);
                for (; m_types.Kind(element) == TypeKind::Array; element = m_types.Arg(element))
                {
                    dimensions += "[" + (m_types.Extent(element) == kNone ? std::string() : std::to_string(m_types.Extent(element))) + "]";
                }

                return Spell(element) + "(*)" + dimensions;
            }

            return Spell(m_types.Arg(type)) + "*";
        }
        case TypeKind::LRef:
            return Spell(m_types.Arg(type)) + "&";
        case TypeKind::RRef:
            return Spell(m_types.Arg(type)) + "&&";
        case TypeKind::Const:
            return m_types.Kind(m_types.Arg(type)) == TypeKind::Pointer ? Spell(m_types.Arg(type)) + " const"
                                                                         : "const " + Spell(m_types.Arg(type));
        case TypeKind::Array:
        {
            // Outermost dimension first: `int[2][3]` is an array of 2 arrays of 3 ints.
            std::string dimensions;
            auto element = type;
            for (; m_types.Kind(element) == TypeKind::Array; element = m_types.Arg(element))
            {
                dimensions += "[" + (m_types.Extent(element) == kNone ? std::string() : std::to_string(m_types.Extent(element))) + "]";
            }

            return Spell(element) + dimensions;
        }
        default:
            return "<unknown>";
        }
    }

    std::string_view TypeModel::ExternalHead(TypeId type) const
    {
        if (m_types.Kind(type) != TypeKind::External)
        {
            return {};
        }

        const auto text = m_externals.Text(m_types.Arg(type));
        return text.substr(0, text.find('<'));
    }

    // ---- Typer --------------------------------------------------------------
    namespace
    {

        bool IsStringHead(std::string_view head)
        {
            return head == "std::string" || head == "std::wstring" || head == "std::basic_string" ||
                head == "std::string_view" || head == "std::wstring_view" || head == "std::basic_string_view" ||
                head == "std::u8string" || head == "std::u16string" || head == "std::u32string";
        }

    } // namespace

    bool IsStdContainerHead(std::string_view head)
    {
        static constexpr std::array<std::string_view, 15> kHeads {"std::vector", "std::deque", "std::list",
            "std::array", "std::span", "std::map", "std::set", "std::multimap", "std::multiset",
            "std::unordered_map", "std::unordered_set", "std::unordered_multimap", "std::unordered_multiset",
            "std::forward_list", "std::flat_map"};
        return IsStringHead(head) || std::find(kHeads.begin(), kHeads.end(), head) != kHeads.end();
    }

    bool IsStdIndexableHead(std::string_view head)
    {
        return head == "std::vector" || head == "std::deque" || head == "std::array" || head == "std::span" ||
            IsStringHead(head);
    }

    namespace
    {

        constexpr TypeId kPending = ~0u;
        constexpr std::size_t kMaxDepth = 192;

        bool IsWordChar(char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
        }

        void Append(std::string &out, std::string_view piece)
        {
            if (!out.empty() && !piece.empty() && IsWordChar(out.back()) && IsWordChar(piece.front()))
            {
                out += ' ';
            }

            out += piece;
        }

        bool IsBuiltinWord(Tok tok)
        {
            switch (tok)
            {
            case Tok::KwSigned:
            case Tok::KwUnsigned:
            case Tok::KwShort:
            case Tok::KwLong:
            case Tok::KwInt:
            case Tok::KwChar:
            case Tok::KwBool:
            case Tok::KwFloat:
            case Tok::KwDouble:
            case Tok::KwVoid:
            case Tok::KwWchar:
            case Tok::KwChar8:
            case Tok::KwChar16:
            case Tok::KwChar32:
                return true;
            default:
                return false;
            }
        }

        // Specifiers that do not change the type of the declared entity.
        bool IsIgnoredSpecifier(Tok tok)
        {
            switch (tok)
            {
            case Tok::KwStatic:
            case Tok::KwExtern:
            case Tok::KwConstexpr:
            case Tok::KwConstinit:
            case Tok::KwConsteval:
            case Tok::KwInline:
            case Tok::KwThreadLocal:
            case Tok::KwRegister:
            case Tok::KwMutable:
            case Tok::KwVirtual:
            case Tok::KwExplicit:
            case Tok::KwFriend:
            case Tok::KwVolatile:
            case Tok::KwTypedef:
                return true;
            default:
                return false;
            }
        }

        bool IsAssignment(Tok tok)
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

        int DigitValue(char c)
        {
            if (c >= '0' && c <= '9')
            {
                return c - '0';
            }

            if (c >= 'a' && c <= 'f')
            {
                return c - 'a' + 10;
            }

            if (c >= 'A' && c <= 'F')
            {
                return c - 'A' + 10;
            }

            return -1;
        }

        // Rank for the usual arithmetic conversions.
        int Rank(BuiltinType type)
        {
            switch (type)
            {
            case BuiltinType::Int:
            case BuiltinType::UInt:
                return 1;
            case BuiltinType::Long:
            case BuiltinType::ULong:
                return 2;
            default:
                return 3;
            }
        }

        bool IsUnsignedInt(BuiltinType type)
        {
            return type == BuiltinType::UInt || type == BuiltinType::ULong || type == BuiltinType::ULongLong;
        }

        std::optional<BuiltinType> Promote(BuiltinType type)
        {
            switch (type)
            {
            case BuiltinType::Bool:
            case BuiltinType::Char:
            case BuiltinType::SChar:
            case BuiltinType::UChar:
            case BuiltinType::Short:
            case BuiltinType::UShort:
                return BuiltinType::Int;
            case BuiltinType::WChar:
            case BuiltinType::Char8:
            case BuiltinType::Char16:
            case BuiltinType::Char32:
                return std::nullopt;
            default:
                return type;
            }
        }

    } // namespace

    class TyperImpl
    {
    public:
        TyperImpl(TypeModel &out, const SemanticModel &model)
        : m_out(out), m_model(model), m_tree(model.Tree()), m_types(out.m_types), m_sig(model.Significant()),
            m_symbols(model.Symbols()), m_nodes(model.Tree().NodesSoA())
        {
        }

        void Run();

    private:
        struct Base
        {
            TypeId type = TypeTable::Unknown;
            std::size_t next = 0;
            bool ok = false;
            bool is_auto = false;
            bool is_const = false;
        };

        // ---- token helpers ---------------------------------------------------
        const std::vector<Token> &Tokens() const
        {
            return m_tree.Tokens();
        }
        std::string_view Text(std::uint32_t token) const
        {
            return m_tree.Text(Tokens()[token]);
        }
        std::size_t Pos(std::uint32_t token) const
        {
            return static_cast<std::size_t>(std::lower_bound(m_sig.begin(), m_sig.end(), token) - m_sig.begin());
        }
        Tok At(std::size_t pos) const
        {
            return pos < m_sig.size() ? Tokens()[m_sig[pos]].tok : Tok::None;
        }
        bool WordAt(std::size_t pos) const
        {
            return pos < m_sig.size() && Tokens()[m_sig[pos]].kind == TokenKind::Identifier &&
                Tokens()[m_sig[pos]].tok == Tok::None;
        }
        std::string_view TextAt(std::size_t pos) const
        {
            return pos < m_sig.size() ? Text(m_sig[pos]) : std::string_view {};
        }
        std::pair<std::size_t, std::size_t> NodeSig(std::uint32_t node) const
        {
            const auto n = m_nodes[node];
            const auto begin = Pos(n.GetFirstToken());
            return {begin, std::max(begin, Pos(n.GetFirstToken() + n.GetTokenCount()))};
        }
        std::size_t MatchParen(std::size_t open, std::size_t limit) const;
        std::size_t MatchAngle(std::size_t open, std::size_t limit) const;
        std::uint32_t FindChild(std::uint32_t node, GrammarKind kind) const
        {
            for (const auto child: m_model.ChildrenOf(node))
            {
                if (m_nodes.Kind(child) == kind)
                {
                    return child;
                }
            }

            return kNone;
        }
        bool SameStart(std::uint32_t a, std::uint32_t b) const
        {
            return Pos(m_nodes.FirstToken(a)) == Pos(m_nodes.FirstToken(b));
        }

        // ---- declared types --------------------------------------------------
        TypeId SymbolType(SymbolId symbol);
        TypeId ComputeSymbolType(SymbolId symbol);
        TypeId VariableType(SymbolId symbol);
        TypeId ReturnType(SymbolId symbol);
        TypeId AliasTarget(SymbolId symbol);
        TypeId OverloadReturn(SymbolId symbol);
        Base ParseBase(std::size_t begin, std::size_t end, ScopeId scope, std::uint32_t before);
        TypeId ParseName(std::size_t &pos, std::size_t end, ScopeId scope, std::uint32_t before);
        TypeId ApplyOperators(TypeId type, std::size_t begin, std::size_t end);
        TypeId ParseTypeTokens(std::size_t begin, std::size_t end, ScopeId scope, std::uint32_t before);
        TypeId DeclaratorType(Base base, std::uint32_t declarator, SymbolKind kind, TypeId init_type);
        TypeId NumberType(std::string_view text);
        TypeId CharType(std::string_view text);

        // ---- expressions -----------------------------------------------------
        TypeId NodeType(std::uint32_t node);
        TypeId ComputeNodeType(std::uint32_t node);
        TypeId LiteralType(std::uint32_t node);
        TypeId IdentifierType(std::uint32_t node);
        TypeId UnaryType(std::uint32_t node);
        TypeId BinaryType(std::uint32_t node);
        TypeId ConditionalType(std::uint32_t node);
        TypeId CallType(std::uint32_t node);
        TypeId SubscriptType(std::uint32_t node);
        TypeId MemberType(std::uint32_t node, bool as_call);
        TypeId CastType(std::uint32_t callee);
        TypeId ExternalCall(TypeId object, std::string_view member);
        TypeId Arithmetic(TypeId a, TypeId b);
        TypeId Promoted(TypeId type);
        bool IsScalar(TypeId type) const;
        ScopeId EnclosingClass(ScopeId scope) const;

        TypeModel &m_out;
        const SemanticModel &m_model;
        const ParseTree &m_tree;
        TypeTable &m_types;
        const std::pmr::vector<std::uint32_t> &m_sig;
        const SymbolTable &m_symbols;
        const GrammarNodeSoA &m_nodes;
        std::size_t m_depth = 0;
    };

    // ---- token helpers --------------------------------------------------------
    std::size_t TyperImpl::MatchParen(std::size_t open, std::size_t limit) const
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

    // Position of the `>` that closes the `<` at `open` in a type context (`>>`
    // closes two levels), or `limit`.
    std::size_t TyperImpl::MatchAngle(std::size_t open, std::size_t limit) const
    {
        int depth = 0;
        for (std::size_t i = open; i < limit && i < m_sig.size(); ++i)
        {
            switch (At(i))
            {
            case Tok::Lt:
                ++depth;
                break;
            case Tok::Gt:
                if (--depth == 0)
                {
                    return i;
                }

                break;
            case Tok::Shr:
                depth -= 2;
                if (depth <= 0)
                {
                    return depth == 0 ? i : limit;
                }

                break;
            case Tok::LParen:
            case Tok::LBracket:
                i = MatchParen(i, limit);
                break;
            case Tok::Semi:
            case Tok::LBrace:
            case Tok::RBrace:
                return limit;
            default:
                break;
            }
        }

        return limit;
    }

    // ---- literals ---------------------------------------------------------------
    TypeId TyperImpl::NumberType(std::string_view text)
    {
        std::string s;
        for (const char c: text)
        {
            if (c != '\'')
            {
                s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
        }

        if (s.empty())
        {
            return TypeTable::Unknown;
        }

        const bool hex = s.size() > 1 && s[0] == '0' && s[1] == 'x';
        const bool binary = s.size() > 1 && s[0] == '0' && s[1] == 'b';
        const bool is_float = hex ? s.find('p') != std::string::npos
                                  : !binary && s.find_first_of(".e") != std::string::npos;
        if (is_float)
        {
            const char last = s.back();
            if (last == 'f')
            {
                return m_types.Builtin(BuiltinType::Float);
            }

            if (last == 'l')
            {
                return m_types.Builtin(BuiltinType::LongDouble);
            }

            return std::isdigit(static_cast<unsigned char>(last)) != 0 || last == '.' ? m_types.Builtin(BuiltinType::Double)
                                                                                      : TypeTable::Unknown;
        }

        std::size_t i = 0;
        int base = 10;
        if (hex)
        {
            base = 16;
            i = 2;
        }
        else if (binary)
        {
            base = 2;
            i = 2;
        }
        else if (s.size() > 1 && s[0] == '0')
        {
            base = 8;
            i = 1;
        }

        std::uint64_t value = 0;
        bool overflow = false;
        bool any = base == 8;
        for (; i < s.size(); ++i)
        {
            const int digit = DigitValue(s[i]);
            if (digit < 0 || digit >= base)
            {
                break;
            }

            any = true;
            if (value > (~0ull - static_cast<std::uint64_t>(digit)) / static_cast<std::uint64_t>(base))
            {
                overflow = true;
            }
            else
            {
                value = value * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(digit);
            }
        }

        if (!any || overflow)
        {
            return TypeTable::Unknown;
        }

        bool has_unsigned = false;
        std::string longs;
        for (; i < s.size(); ++i)
        {
            if (s[i] == 'u' && !has_unsigned)
            {
                has_unsigned = true;
            }
            else if (s[i] == 'l')
            {
                longs += 'l';
            }
            else
            {
                return TypeTable::Unknown; // user-defined literal, `z`, imaginary...
            }
        }

        // `lul` is not a valid suffix: the `l`s must be adjacent.
        const auto suffix = s.substr(s.size() - (longs.size() + (has_unsigned ? 1 : 0)));
        if (longs.size() > 2 || (longs.size() == 2 && suffix.find("ll") == std::string::npos))
        {
            return TypeTable::Unknown;
        }

        const bool fits_int = value <= 0x7FFFFFFFull;
        const bool fits_uint = value <= 0xFFFFFFFFull;
        if (longs.empty())
        {
            if (has_unsigned)
            {
                return fits_uint ? m_types.Builtin(BuiltinType::UInt) : TypeTable::Unknown;
            }

            if (fits_int)
            {
                return m_types.Builtin(BuiltinType::Int);
            }

            // Wider than int: long, unsigned or long long depending on the platform.
            return base != 10 && fits_uint ? m_types.Builtin(BuiltinType::UInt) : TypeTable::Unknown;
        }

        if (longs.size() == 1)
        {
            return m_types.Builtin(has_unsigned ? BuiltinType::ULong : BuiltinType::Long);
        }

        return m_types.Builtin(has_unsigned ? BuiltinType::ULongLong : BuiltinType::LongLong);
    }

    TypeId TyperImpl::CharType(std::string_view text)
    {
        if (text.empty())
        {
            return TypeTable::Unknown;
        }

        if (text[0] == '\'')
        {
            return m_types.Builtin(BuiltinType::Char);
        }

        if (text[0] == 'L')
        {
            return m_types.Builtin(BuiltinType::WChar);
        }

        if (text[0] == 'U')
        {
            return m_types.Builtin(BuiltinType::Char32);
        }

        if (text[0] == 'u')
        {
            return m_types.Builtin(text.size() > 1 && text[1] == '8' ? BuiltinType::Char8 : BuiltinType::Char16);
        }

        return TypeTable::Unknown;
    }

    // ---- declared types ---------------------------------------------------------
    TypeId TyperImpl::SymbolType(SymbolId symbol)
    {
        if (symbol >= m_symbols.Size())
        {
            return TypeTable::Unknown;
        }

        auto &slot = m_out.m_symbol_type[symbol];
        if (slot != kPending)
        {
            return slot;
        }

        if (m_depth >= kMaxDepth)
        {
            return TypeTable::Unknown;
        }

        // A cycle (alias to itself, `auto x = x;`) reads Unknown.
        slot = TypeTable::Unknown;
        ++m_depth;
        const auto result = ComputeSymbolType(symbol);
        --m_depth;
        m_out.m_symbol_type[symbol] = result;
        return result;
    }

    TypeId TyperImpl::ComputeSymbolType(SymbolId symbol)
    {
        switch (m_symbols.kind[symbol])
        {
        case SymbolKind::Variable:
        case SymbolKind::Parameter:
            return VariableType(symbol);
        case SymbolKind::Function:
            return ReturnType(symbol);
        case SymbolKind::TypeAlias:
            return AliasTarget(symbol);
        default:
            return TypeTable::Unknown;
        }
    }

    // `[::] a [<...>] [:: b [<...>]]...` at `pos`: a type name. Library (`std::`)
    // names stay External; names declared in this file resolve to symbols.
    TypeId TyperImpl::ParseName(std::size_t &pos, std::size_t end, ScopeId scope, std::uint32_t before)
    {
        const bool global = At(pos) == Tok::ColonColon;
        if (global)
        {
            ++pos;
        }

        std::string text;
        SymbolId current = kNone;
        bool first = true;
        bool is_std = false;
        bool template_args = false;
        bool dependent = false;
        bool unresolved = false;
        for (; pos < end && WordAt(pos);)
        {
            const auto spelled = TextAt(pos);
            Append(text, spelled);
            const auto name = m_model.Names().Find(spelled);
            SymbolId found = kNone;
            if (first)
            {
                is_std = spelled == "std";
                if (name != kNone)
                {
                    found = global ? m_model.LookupLocal(SemanticModel::TranslationUnitScope, name)
                                   : m_model.Lookup(scope, name, before);
                }
            }
            else if (current != kNone && name != kNone)
            {
                found = m_model.LookupMember(current, name);
            }

            if (found == kNone)
            {
                unresolved = true;
            }

            current = found;
            first = false;
            ++pos;
            if (pos < end && At(pos) == Tok::Lt)
            {
                const auto close = MatchAngle(pos, end);
                if (close >= end)
                {
                    pos = end;
                    return TypeTable::Unknown;
                }

                template_args = true;
                text += '<';
                for (std::size_t i = pos + 1; i < close; ++i)
                {
                    Append(text, TextAt(i));
                }

                text += '>';
                pos = close + 1;
                // `>>` closes two levels: the second `>` is consumed with the first.
                if (At(close) == Tok::Shr)
                {
                    text.pop_back();
                    text += ">>";
                }
            }

            if (pos + 1 < end && At(pos) == Tok::ColonColon && WordAt(pos + 1))
            {
                text += "::";
                ++pos;
                continue;
            }

            break;
        }

        if (text.empty() || dependent)
        {
            return TypeTable::Unknown;
        }

        if (is_std)
        {
            // Well-known aliases the engine can resolve exactly.
            if (text == "std::size_t")
            {
                return m_types.Builtin(BuiltinType::SizeT);
            }

            return m_types.External(m_out.m_externals.InternCopy(text));
        }

        if (current == kNone || unresolved)
        {
            if (unresolved && text == "size_t")
            {
                return m_types.Builtin(BuiltinType::SizeT);
            }

            return TypeTable::Unknown;
        }

        switch (m_symbols.kind[current])
        {
        case SymbolKind::Class:
            return template_args || (m_symbols.flags[current] & SymbolFlag::Template) != 0 ? TypeTable::Unknown
                                                                                          : m_types.Class(current);
        case SymbolKind::Enum:
            return m_types.Enum(current);
        case SymbolKind::TypeAlias:
            return template_args ? TypeTable::Unknown : SymbolType(current);
        default:
            return TypeTable::Unknown;
        }
    }

    TyperImpl::Base TyperImpl::ParseBase(std::size_t begin, std::size_t end, ScopeId scope, std::uint32_t before)
    {
        Base result;
        result.next = begin;
        std::size_t pos = begin;
        const auto skip_qualifiers = [&]()
        {
            while (pos < end)
            {
                const Tok tok = At(pos);
                if (tok == Tok::KwConst)
                {
                    result.is_const = true;
                }
                else if (!IsIgnoredSpecifier(tok))
                {
                    break;
                }

                ++pos;
            }
        };

        skip_qualifiers();
        if (pos >= end)
        {
            return result;
        }

        TypeId type = TypeTable::Unknown;
        const Tok first = At(pos);
        if (IsBuiltinWord(first))
        {
            bool is_unsigned = false;
            bool is_signed = false;
            bool is_short = false;
            int longs = 0;
            std::optional<Tok> core;
            while (pos < end && (IsBuiltinWord(At(pos)) || At(pos) == Tok::KwConst || At(pos) == Tok::KwVolatile))
            {
                switch (At(pos))
                {
                case Tok::KwUnsigned:
                    is_unsigned = true;
                    break;
                case Tok::KwSigned:
                    is_signed = true;
                    break;
                case Tok::KwShort:
                    is_short = true;
                    break;
                case Tok::KwLong:
                    ++longs;
                    break;
                case Tok::KwConst:
                    result.is_const = true;
                    break;
                case Tok::KwVolatile:
                    break;
                default:
                    core = At(pos);
                    break;
                }

                ++pos;
            }

            const Tok kind = core.value_or(Tok::KwInt);
            switch (kind)
            {
            case Tok::KwVoid:
                type = m_types.Builtin(BuiltinType::Void);
                break;
            case Tok::KwBool:
                type = m_types.Builtin(BuiltinType::Bool);
                break;
            case Tok::KwFloat:
                type = m_types.Builtin(BuiltinType::Float);
                break;
            case Tok::KwDouble:
                type = m_types.Builtin(longs > 0 ? BuiltinType::LongDouble : BuiltinType::Double);
                break;
            case Tok::KwWchar:
                type = m_types.Builtin(BuiltinType::WChar);
                break;
            case Tok::KwChar8:
                type = m_types.Builtin(BuiltinType::Char8);
                break;
            case Tok::KwChar16:
                type = m_types.Builtin(BuiltinType::Char16);
                break;
            case Tok::KwChar32:
                type = m_types.Builtin(BuiltinType::Char32);
                break;
            case Tok::KwChar:
                type = m_types.Builtin(is_unsigned ? BuiltinType::UChar : is_signed ? BuiltinType::SChar
                                                                                    : BuiltinType::Char);
                break;
            default:
                if (is_short)
                {
                    type = m_types.Builtin(is_unsigned ? BuiltinType::UShort : BuiltinType::Short);
                }
                else if (longs == 0)
                {
                    type = m_types.Builtin(is_unsigned ? BuiltinType::UInt : BuiltinType::Int);
                }
                else if (longs == 1)
                {
                    type = m_types.Builtin(is_unsigned ? BuiltinType::ULong : BuiltinType::Long);
                }
                else
                {
                    type = m_types.Builtin(is_unsigned ? BuiltinType::ULongLong : BuiltinType::LongLong);
                }

                break;
            }
        }
        else if (first == Tok::KwAuto)
        {
            result.is_auto = true;
            ++pos;
        }
        else if (first == Tok::KwDecltype)
        {
            pos = pos + 1 < end && At(pos + 1) == Tok::LParen ? MatchParen(pos + 1, end) + 1 : end;
        }
        else
        {
            bool dependent = false;
            if (first == Tok::KwTypename)
            {
                dependent = true;
                ++pos;
            }
            else if (first == Tok::KwStruct || first == Tok::KwClass || first == Tok::KwUnion || first == Tok::KwEnum)
            {
                ++pos;
            }

            type = ParseName(pos, end, scope, before);
            if (dependent)
            {
                type = TypeTable::Unknown;
            }
        }

        skip_qualifiers();
        result.next = std::min(pos, end);
        result.ok = true;
        result.type = result.is_const && !result.is_auto ? m_types.Const(type) : type;
        return result;
    }

    // `*`, `&`, `&&` and cv-qualifiers applied to `type`, left to right. Anything
    // else (function declarators, packs, names) makes the result Unknown.
    TypeId TyperImpl::ApplyOperators(TypeId type, std::size_t begin, std::size_t end)
    {
        for (std::size_t i = begin; i < end; ++i)
        {
            switch (At(i))
            {
            case Tok::Star:
                type = m_types.Pointer(type);
                break;
            case Tok::Amp:
                type = m_types.LRef(type);
                break;
            case Tok::AmpAmp:
                type = m_types.RRef(type);
                break;
            case Tok::KwConst:
                type = m_types.Const(type);
                break;
            case Tok::KwVolatile:
                break;
            default:
                return TypeTable::Unknown;
            }
        }

        return type;
    }

    TypeId TyperImpl::ParseTypeTokens(std::size_t begin, std::size_t end, ScopeId scope, std::uint32_t before)
    {
        const auto base = ParseBase(begin, end, scope, before);
        if (!base.ok || base.is_auto)
        {
            return TypeTable::Unknown;
        }

        return ApplyOperators(base.type, base.next, end);
    }

    // The type declared by `declarator` for the written base type `base`.
    TypeId TyperImpl::DeclaratorType(Base base, std::uint32_t declarator, SymbolKind kind, TypeId init_type)
    {
        if (declarator == kNone || FindChild(declarator, GrammarKind::FunctionSuffix) != kNone)
        {
            return TypeTable::Unknown;
        }

        const auto name = FindChild(declarator, GrammarKind::DeclaredName);
        if (name == kNone)
        {
            return TypeTable::Unknown;
        }

        const auto prefix_begin = Pos(m_nodes.FirstToken(declarator));
        const auto prefix_end = Pos(m_nodes.FirstToken(name));
        TypeId type = base.type;
        if (base.is_auto)
        {
            // `auto x = e;` takes e's type without references, top const or arrays.
            if (init_type == TypeTable::Unknown)
            {
                return TypeTable::Unknown;
            }

            bool reference = false;
            for (std::size_t i = prefix_begin; i < prefix_end; ++i)
            {
                if (At(i) == Tok::Amp)
                {
                    reference = true;
                }
                else if (At(i) != Tok::KwConst && At(i) != Tok::KwVolatile)
                {
                    return TypeTable::Unknown; // `auto*`, `auto&&`, `auto**`
                }
            }

            if (reference)
            {
                type = m_types.LRef(base.is_const ? m_types.Const(m_types.Value(init_type)) : m_types.Value(init_type));
            }
            else
            {
                type = m_types.Decay(m_types.Strip(init_type));
                if (base.is_const)
                {
                    type = m_types.Const(type);
                }
            }
        }
        else
        {
            type = ApplyOperators(type, prefix_begin, prefix_end);
        }

        // Array suffixes wrap the element type, the last one innermost.
        std::vector<std::uint32_t> suffixes;
        for (const auto child: m_model.ChildrenOf(declarator))
        {
            if (m_nodes.Kind(child) == GrammarKind::ArraySuffix)
            {
                suffixes.push_back(child);
            }
        }

        for (auto it = suffixes.rbegin(); it != suffixes.rend(); ++it)
        {
            const auto [b, e] = NodeSig(*it);
            std::uint32_t extent = kNone;
            if (e == b + 3 && At(b) == Tok::LBracket && Tokens()[m_sig[b + 1]].kind == TokenKind::Number)
            {
                const auto text = TextAt(b + 1);
                std::uint64_t value = 0;
                bool digits = !text.empty() && text.size() < 10;
                for (const char c: text)
                {
                    digits = digits && c >= '0' && c <= '9';
                    value = value * 10 + static_cast<std::uint64_t>(c - '0');
                }

                if (digits)
                {
                    extent = static_cast<std::uint32_t>(value);
                }
            }

            type = m_types.Array(type, extent);
        }

        // A parameter declared as an array is a pointer.
        if (kind == SymbolKind::Parameter && m_types.Kind(type) == TypeKind::Array)
        {
            type = m_types.Pointer(m_types.Arg(type));
        }

        return type;
    }

    TypeId TyperImpl::VariableType(SymbolId symbol)
    {
        const auto node = m_symbols.decl_node[symbol];
        if (node >= m_nodes.size())
        {
            return TypeTable::Unknown;
        }

        const auto kind = m_nodes.Kind(node);
        if (kind != GrammarKind::Declaration && kind != GrammarKind::DeclarationStatement &&
            kind != GrammarKind::ParameterDeclaration)
        {
            return TypeTable::Unknown;
        }

        const auto spec = FindChild(node, GrammarKind::TypeSpecifier);
        if (spec == kNone)
        {
            return TypeTable::Unknown;
        }

        std::uint32_t declarator = kNone;
        std::uint32_t init = kNone;
        for (const auto child: m_model.ChildrenOf(node))
        {
            std::uint32_t candidate = kNone;
            if (m_nodes.Kind(child) == GrammarKind::InitDeclarator)
            {
                candidate = FindChild(child, GrammarKind::Declarator);
            }
            else if (m_nodes.Kind(child) == GrammarKind::Declarator)
            {
                candidate = child;
            }

            if (candidate == kNone)
            {
                continue;
            }

            const auto name = FindChild(candidate, GrammarKind::DeclaredName);
            if (name != kNone && m_nodes.FirstToken(name) == m_symbols.decl_token[symbol])
            {
                declarator = candidate;
                init = m_nodes.Kind(child) == GrammarKind::InitDeclarator ? child : kNone;
                break;
            }
        }

        if (declarator == kNone)
        {
            return TypeTable::Unknown;
        }

        const auto [spec_begin, spec_end] = NodeSig(spec);
        const auto scope = m_model.ScopeOfNode(node);
        auto base = ParseBase(spec_begin, spec_end, scope, m_symbols.decl_token[symbol]);
        if (!base.ok)
        {
            return TypeTable::Unknown;
        }

        // Operators the grammar left inside the specifier.
        if (base.next < spec_end)
        {
            if (base.is_auto)
            {
                return TypeTable::Unknown;
            }

            base.type = ApplyOperators(base.type, base.next, spec_end);
        }

        TypeId init_type = TypeTable::Unknown;
        if (base.is_auto && init != kNone)
        {
            const auto name_pos = Pos(m_symbols.decl_token[symbol]);
            if (At(name_pos + 1) == Tok::Eq)
            {
                const auto init_end = NodeSig(init).second;
                for (const auto child: m_model.ChildrenOf(init))
                {
                    if (m_nodes.Kind(child) == GrammarKind::Declarator || m_nodes.Kind(child) == GrammarKind::TypeSpecifier)
                    {
                        continue;
                    }

                    const auto [cb, ce] = NodeSig(child);
                    if (cb == name_pos + 2 && ce == init_end)
                    {
                        init_type = NodeType(child);
                        break;
                    }
                }
            }
        }

        return DeclaratorType(base, declarator, m_symbols.kind[symbol], init_type);
    }

    TypeId TyperImpl::ReturnType(SymbolId symbol)
    {
        if ((m_symbols.flags[symbol] & (SymbolFlag::Constructor | SymbolFlag::Destructor)) != 0)
        {
            return TypeTable::Unknown;
        }

        const auto node = m_symbols.decl_node[symbol];
        if (node >= m_nodes.size() ||
            (m_nodes.Kind(node) != GrammarKind::FunctionDeclaration && m_nodes.Kind(node) != GrammarKind::FunctionDefinition))
        {
            return TypeTable::Unknown;
        }

        const auto spec = FindChild(node, GrammarKind::TypeSpecifier);
        const auto declarator = FindChild(node, GrammarKind::Declarator);
        if (spec == kNone || declarator == kNone)
        {
            return TypeTable::Unknown;
        }

        const auto [spec_begin, spec_end] = NodeSig(spec);
        const auto scope = m_model.ScopeOfNode(node);
        const auto base = ParseBase(spec_begin, spec_end, scope, m_symbols.decl_token[symbol]);
        if (!base.ok)
        {
            return TypeTable::Unknown;
        }

        if (base.is_auto)
        {
            auto suffix = FindChild(declarator, GrammarKind::FunctionSuffix);
            auto trailing = FindChild(declarator, GrammarKind::TrailingReturnType);
            if (trailing == kNone && suffix != kNone)
            {
                trailing = FindChild(suffix, GrammarKind::TrailingReturnType);
            }

            if (trailing == kNone)
            {
                return TypeTable::Unknown; // deduced from the body
            }

            std::size_t begin = NodeSig(trailing).first;
            if (At(begin) == Tok::Arrow)
            {
                ++begin;
            }

            std::size_t end = begin;
            while (end < m_sig.size() && At(end) != Tok::LBrace && At(end) != Tok::Semi && At(end) != Tok::Eq &&
                At(end) != Tok::KwRequires && At(end) != Tok::KwOverride && At(end) != Tok::KwFinal)
            {
                if (At(end) == Tok::LParen || At(end) == Tok::LBracket)
                {
                    end = MatchParen(end, m_sig.size());
                }

                ++end;
            }

            return ParseTypeTokens(begin, std::min(end, m_sig.size()), scope, m_symbols.decl_token[symbol]);
        }

        const auto name = FindChild(declarator, GrammarKind::DeclaredName);
        if (name == kNone)
        {
            return TypeTable::Unknown;
        }

        const auto prefix_begin = Pos(m_nodes.FirstToken(declarator));
        const auto prefix_end = Pos(m_nodes.FirstToken(name));
        TypeId type = base.type;
        if (base.next < spec_end)
        {
            type = ApplyOperators(type, base.next, spec_end);
        }

        // A qualified name or function-pointer declarator leaves other tokens: Unknown.
        return ApplyOperators(type, prefix_begin, prefix_end);
    }

    TypeId TyperImpl::AliasTarget(SymbolId symbol)
    {
        const auto node = m_symbols.decl_node[symbol];
        if (node >= m_nodes.size())
        {
            return TypeTable::Unknown;
        }

        if (m_nodes.Kind(node) == GrammarKind::UsingDeclaration)
        {
            const auto parent = m_nodes.Parent(node);
            if (parent < m_nodes.size() && m_nodes.Kind(parent) == GrammarKind::TemplateDeclaration)
            {
                return TypeTable::Unknown;
            }

            const auto [begin, end] = NodeSig(node);
            const auto name_pos = Pos(m_symbols.decl_token[symbol]);
            if (name_pos + 2 >= end || At(name_pos + 1) != Tok::Eq)
            {
                return TypeTable::Unknown;
            }

            std::size_t stop = name_pos + 2;
            while (stop < end && At(stop) != Tok::Semi)
            {
                ++stop;
            }

            (void) begin;
            return ParseTypeTokens(name_pos + 2, stop, m_model.ScopeOfNode(node), m_symbols.decl_token[symbol]);
        }

        // `typedef T Name;` reads like a variable declaration.
        return VariableType(symbol);
    }

    // The return type shared by every overload of `symbol`'s name in its scope.
    TypeId TyperImpl::OverloadReturn(SymbolId symbol)
    {
        TypeId shared = TypeTable::Unknown;
        bool first = true;
        for (auto candidate = m_model.LookupLocal(m_symbols.scope[symbol], m_symbols.name[symbol]); candidate != kNone;
            candidate = m_symbols.next_same_name[candidate])
        {
            if (m_symbols.kind[candidate] != SymbolKind::Function)
            {
                return TypeTable::Unknown;
            }

            if ((m_symbols.flags[candidate] & SymbolFlag::Template) != 0)
            {
                return TypeTable::Unknown;
            }

            const auto type = SymbolType(candidate);
            if (type == TypeTable::Unknown || (!first && type != shared))
            {
                return TypeTable::Unknown;
            }

            shared = type;
            first = false;
        }

        return shared;
    }

    // ---- expressions --------------------------------------------------------------
    bool TyperImpl::IsScalar(TypeId type) const
    {
        const auto kind = m_types.Kind(type);
        if (kind == TypeKind::Builtin)
        {
            return !m_types.IsBuiltin(type, BuiltinType::Void);
        }

        return kind == TypeKind::Pointer || kind == TypeKind::Enum;
    }

    ScopeId TyperImpl::EnclosingClass(ScopeId scope) const
    {
        const auto &scopes = m_model.Scopes();
        for (std::size_t steps = 0; scope != kNone && scope < scopes.Size() && steps <= scopes.Size(); ++steps)
        {
            if (scopes.kind[scope] == ScopeKind::Class)
            {
                return scope;
            }

            if (scope == SemanticModel::TranslationUnitScope)
            {
                break;
            }

            scope = scopes.parent[scope];
        }

        return kNone;
    }

    TypeId TyperImpl::Promoted(TypeId type)
    {
        type = m_types.Strip(type);
        if (!m_types.IsArithmetic(type))
        {
            return TypeTable::Unknown;
        }

        const auto builtin = static_cast<BuiltinType>(m_types.Arg(type));
        if (m_types.IsFloating(type) || builtin == BuiltinType::SizeT)
        {
            return type;
        }

        const auto promoted = Promote(builtin);
        return promoted ? m_types.Builtin(*promoted) : TypeTable::Unknown;
    }

    // Usual arithmetic conversions. Unknown whenever the result depends on the
    // platform's type widths.
    TypeId TyperImpl::Arithmetic(TypeId a, TypeId b)
    {
        a = m_types.Strip(a);
        b = m_types.Strip(b);
        if (!m_types.IsArithmetic(a) || !m_types.IsArithmetic(b))
        {
            return TypeTable::Unknown;
        }

        const auto ba = static_cast<BuiltinType>(m_types.Arg(a));
        const auto bb = static_cast<BuiltinType>(m_types.Arg(b));
        for (const auto floating: {BuiltinType::LongDouble, BuiltinType::Double, BuiltinType::Float})
        {
            if (ba == floating || bb == floating)
            {
                return m_types.Builtin(floating);
            }
        }

        if (ba == BuiltinType::SizeT || bb == BuiltinType::SizeT)
        {
            return m_types.Builtin(BuiltinType::SizeT);
        }

        const auto pa = Promote(ba);
        const auto pb = Promote(bb);
        if (!pa || !pb)
        {
            return TypeTable::Unknown;
        }

        if (*pa == *pb)
        {
            return m_types.Builtin(*pa);
        }

        if (IsUnsignedInt(*pa) == IsUnsignedInt(*pb))
        {
            return m_types.Builtin(Rank(*pa) >= Rank(*pb) ? *pa : *pb);
        }

        const auto unsigned_type = IsUnsignedInt(*pa) ? *pa : *pb;
        const auto signed_type = IsUnsignedInt(*pa) ? *pb : *pa;
        if (Rank(unsigned_type) >= Rank(signed_type))
        {
            return m_types.Builtin(unsigned_type);
        }

        return signed_type == BuiltinType::LongLong && unsigned_type == BuiltinType::UInt ? m_types.Builtin(signed_type)
                                                                                          : TypeTable::Unknown;
    }

    TypeId TyperImpl::NodeType(std::uint32_t node)
    {
        if (node >= m_nodes.size())
        {
            return TypeTable::Unknown;
        }

        auto &slot = m_out.m_node_type[node];
        if (slot != kPending)
        {
            return slot;
        }

        if (m_depth >= kMaxDepth)
        {
            return TypeTable::Unknown;
        }

        slot = TypeTable::Unknown;
        ++m_depth;
        const auto result = m_types.Value(ComputeNodeType(node));
        --m_depth;
        m_out.m_node_type[node] = result;
        return result;
    }

    TypeId TyperImpl::ComputeNodeType(std::uint32_t node)
    {
        switch (m_nodes.Kind(node))
        {
        case GrammarKind::LiteralExpression:
            return LiteralType(node);
        case GrammarKind::IdentifierExpression:
            return IdentifierType(node);
        case GrammarKind::ParenthesizedExpression:
        {
            const auto kids = m_model.ChildrenOf(node);
            return kids.size() == 1 ? NodeType(kids[0]) : TypeTable::Unknown;
        }
        case GrammarKind::UnaryExpression:
            return UnaryType(node);
        case GrammarKind::BinaryExpression:
            return BinaryType(node);
        case GrammarKind::ConditionalExpression:
            return ConditionalType(node);
        case GrammarKind::CallExpression:
            return CallType(node);
        case GrammarKind::SubscriptExpression:
            return SubscriptType(node);
        case GrammarKind::MemberExpression:
            return MemberType(node, false);
        default:
            return TypeTable::Unknown;
        }
    }

    TypeId TyperImpl::LiteralType(std::uint32_t node)
    {
        const auto [begin, end] = NodeSig(node);
        if (begin >= end)
        {
            return TypeTable::Unknown;
        }

        const auto &token = Tokens()[m_sig[begin]];
        switch (token.tok)
        {
        case Tok::KwTrue:
        case Tok::KwFalse:
            return m_types.Builtin(BuiltinType::Bool);
        case Tok::KwNullptr:
            return m_types.Builtin(BuiltinType::NullptrT);
        default:
            break;
        }

        switch (token.kind)
        {
        case TokenKind::Number:
            return NumberType(Text(m_sig[begin]));
        case TokenKind::CharacterLiteral:
            return CharType(Text(m_sig[begin]));
        case TokenKind::StringLiteral:
        case TokenKind::RawStringLiteral:
        {
            // Plain narrow literals only: `"..."` and `R"(...)"`.
            const auto text = Text(m_sig[begin]);
            if (!text.empty() && (text[0] == '"' || (text[0] == 'R' && text.size() > 1 && text[1] == '"')))
            {
                return m_types.Pointer(m_types.Const(m_types.Builtin(BuiltinType::Char)));
            }

            return TypeTable::Unknown;
        }
        default:
            return TypeTable::Unknown;
        }
    }

    TypeId TyperImpl::IdentifierType(std::uint32_t node)
    {
        const auto [begin, end] = NodeSig(node);
        if (end != begin + 1)
        {
            return TypeTable::Unknown;
        }

        const auto token = m_sig[begin];
        // The grammar reads these keywords as identifiers.
        switch (Tokens()[token].tok)
        {
        case Tok::KwTrue:
        case Tok::KwFalse:
            return m_types.Builtin(BuiltinType::Bool);
        case Tok::KwNullptr:
            return m_types.Builtin(BuiltinType::NullptrT);
        default:
            break;
        }

        if (Tokens()[token].tok == Tok::KwThis)
        {
            const auto klass = EnclosingClass(m_model.ScopeOfNode(node));
            if (klass == kNone || m_model.Scopes().owner[klass] == kNone)
            {
                return TypeTable::Unknown;
            }

            return m_types.Pointer(m_types.Class(m_model.Scopes().owner[klass]));
        }

        const auto symbol = m_model.ResolveToken(token);
        if (symbol == kNone)
        {
            return TypeTable::Unknown;
        }

        const auto kind = m_symbols.kind[symbol];
        return kind == SymbolKind::Variable || kind == SymbolKind::Parameter ? SymbolType(symbol) : TypeTable::Unknown;
    }

    TypeId TyperImpl::UnaryType(std::uint32_t node)
    {
        const auto kids = m_model.ChildrenOf(node);
        const auto [begin, end] = NodeSig(node);
        if (kids.empty() || end <= begin)
        {
            return TypeTable::Unknown;
        }

        const Tok first = At(begin);
        const bool prefix = (first == Tok::Plus || first == Tok::Minus || first == Tok::Bang || first == Tok::Tilde ||
                                first == Tok::Star || first == Tok::Amp || first == Tok::PlusPlus ||
                                first == Tok::MinusMinus) &&
            Pos(m_nodes.FirstToken(kids[0])) == begin + 1;
        TypeId operand = TypeTable::Unknown;
        Tok op = Tok::None;
        if (prefix)
        {
            op = first;
            operand = NodeType(kids[0]);
        }
        else
        {
            const Tok last = At(end - 1);
            if ((last != Tok::PlusPlus && last != Tok::MinusMinus) || !SameStart(kids[0], node))
            {
                return TypeTable::Unknown;
            }

            op = last;
            operand = NodeType(kids[0]);
        }

        const auto stripped = m_types.Strip(operand);
        switch (op)
        {
        case Tok::Bang:
            return IsScalar(stripped) || m_types.IsArray(stripped) ? m_types.Builtin(BuiltinType::Bool)
                                                                   : TypeTable::Unknown;
        case Tok::Plus:
        case Tok::Minus:
            return m_types.IsPointer(stripped) && op == Tok::Plus ? stripped : Promoted(stripped);
        case Tok::Tilde:
            return m_types.IsInteger(stripped) ? Promoted(stripped) : TypeTable::Unknown;
        case Tok::Star:
            if (m_types.IsPointer(stripped) || m_types.IsArray(stripped))
            {
                return m_types.Value(m_types.Element(stripped));
            }

            return TypeTable::Unknown;
        case Tok::Amp:
            if (operand == TypeTable::Unknown || m_types.Kind(stripped) == TypeKind::Class ||
                m_types.Kind(stripped) == TypeKind::External)
            {
                return TypeTable::Unknown; // `operator&` may be overloaded
            }

            return m_types.Pointer(m_types.Value(operand));
        case Tok::PlusPlus:
        case Tok::MinusMinus:
            return m_types.IsArithmetic(stripped) || m_types.IsPointer(stripped) ? stripped : TypeTable::Unknown;
        default:
            return TypeTable::Unknown;
        }
    }

    TypeId TyperImpl::BinaryType(std::uint32_t node)
    {
        const auto kids = m_model.ChildrenOf(node);
        if (kids.size() != 2 || !SameStart(kids[0], node))
        {
            return TypeTable::Unknown;
        }

        const auto op_pos = NodeSig(kids[0]).second;
        const Tok op = At(op_pos);
        const auto left_type = NodeType(kids[0]);
        const auto right_type = NodeType(kids[1]);
        if (op == Tok::Comma)
        {
            return right_type;
        }

        const auto left = m_types.Decay(m_types.Strip(left_type));
        const auto right = m_types.Decay(m_types.Strip(right_type));
        const auto boolean = m_types.Builtin(BuiltinType::Bool);
        if (IsAssignment(op))
        {
            return IsScalar(left) ? m_types.Value(left_type) : TypeTable::Unknown;
        }

        switch (op)
        {
        case Tok::AmpAmp:
        case Tok::PipePipe:
        case Tok::EqEq:
        case Tok::BangEq:
        case Tok::Lt:
        case Tok::Gt:
        case Tok::Le:
        case Tok::Ge:
            return IsScalar(left) && IsScalar(right) ? boolean : TypeTable::Unknown;
        case Tok::Plus:
            if (m_types.IsPointer(left) && m_types.IsInteger(right))
            {
                return left;
            }

            if (m_types.IsInteger(left) && m_types.IsPointer(right))
            {
                return right;
            }

            return Arithmetic(left, right);
        case Tok::Minus:
            if (m_types.IsPointer(left) && m_types.IsInteger(right))
            {
                return left;
            }

            return Arithmetic(left, right);
        case Tok::Star:
        case Tok::Slash:
            return Arithmetic(left, right);
        case Tok::Percent:
            return m_types.IsInteger(left) && m_types.IsInteger(right) ? Arithmetic(left, right) : TypeTable::Unknown;
        case Tok::Shl:
        case Tok::Shr:
            return m_types.IsInteger(left) && m_types.IsInteger(right) ? Promoted(left) : TypeTable::Unknown;
        case Tok::Amp:
        case Tok::Pipe:
        case Tok::Caret:
            return (m_types.IsInteger(left) || m_types.IsBool(left)) && (m_types.IsInteger(right) || m_types.IsBool(right))
                ? Arithmetic(left, right)
                : TypeTable::Unknown;
        default:
            return TypeTable::Unknown;
        }
    }

    TypeId TyperImpl::ConditionalType(std::uint32_t node)
    {
        const auto kids = m_model.ChildrenOf(node);
        if (kids.size() != 3 || !SameStart(kids[0], node))
        {
            return TypeTable::Unknown;
        }

        const auto a = m_types.Value(NodeType(kids[1]));
        const auto b = m_types.Value(NodeType(kids[2]));
        if (a == b)
        {
            return a;
        }

        const auto sa = m_types.Decay(m_types.Strip(a));
        const auto sb = m_types.Decay(m_types.Strip(b));
        if (m_types.IsArithmetic(sa) && m_types.IsArithmetic(sb))
        {
            return Arithmetic(sa, sb);
        }

        if (m_types.IsPointer(sa) && m_types.IsBuiltin(sb, BuiltinType::NullptrT))
        {
            return sa;
        }

        if (m_types.IsPointer(sb) && m_types.IsBuiltin(sa, BuiltinType::NullptrT))
        {
            return sb;
        }

        return TypeTable::Unknown;
    }

    TypeId TyperImpl::SubscriptType(std::uint32_t node)
    {
        const auto kids = m_model.ChildrenOf(node);
        if (kids.size() != 2 || !SameStart(kids[0], node))
        {
            return TypeTable::Unknown;
        }

        const auto base = m_types.Strip(NodeType(kids[0]));
        if (m_types.IsArray(base) || m_types.IsPointer(base))
        {
            return m_types.Value(m_types.Element(base));
        }

        if (m_types.Kind(base) == TypeKind::Class)
        {
            const auto name = m_model.Names().Find("operator[]");
            const auto member = name == kNone ? kNone : m_model.LookupMember(m_types.Arg(base), name);
            if (member != kNone && m_symbols.kind[member] == SymbolKind::Function)
            {
                return OverloadReturn(member);
            }
        }

        return TypeTable::Unknown;
    }

    // Result of `size()`, `empty()`... on a library container.
    TypeId TyperImpl::ExternalCall(TypeId object, std::string_view member)
    {
        const auto head = m_out.ExternalHead(object);
        if (!IsStdContainerHead(head))
        {
            return TypeTable::Unknown;
        }

        if (member == "size" || member == "max_size" || (member == "length" && IsStringHead(head)) ||
            (member == "capacity" && (head == "std::vector" || IsStringHead(head))))
        {
            return m_types.Builtin(BuiltinType::SizeT);
        }

        return member == "empty" ? m_types.Builtin(BuiltinType::Bool) : TypeTable::Unknown;
    }

    // `a.b`, `p->b` and `N::b`: the type of the field, or (as a callee) of the
    // result of calling the function.
    TypeId TyperImpl::MemberType(std::uint32_t node, bool as_call)
    {
        const auto kids = m_model.ChildrenOf(node);
        if (kids.size() != 2 || !SameStart(kids[0], node))
        {
            return TypeTable::Unknown;
        }

        const auto op_pos = NodeSig(kids[0]).second;
        const Tok op = At(op_pos);
        const auto [name_begin, name_end] = NodeSig(kids[1]);
        if (name_end != name_begin + 1 || !WordAt(name_begin) || name_begin != op_pos + 1)
        {
            return TypeTable::Unknown;
        }

        SymbolId member = kNone;
        TypeId object = TypeTable::Unknown;
        if (op == Tok::ColonColon)
        {
            member = m_model.ResolveToken(m_sig[name_begin]);
        }
        else if (op == Tok::Dot || op == Tok::Arrow)
        {
            object = m_types.Strip(NodeType(kids[0]));
            if (op == Tok::Arrow)
            {
                if (!m_types.IsPointer(object))
                {
                    return TypeTable::Unknown; // smart pointers and iterators
                }

                object = m_types.Strip(m_types.Element(object));
            }

            if (m_types.Kind(object) == TypeKind::Class)
            {
                const auto name = m_model.Names().Find(TextAt(name_begin));
                member = name == kNone ? kNone : m_model.LookupMember(m_types.Arg(object), name);
            }
            else if (m_types.Kind(object) == TypeKind::External && as_call && op == Tok::Dot)
            {
                return ExternalCall(object, TextAt(name_begin));
            }
        }

        if (member == kNone)
        {
            return TypeTable::Unknown;
        }

        const auto kind = m_symbols.kind[member];
        if (as_call)
        {
            return kind == SymbolKind::Function ? OverloadReturn(member) : TypeTable::Unknown;
        }

        return kind == SymbolKind::Variable || kind == SymbolKind::Parameter ? SymbolType(member) : TypeTable::Unknown;
    }

    // `static_cast<T>(...)` and friends: the callee is `kw < T >`.
    TypeId TyperImpl::CastType(std::uint32_t callee)
    {
        const auto [begin, end] = NodeSig(callee);
        if (end < begin + 4 || At(begin + 1) != Tok::Lt || At(end - 1) != Tok::Gt)
        {
            return TypeTable::Unknown;
        }

        return ParseTypeTokens(begin + 2, end - 1, m_model.ScopeOfNode(callee), m_sig[begin]);
    }

    TypeId TyperImpl::CallType(std::uint32_t node)
    {
        const auto kids = m_model.ChildrenOf(node);
        if (kids.empty() || !SameStart(kids[0], node))
        {
            return TypeTable::Unknown;
        }

        const auto callee = kids[0];
        switch (m_nodes.Kind(callee))
        {
        case GrammarKind::IdentifierExpression:
        {
            const auto [begin, end] = NodeSig(callee);
            if (end != begin + 1)
            {
                return TypeTable::Unknown;
            }

            const Tok tok = At(begin);
            if (tok == Tok::KwSizeof || tok == Tok::KwAlignof)
            {
                return m_types.Builtin(BuiltinType::SizeT);
            }

            if (tok == Tok::KwNoexcept)
            {
                return m_types.Builtin(BuiltinType::Bool);
            }

            if (IsBuiltinWord(tok))
            {
                return ParseTypeTokens(begin, end, m_model.ScopeOfNode(callee), m_sig[begin]);
            }

            if (!WordAt(begin))
            {
                return TypeTable::Unknown;
            }

            const auto symbol = m_model.ResolveToken(m_sig[begin]);
            if (symbol == kNone)
            {
                return TypeTable::Unknown;
            }

            switch (m_symbols.kind[symbol])
            {
            case SymbolKind::Function:
                return OverloadReturn(symbol);
            case SymbolKind::Class:
                return (m_symbols.flags[symbol] & SymbolFlag::Template) != 0 ? TypeTable::Unknown
                                                                            : m_types.Class(symbol);
            case SymbolKind::TypeAlias:
                return SymbolType(symbol);
            default:
                return TypeTable::Unknown;
            }
        }
        case GrammarKind::TemplateIdExpression:
        {
            const Tok tok = At(NodeSig(callee).first);
            if (tok == Tok::KwStaticCast || tok == Tok::KwDynamicCast || tok == Tok::KwReinterpretCast ||
                tok == Tok::KwConstCast)
            {
                return CastType(callee);
            }

            return TypeTable::Unknown;
        }
        case GrammarKind::MemberExpression:
            return MemberType(callee, true);
        default:
            return TypeTable::Unknown;
        }
    }

    void TyperImpl::Run()
    {
        m_out.m_symbol_type.assign(m_symbols.Size(), kPending);
        m_out.m_node_type.assign(m_nodes.size(), kPending);
        for (SymbolId symbol = 0; symbol < m_symbols.Size(); ++symbol)
        {
            SymbolType(symbol);
        }

        for (std::uint32_t node = 0; node < m_nodes.size(); ++node)
        {
            switch (m_nodes.Kind(node))
            {
            case GrammarKind::LiteralExpression:
            case GrammarKind::IdentifierExpression:
            case GrammarKind::ParenthesizedExpression:
            case GrammarKind::UnaryExpression:
            case GrammarKind::BinaryExpression:
            case GrammarKind::ConditionalExpression:
            case GrammarKind::CallExpression:
            case GrammarKind::SubscriptExpression:
            case GrammarKind::MemberExpression:
                NodeType(node);
                break;
            default:
                m_out.m_node_type[node] = TypeTable::Unknown;
                break;
            }
        }
    }

    TypeModel Typer::Type(const SemanticModel &model)
    {
        const auto nodes = model.Tree().NodesSoA().size();
        TypeModel result(model, std::max<std::size_t>(32 * 1024, nodes * 24));
        TyperImpl(result, model).Run();
        return result;
    }

} // namespace heimdall
