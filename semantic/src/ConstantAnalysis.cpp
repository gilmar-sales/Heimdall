#include "detail/ConstantAnalysis.hpp"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <string>
#include <string_view>

namespace heimdall::detail
{

    namespace
    {

        constexpr std::int64_t kIntMin  = INT_MIN;
        constexpr std::int64_t kIntMax  = INT_MAX;
        constexpr std::int64_t kUIntMax = UINT_MAX;

        bool IsTypeKeyword(Tok tok)
        {
            switch (tok)
            {
                case Tok::KwConst:
                case Tok::KwUnsigned:
                case Tok::KwSigned:
                case Tok::KwInt:
                case Tok::KwLong:
                case Tok::KwShort:
                case Tok::KwChar:
                case Tok::KwBool:
                case Tok::KwFloat:
                case Tok::KwDouble:
                case Tok::KwWchar:
                case Tok::KwChar8:
                case Tok::KwChar16:
                case Tok::KwChar32:
                case Tok::Star:
                    return true;
                default:
                    return false;
            }
        }

    } // namespace

    // Precedence-climbing evaluator over the significant tokens [pos, end) of one
    // expression. Works on tokens, not on grammar nodes: the grammar reads casts and
    // some initializers oddly, and a token-level reading either understands the
    // whole text or reports "not constant".
    class ConstantAnalysis::Parser
    {
      public:
        Parser(ConstantAnalysis& owner, std::size_t begin, std::size_t end) :
            m_owner(owner), m_view(owner.m_view), m_pos(begin), m_end(end)
        {
        }

        // The whole range must be one expression.
        Value Run()
        {
            Value value = Ternary();
            return m_pos == m_end && value.ok ? value : Value {};
        }

      private:
        static Value Int(std::int64_t i, bool known, bool wide, bool is_unsigned)
        {
            Value v;
            v.ok          = true;
            v.known       = known && !wide;
            v.wide        = wide;
            v.is_unsigned = is_unsigned;
            v.i           = v.known ? i : 0;
            return v;
        }

        static Value Float(double f, bool known)
        {
            Value v;
            v.ok       = true;
            v.is_float = true;
            v.known    = known;
            v.f        = known ? f : 0;
            return v;
        }

        static double AsDouble(const Value& v)
        {
            return v.is_float ? v.f : static_cast<double>(v.i);
        }

        // Result of an integer operation: wraps unsigned, rejects signed overflow.
        static Value Normalize(std::int64_t r, bool known, bool wide, bool is_unsigned)
        {
            if (wide || !known)
            {
                return Int(0, false, wide, is_unsigned);
            }

            if (is_unsigned)
            {
                return Int(static_cast<std::int64_t>(static_cast<std::uint32_t>(r)), true, false,
                           true);
            }

            if (r < kIntMin || r > kIntMax)
            {
                return Value {}; // signed overflow: not a constant expression
            }

            return Int(r, true, false, false);
        }

        static std::int64_t As(const Value& v, bool is_unsigned)
        {
            return is_unsigned ? static_cast<std::int64_t>(static_cast<std::uint32_t>(v.i)) : v.i;
        }

        static bool Truthy(const Value& v) { return v.is_float ? v.f != 0.0 : v.i != 0; }

        Value Number(std::string_view text) const
        {
            std::string digits;
            for (const char c : text)
            {
                if (c != '\'')
                {
                    digits += c;
                }
            }

            const bool hex =
                digits.size() > 1 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X');
            const bool binary =
                digits.size() > 1 && digits[0] == '0' && (digits[1] == 'b' || digits[1] == 'B');
            const bool floating = hex ? digits.find_first_of("pP") != std::string::npos
                                      : !binary && digits.find_first_of(".eE") != std::string::npos;
            if (floating)
            {
                while (!digits.empty() && (digits.back() == 'f' || digits.back() == 'F' ||
                                           digits.back() == 'l' || digits.back() == 'L'))
                {
                    if (hex && (digits.back() == 'f' || digits.back() == 'F'))
                    {
                        break; // a hex digit, not a suffix
                    }

                    digits.pop_back();
                }

                char*        stop   = nullptr;
                const double parsed = std::strtod(digits.c_str(), &stop);
                return stop != nullptr && *stop == '\0' ? Float(parsed, true) : Value {};
            }

            bool        is_unsigned = false;
            std::size_t longs       = 0;
            while (!digits.empty() &&
                   std::string_view("uUlLzZ").find(digits.back()) != std::string_view::npos)
            {
                const char c = digits.back();
                is_unsigned  = is_unsigned || c == 'u' || c == 'U';
                longs += (c == 'l' || c == 'L' || c == 'z' || c == 'Z') ? 1 : 0;
                digits.pop_back();
            }

            int         base = 10;
            std::size_t skip = 0;
            if (hex)
            {
                base = 16;
                skip = 2;
            }
            else if (binary)
            {
                base = 2;
                skip = 2;
            }
            else if (digits.size() > 1 && digits[0] == '0')
            {
                base = 8;
                skip = 1;
            }

            if (digits.size() <= skip)
            {
                return Value {};
            }

            std::uint64_t magnitude = 0;
            for (std::size_t k = skip; k < digits.size(); ++k)
            {
                const char c = digits[k];
                const int  digit =
                    c >= '0' && c <= '9'   ? c - '0'
                    : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                           : 99;
                if (digit >= base || magnitude > (UINT64_MAX - static_cast<std::uint64_t>(digit)) /
                                                     static_cast<std::uint64_t>(base))
                {
                    return Int(0, false, true, is_unsigned); // malformed or beyond 64 bits
                }

                magnitude = magnitude * static_cast<std::uint64_t>(base) +
                            static_cast<std::uint64_t>(digit);
            }

            if (longs > 0)
            {
                return Int(0, false, true, is_unsigned);
            }

            if (is_unsigned)
            {
                return magnitude <= static_cast<std::uint64_t>(kUIntMax)
                           ? Int(static_cast<std::int64_t>(magnitude), true, false, true)
                           : Int(0, false, true, true);
            }

            if (magnitude <= static_cast<std::uint64_t>(kIntMax))
            {
                return Int(static_cast<std::int64_t>(magnitude), true, false, false);
            }

            if (base != 10 && magnitude <= static_cast<std::uint64_t>(kUIntMax))
            {
                return Int(static_cast<std::int64_t>(magnitude), true, false, true);
            }

            return Int(0, false, true, false);
        }

        Value Logical(bool a_known, bool b_known, bool result) const
        {
            return Int(result ? 1 : 0, a_known && b_known, false, false);
        }

        Value Apply(Tok op, const Value& a, const Value& b) const
        {
            if (!a.ok || !b.ok)
            {
                return Value {};
            }

            const bool both = a.known && b.known;
            if (op == Tok::AmpAmp || op == Tok::PipePipe)
            {
                const bool result =
                    op == Tok::AmpAmp ? Truthy(a) && Truthy(b) : Truthy(a) || Truthy(b);
                return Logical(a.known, b.known, result);
            }

            const bool comparison = op == Tok::Lt || op == Tok::Gt || op == Tok::Le ||
                                    op == Tok::Ge || op == Tok::EqEq || op == Tok::BangEq;
            if (a.is_float || b.is_float)
            {
                if (comparison)
                {
                    const double x      = AsDouble(a);
                    const double y      = AsDouble(b);
                    bool         result = false;
                    switch (op)
                    {
                        case Tok::Lt:
                            result = x < y;
                            break;
                        case Tok::Gt:
                            result = x > y;
                            break;
                        case Tok::Le:
                            result = x <= y;
                            break;
                        case Tok::Ge:
                            result = x >= y;
                            break;
                        case Tok::EqEq:
                            result = x == y;
                            break;
                        default:
                            result = x != y;
                            break;
                    }

                    return Logical(a.known, b.known, result);
                }

                if (op != Tok::Plus && op != Tok::Minus && op != Tok::Star && op != Tok::Slash)
                {
                    return Value {};
                }

                if (op == Tok::Slash && b.known && AsDouble(b) == 0.0)
                {
                    return Value {};
                }

                const double x = AsDouble(a);
                const double y = AsDouble(b);
                const double r = op == Tok::Plus    ? x + y
                                 : op == Tok::Minus ? x - y
                                 : op == Tok::Star  ? x * y
                                                    : x / y;
                return Float(r, both);
            }

            // Shifts take the type of the left operand; everything else the usual
            // arithmetic conversions.
            if (op == Tok::Shl || op == Tok::Shr)
            {
                if (b.known && (b.i < 0 || b.i >= (a.wide ? 64 : 32)))
                {
                    return Value {};
                }

                if (!both || a.wide)
                {
                    return Int(0, false, a.wide, a.is_unsigned);
                }

                if (op == Tok::Shr)
                {
                    return Normalize(a.i >> b.i, true, false, a.is_unsigned);
                }

                const auto shifted = static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(As(a, a.is_unsigned)) << b.i);
                return a.is_unsigned
                           ? Normalize(static_cast<std::int64_t>(shifted), true, false, true)
                           : Normalize(
                                 static_cast<std::int64_t>(static_cast<std::int32_t>(shifted)),
                                 true, false, false);
            }

            const bool wide        = a.wide || b.wide;
            const bool is_unsigned = a.is_unsigned || b.is_unsigned;
            if (comparison)
            {
                if (!both || wide)
                {
                    return Int(0, false, false, false);
                }

                const auto x      = As(a, is_unsigned);
                const auto y      = As(b, is_unsigned);
                bool       result = false;
                switch (op)
                {
                    case Tok::Lt:
                        result = x < y;
                        break;
                    case Tok::Gt:
                        result = x > y;
                        break;
                    case Tok::Le:
                        result = x <= y;
                        break;
                    case Tok::Ge:
                        result = x >= y;
                        break;
                    case Tok::EqEq:
                        result = x == y;
                        break;
                    default:
                        result = x != y;
                        break;
                }

                return Int(result ? 1 : 0, true, false, false);
            }

            if (op == Tok::Slash || op == Tok::Percent)
            {
                if (b.known && b.i == 0)
                {
                    return Value {};
                }

                if (!both || wide)
                {
                    return Int(0, false, wide, is_unsigned);
                }

                const auto x = As(a, is_unsigned);
                const auto y = As(b, is_unsigned);
                if (y == 0)
                {
                    return Value {};
                }

                return Normalize(op == Tok::Slash ? x / y : x % y, true, false, is_unsigned);
            }

            if (!both || wide)
            {
                return Int(0, false, wide, is_unsigned);
            }

            const auto x = As(a, is_unsigned);
            const auto y = As(b, is_unsigned);
            switch (op)
            {
                case Tok::Plus:
                    return Normalize(x + y, true, false, is_unsigned);
                case Tok::Minus:
                    return Normalize(x - y, true, false, is_unsigned);
                case Tok::Star:
                    // Both operands fit in 32 bits, so the product fits in 64 unless two large
                    // unsigned values meet; those are left unknown.
                    if (std::llabs(x) > (1ll << 31) || std::llabs(y) > (1ll << 31))
                    {
                        return Int(0, false, false, is_unsigned);
                    }

                    return Normalize(x * y, true, false, is_unsigned);
                case Tok::Amp:
                    return Normalize(x & y, true, false, is_unsigned);
                case Tok::Pipe:
                    return Normalize(x | y, true, false, is_unsigned);
                case Tok::Caret:
                    return Normalize(x ^ y, true, false, is_unsigned);
                default:
                    return Value {};
            }
        }

        static int Precedence(Tok tok)
        {
            switch (tok)
            {
                case Tok::PipePipe:
                    return 1;
                case Tok::AmpAmp:
                    return 2;
                case Tok::Pipe:
                    return 3;
                case Tok::Caret:
                    return 4;
                case Tok::Amp:
                    return 5;
                case Tok::EqEq:
                case Tok::BangEq:
                    return 6;
                case Tok::Lt:
                case Tok::Gt:
                case Tok::Le:
                case Tok::Ge:
                    return 7;
                case Tok::Shl:
                case Tok::Shr:
                    return 8;
                case Tok::Plus:
                case Tok::Minus:
                    return 9;
                case Tok::Star:
                case Tok::Slash:
                case Tok::Percent:
                    return 10;
                default:
                    return 0;
            }
        }

        Value Ternary()
        {
            const Value condition = Binary(1);
            if (!condition.ok || m_pos >= m_end || m_view.At(m_pos) != Tok::Question)
            {
                return condition;
            }

            ++m_pos;
            const Value yes = Ternary();
            if (!yes.ok || m_pos >= m_end || m_view.At(m_pos) != Tok::Colon)
            {
                return Value {};
            }

            ++m_pos;
            const Value no = Ternary();
            if (!no.ok)
            {
                return Value {};
            }

            if (condition.known && yes.known && no.known && yes.is_float == no.is_float)
            {
                return Truthy(condition) ? yes : no;
            }

            if (yes.is_float || no.is_float)
            {
                return Float(0, false);
            }

            return Int(0, false, yes.wide || no.wide, yes.is_unsigned || no.is_unsigned);
        }

        Value Binary(int min_precedence)
        {
            Value left = Unary();
            while (left.ok && m_pos < m_end)
            {
                const Tok op         = m_view.At(m_pos);
                const int precedence = Precedence(op);
                if (precedence == 0 || precedence < min_precedence)
                {
                    break;
                }

                ++m_pos;
                const Value right = Binary(precedence + 1);
                left              = Apply(op, left, right);
            }

            return left;
        }

        Value Unary()
        {
            if (m_pos >= m_end)
            {
                return Value {};
            }

            const Tok tok = m_view.At(m_pos);
            if (tok == Tok::Minus || tok == Tok::Plus || tok == Tok::Bang || tok == Tok::Tilde)
            {
                ++m_pos;
                const Value operand = Unary();
                if (!operand.ok)
                {
                    return Value {};
                }

                if (tok == Tok::Bang)
                {
                    return Int(Truthy(operand) ? 0 : 1, operand.known, false, false);
                }

                if (tok == Tok::Plus)
                {
                    return operand;
                }

                if (operand.is_float)
                {
                    return tok == Tok::Minus ? Float(-operand.f, operand.known) : Value {};
                }

                if (!operand.known || operand.wide)
                {
                    return Int(0, false, operand.wide, operand.is_unsigned);
                }

                if (tok == Tok::Minus)
                {
                    return Normalize(-operand.i, true, false, operand.is_unsigned);
                }

                return Normalize(~operand.i, true, false, operand.is_unsigned);
            }

            return Primary();
        }

        Value Primary()
        {
            if (m_pos >= m_end)
            {
                return Value {};
            }

            const Tok tok = m_view.At(m_pos);
            if (tok == Tok::KwTrue || tok == Tok::KwFalse)
            {
                ++m_pos;
                return Int(tok == Tok::KwTrue ? 1 : 0, true, false, false);
            }

            if (m_view.KindAt(m_pos) == TokenKind::Number)
            {
                return Number(m_view.Text(m_pos++));
            }

            if (m_view.KindAt(m_pos) == TokenKind::CharacterLiteral)
            {
                ++m_pos;
                return Int(0, false, false, false); // a char promotes to int
            }

            if (tok == Tok::LParen)
            {
                ++m_pos;
                const Value inner = Ternary();
                if (!inner.ok || m_pos >= m_end || m_view.At(m_pos) != Tok::RParen)
                {
                    return Value {};
                }

                ++m_pos;
                // `(T)x` is a cast, not a parenthesized value.
                if (m_pos < m_end &&
                    (m_view.IsWord(m_pos) || m_view.KindAt(m_pos) == TokenKind::Number ||
                     m_view.At(m_pos) == Tok::LParen))
                {
                    return Value {};
                }

                return inner;
            }

            if (m_view.IsWord(m_pos))
            {
                return Name();
            }

            return Value {};
        }

        // An enumerator, a constant variable or a call to a constexpr function.
        Value Name()
        {
            const auto position = m_pos++;
            const auto symbol   = m_owner.m_model.ResolveToken(m_view.TokenAt(position));
            if (symbol == kNone)
            {
                return Value {};
            }

            const auto& symbols = m_owner.m_symbols;
            switch (symbols.kind[symbol])
            {
                case SymbolKind::Enumerator:
                    return Int(0, false, true, false); // the underlying type is not tracked
                case SymbolKind::Enum:
                    // `Color::Red`
                    if (m_pos + 1 < m_end && m_view.At(m_pos) == Tok::ColonColon &&
                        m_view.IsWord(m_pos + 1))
                    {
                        m_pos += 2;
                        return Int(0, false, true, false);
                    }

                    return Value {};
                case SymbolKind::Variable:
                    return m_owner.UsableInConstantExpression(symbol) ? Variable(symbol) : Value {};
                case SymbolKind::Function:
                    return Call(symbol);
                default:
                    return Value {};
            }
        }

        Value Variable(SymbolId symbol) const
        {
            const auto found = m_owner.m_values.find(symbol);
            return found == m_owner.m_values.end() ? Value {} : found->second;
        }

        Value Call(SymbolId function)
        {
            if (m_pos >= m_end || m_view.At(m_pos) != Tok::LParen ||
                !m_owner.CalleeUsable(function, kNone))
            {
                return Value {};
            }

            const auto close = m_view.Match(m_pos, m_end);
            if (close >= m_end)
            {
                return Value {};
            }

            ++m_pos;
            while (m_pos < close)
            {
                if (!Ternary().ok)
                {
                    return Value {};
                }

                if (m_pos < close)
                {
                    if (m_view.At(m_pos) != Tok::Comma)
                    {
                        return Value {};
                    }

                    ++m_pos;
                }
            }

            m_pos = close + 1;
            Value unknown;
            unknown.ok = true;
            return Convert(unknown, m_owner.m_types.SymbolType(function), m_owner.m_table);
        }

      public:
        // The value converted to what a variable or function result of `type` holds.
        static Value Convert(const Value& value, TypeId type, const TypeTable& table)
        {
            if (!value.ok)
            {
                return Value {};
            }

            const TypeId base = table.Strip(type);
            if (table.Kind(base) == TypeKind::Enum)
            {
                return Int(0, false, true, false);
            }

            if (table.Kind(base) != TypeKind::Builtin || !table.IsArithmetic(base))
            {
                return Value {};
            }

            switch (static_cast<BuiltinType>(table.Arg(base)))
            {
                case BuiltinType::Bool:
                    return Int(Truthy(value) ? 1 : 0, value.known, false, false);
                case BuiltinType::Float:
                case BuiltinType::Double:
                case BuiltinType::LongDouble:
                    return Float(AsDouble(value), value.known);
                case BuiltinType::Int: {
                    if (value.known && !value.wide && !value.is_float)
                    {
                        return Int(value.i, value.i >= kIntMin && value.i <= kIntMax, false, false);
                    }

                    if (value.known && value.is_float && value.f > -2147483649.0 &&
                        value.f < 2147483648.0)
                    {
                        return Int(static_cast<std::int64_t>(value.f), true, false, false);
                    }

                    return Int(0, false, false, false);
                }
                case BuiltinType::UInt:
                    if (value.known && !value.wide && !value.is_float)
                    {
                        return Int(static_cast<std::int64_t>(static_cast<std::uint32_t>(value.i)),
                                   true, false, true);
                    }

                    return Int(0, false, false, true);
                case BuiltinType::Short:
                case BuiltinType::UShort:
                case BuiltinType::Char:
                case BuiltinType::SChar:
                case BuiltinType::UChar:
                case BuiltinType::WChar:
                case BuiltinType::Char8:
                case BuiltinType::Char16:
                case BuiltinType::Char32:
                    return Int(0, false, false, false); // promotes to int; the value is not tracked
                default:
                    return Int(0, false, true,
                               false); // long, long long, size_t: width depends on the platform
            }
        }

      private:
        ConstantAnalysis& m_owner;
        const TokenView&  m_view;
        std::size_t       m_pos;
        std::size_t       m_end;
    };

    ConstantAnalysis::ConstantAnalysis(const FlowModel& flow) :
        m_flow(flow), m_types(flow.Types()), m_table(flow.Types().Types()), m_model(flow.Model()),
        m_symbols(flow.Model().Symbols()), m_view(flow.Model())
    {
        for (SymbolId symbol = 0; symbol < m_symbols.Size(); ++symbol)
        {
            if (m_symbols.kind[symbol] == SymbolKind::Variable ||
                m_symbols.kind[symbol] == SymbolKind::Parameter)
            {
                m_symbol_at.emplace(m_symbols.decl_token[symbol], symbol);
            }
        }
    }

    Specifiers ConstantAnalysis::SpecifiersOf(std::uint32_t decl_node,
                                              std::uint32_t name_token) const
    {
        Specifiers result;
        if (decl_node >= m_model.Tree().NodesSoA().size())
        {
            return result;
        }

        const auto first = m_view.Range(decl_node).first;
        const auto last  = m_view.PositionOf(name_token);
        for (auto p = first; p < last && p < m_view.Size(); ++p)
        {
            switch (m_view.At(p))
            {
                case Tok::KwStatic:
                    result.mask |= Spec::Static;
                    break;
                case Tok::KwConstexpr:
                    result.mask |= Spec::Constexpr;
                    break;
                case Tok::KwConstinit:
                    result.mask |= Spec::Constinit;
                    break;
                case Tok::KwConsteval:
                    result.mask |= Spec::Consteval;
                    break;
                case Tok::KwExtern:
                    result.mask |= Spec::Extern;
                    break;
                case Tok::KwThreadLocal:
                    result.mask |= Spec::ThreadLocal;
                    break;
                case Tok::KwMutable:
                    result.mask |= Spec::Mutable;
                    break;
                case Tok::KwVolatile:
                    result.mask |= Spec::Volatile;
                    break;
                case Tok::KwRegister:
                    result.mask |= Spec::Register;
                    break;
                case Tok::KwInline:
                    result.mask |= Spec::Inline;
                    break;
                case Tok::KwVirtual:
                    result.mask |= Spec::Virtual;
                    break;
                case Tok::KwFriend:
                    result.mask |= Spec::Friend;
                    break;
                case Tok::KwTypedef:
                    result.mask |= Spec::Typedef;
                    break;
                case Tok::KwExplicit:
                    result.mask |= Spec::Explicit;
                    break;
                case Tok::KwConst:
                    result.mask |= Spec::Const;
                    ++result.const_count;
                    result.const_position = p;
                    break;
                default:
                    break;
            }
        }

        return result;
    }

    bool ConstantAnalysis::InAnonymousNamespace(ScopeId scope) const
    {
        const auto& scopes = m_model.Scopes();
        for (; scope != kNone && scope != SemanticModel::TranslationUnitScope;
             scope = scopes.parent[scope])
        {
            if (scopes.kind[scope] == ScopeKind::Namespace && scopes.owner[scope] == kNone)
            {
                return true;
            }
        }

        return false;
    }

    // Types that may appear in a constexpr function or variable: arithmetic,
    // enums, pointers and arrays of those. Library and user classes are left out.
    bool ConstantAnalysis::Literal(TypeId type, bool allow_void) const
    {
        const TypeId base = m_table.Strip(type);
        switch (m_table.Kind(base))
        {
            case TypeKind::Builtin:
                return allow_void || !m_table.IsBuiltin(base, BuiltinType::Void);
            case TypeKind::Enum:
            case TypeKind::Pointer:
                return true;
            case TypeKind::Array:
                return Literal(m_table.Arg(base), false);
            default:
                return false;
        }
    }

    const std::vector<SymbolId>& ConstantAnalysis::LocalsOf(FunctionId function)
    {
        if (!m_locals_built)
        {
            m_locals.assign(m_flow.Functions().Size(), {});
            for (SymbolId symbol = 0; symbol < m_symbols.Size(); ++symbol)
            {
                if (const auto owner = m_flow.OwnerOf(symbol); owner != kNone)
                {
                    m_locals[owner].push_back(symbol);
                }
            }

            m_locals_built = true;
        }

        static const std::vector<SymbolId> kEmpty;
        return function < m_locals.size() ? m_locals[function] : kEmpty;
    }

    // `= expr` or `{expr}` after the name: the tokens of the expression.
    bool ConstantAnalysis::InitializerRange(SymbolId variable, std::size_t& begin,
                                            std::size_t& end) const
    {
        const auto name     = m_symbols.decl_token[variable];
        const auto position = m_view.PositionOf(name);
        if (position >= m_view.Size() || m_view.TokenAt(position) != name)
        {
            return false;
        }

        const Tok next = m_view.At(position + 1);
        if (next == Tok::LBrace)
        {
            const auto close = m_view.Match(position + 1, m_view.Size());
            begin            = position + 2;
            end              = close;
            return close < m_view.Size() && end > begin;
        }

        if (next != Tok::Eq)
        {
            return false;
        }

        begin = position + 2;
        if (m_view.At(begin) == Tok::LBrace)
        {
            const auto close = m_view.Match(begin, m_view.Size());
            ++begin;
            end = close;
            return close < m_view.Size() && end > begin;
        }

        std::size_t depth = 0;
        for (end = begin; end < m_view.Size(); ++end)
        {
            const Tok tok = m_view.At(end);
            if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace)
            {
                ++depth;
            }
            else if (tok == Tok::RParen || tok == Tok::RBracket || tok == Tok::RBrace)
            {
                if (depth == 0)
                {
                    break;
                }

                --depth;
            }
            else if ((tok == Tok::Comma || tok == Tok::Semi) && depth == 0)
            {
                break;
            }
        }

        return end < m_view.Size() && end > begin;
    }

    bool ConstantAnalysis::ConstantInitializer(SymbolId variable)
    {
        if (const auto found = m_initializer.find(variable); found != m_initializer.end())
        {
            return found->second == State::Yes;
        }

        m_initializer[variable] = State::Visiting; // a cycle (`const int a = a;`) is not constant
        bool        ok          = false;
        Value       value;
        std::size_t begin = 0;
        std::size_t end   = 0;
        if (variable < m_symbols.Size() && m_symbols.kind[variable] == SymbolKind::Variable)
        {
            const auto type = m_types.SymbolType(variable);
            const auto base = m_table.Strip(type);
            if ((m_table.IsArithmetic(base) || m_table.Kind(base) == TypeKind::Enum) &&
                InitializerRange(variable, begin, end))
            {
                Parser parser(*this, begin, end);
                value = Parser::Convert(parser.Run(), type, m_table);
                ok    = value.ok;
            }
        }

        m_initializer[variable] = ok ? State::Yes : State::No;
        if (ok)
        {
            m_values[variable] = value;
        }

        return ok;
    }

    // Readable inside a constant expression: `constexpr`, or `const` of an integral
    // or enumeration type, with a constant initializer.
    bool ConstantAnalysis::UsableInConstantExpression(SymbolId variable)
    {
        if (variable >= m_symbols.Size() || m_symbols.kind[variable] != SymbolKind::Variable ||
            !ConstantInitializer(variable))
        {
            return false;
        }

        const auto spec =
            SpecifiersOf(m_symbols.decl_node[variable], m_symbols.decl_token[variable]);
        if ((spec.mask & Spec::Constexpr) != 0)
        {
            return true;
        }

        const auto type = m_types.SymbolType(variable);
        if (m_table.Kind(type) != TypeKind::Const)
        {
            return false;
        }

        const auto inner = m_table.Arg(type);
        return m_table.IsInteger(inner) || m_table.IsBool(inner) ||
               m_table.Kind(inner) == TypeKind::Enum;
    }

    bool ConstantAnalysis::DeclaredConstexpr(SymbolId function) const
    {
        for (auto other = m_model.LookupLocal(m_symbols.scope[function], m_symbols.name[function]);
             other != kNone;
             other = m_symbols.next_same_name[other])
        {
            if (m_symbols.kind[other] == SymbolKind::Function &&
                (other == function ||
                 m_symbols.signature[other] == m_symbols.signature[function]) &&
                (SpecifiersOf(m_symbols.decl_node[other], m_symbols.decl_token[other]).mask &
                 (Spec::Constexpr | Spec::Consteval)) != 0)
            {
                return true;
            }
        }

        return false;
    }

    // Every overload that a call by this name could pick is, or will be, constexpr.
    bool ConstantAnalysis::CalleeUsable(SymbolId callee, SymbolId current)
    {
        bool any = false;
        for (auto other = m_model.LookupLocal(m_symbols.scope[callee], m_symbols.name[callee]);
             other != kNone;
             other = m_symbols.next_same_name[other])
        {
            if (m_symbols.kind[other] != SymbolKind::Function)
            {
                return false;
            }

            any = true;
            if (other == current || DeclaredConstexpr(other))
            {
                continue;
            }

            const auto found = m_function.find(other);
            if (found != m_function.end() && found->second == State::Visiting)
            {
                return false; // mutual recursion: not assumed
            }

            if (!CouldBeConstexpr(other))
            {
                return false;
            }
        }

        return any;
    }

    bool ConstantAnalysis::EligibleFunction(SymbolId function)
    {
        return function < m_symbols.Size() && !DeclaredConstexpr(function) &&
               CouldBeConstexpr(function);
    }

    bool ConstantAnalysis::CouldBeConstexpr(SymbolId function)
    {
        if (const auto found = m_function.find(function); found != m_function.end())
        {
            return found->second == State::Yes;
        }

        m_function[function] = State::Visiting;
        bool ok              = false;
        do
        {
            constexpr std::uint32_t kExcluded =
                SymbolFlag::Virtual | SymbolFlag::Constructor | SymbolFlag::Destructor |
                SymbolFlag::Qualified | SymbolFlag::Friend | SymbolFlag::Operator |
                SymbolFlag::Template | SymbolFlag::Override | SymbolFlag::Final | SymbolFlag::Pure |
                SymbolFlag::Defaulted;
            if (m_symbols.kind[function] != SymbolKind::Function ||
                (m_symbols.flags[function] & SymbolFlag::Definition) == 0 ||
                (m_symbols.flags[function] & kExcluded) != 0 ||
                m_model.Names().Text(m_symbols.name[function]) == "main")
            {
                break;
            }

            const auto id = m_flow.FunctionOfNode(m_symbols.decl_node[function]);
            if (id == kNone)
            {
                break;
            }

            const auto spec =
                SpecifiersOf(m_symbols.decl_node[function], m_symbols.decl_token[function]);
            if ((spec.mask & (Spec::Virtual | Spec::Friend | Spec::Extern | Spec::Explicit |
                              Spec::Opaque)) != 0)
            {
                break;
            }

            // constexpr implies inline: only a function whose linkage is internal, that
            // is already inline, or that is a static member can take it without
            // breaking callers in other files.
            const auto& scopes     = m_model.Scopes();
            const auto  scope      = m_symbols.scope[function];
            bool        linkage_ok = false;
            if (scopes.kind[scope] == ScopeKind::Class)
            {
                linkage_ok = (m_symbols.flags[function] & SymbolFlag::Static) != 0 ||
                             (spec.mask & Spec::Static) != 0;
            }
            else if (scopes.kind[scope] == ScopeKind::TranslationUnit ||
                     scopes.kind[scope] == ScopeKind::Namespace)
            {
                linkage_ok =
                    (spec.mask & (Spec::Static | Spec::Inline)) != 0 || InAnonymousNamespace(scope);
            }

            bool in_template = false;
            for (auto up = scope; up != kNone && up != SemanticModel::TranslationUnitScope;
                 up      = scopes.parent[up])
            {
                if (scopes.kind[up] == ScopeKind::Class && scopes.owner[up] != kNone &&
                    (m_symbols.flags[scopes.owner[up]] & SymbolFlag::Template) != 0)
                {
                    in_template = true;
                }
            }

            if (!linkage_ok || in_template)
            {
                break;
            }

            // Another declaration of the same function would need `constexpr` too.
            bool redeclared = false;
            for (auto other = m_model.LookupLocal(scope, m_symbols.name[function]); other != kNone;
                 other      = m_symbols.next_same_name[other])
            {
                redeclared = redeclared ||
                             (other != function && m_symbols.kind[other] == SymbolKind::Function &&
                              m_symbols.signature[other] == m_symbols.signature[function]);
            }

            if (redeclared || !Literal(m_types.SymbolType(function), true))
            {
                break;
            }

            // Parameters: every written one has a name and a literal type.
            const auto& nodes   = m_model.Tree().NodesSoA();
            std::size_t written = 0;
            for (const auto declarator : m_model.ChildrenOf(m_symbols.decl_node[function]))
            {
                if (nodes.Kind(declarator) != GrammarKind::Declarator)
                {
                    continue;
                }

                for (const auto suffix : m_model.ChildrenOf(declarator))
                {
                    if (nodes.Kind(suffix) != GrammarKind::FunctionSuffix)
                    {
                        continue;
                    }

                    for (const auto parameter : m_model.ChildrenOf(suffix))
                    {
                        written +=
                            nodes.Kind(parameter) == GrammarKind::ParameterDeclaration ? 1 : 0;
                    }
                }
            }

            std::size_t named              = 0;
            bool        literal_parameters = true;
            for (const auto local : LocalsOf(id))
            {
                if (m_symbols.kind[local] == SymbolKind::Parameter)
                {
                    ++named;
                    literal_parameters =
                        literal_parameters && Literal(m_types.SymbolType(local), false);
                }
            }

            if (!literal_parameters || named != written)
            {
                break;
            }

            ok = BodyIsConstexprSafe(function, id);
        } while (false);

        m_function[function] = ok ? State::Yes : State::No;
        return ok;
    }

    bool ConstantAnalysis::BodyIsConstexprSafe(SymbolId function, FunctionId id)
    {
        const auto& functions = m_flow.Functions();
        const auto& nodes     = m_model.Tree().NodesSoA();
        if (functions.complete[id] == 0 || !m_flow.ExitReachable(id))
        {
            return false;
        }

        const bool returns_void =
            m_table.IsBuiltin(m_table.Strip(m_types.SymbolType(function)), BuiltinType::Void);
        if (!returns_void && !m_flow.HasReachableReturn(id))
        {
            return false;
        }

        const auto [begin, end] = m_view.Range(functions.body[id]);
        if (end < begin + 3)
        {
            return false; // `{}`: nothing to evaluate
        }

        // Positions inside a type: their names are not expressions.
        std::vector<std::uint8_t>  in_type(end - begin, 0);
        std::vector<std::uint32_t> pending { functions.body[id] };
        while (!pending.empty())
        {
            const auto node = pending.back();
            pending.pop_back();
            switch (nodes.Kind(node))
            {
                case GrammarKind::CompoundStatement:
                case GrammarKind::DeclarationStatement:
                case GrammarKind::InitDeclarator:
                case GrammarKind::Declarator:
                case GrammarKind::DeclaredName:
                case GrammarKind::PointerOperator:
                case GrammarKind::ArraySuffix:
                case GrammarKind::ExpressionStatement:
                case GrammarKind::ReturnStatement:
                case GrammarKind::IfStatement:
                case GrammarKind::LoopStatement:
                case GrammarKind::DoStatement:
                case GrammarKind::SwitchStatement:
                case GrammarKind::CaseLabel:
                case GrammarKind::JumpStatement:
                case GrammarKind::EmptyStatement:
                case GrammarKind::IdentifierExpression:
                case GrammarKind::LiteralExpression:
                case GrammarKind::ParenthesizedExpression:
                case GrammarKind::UnaryExpression:
                case GrammarKind::BinaryExpression:
                case GrammarKind::ConditionalExpression:
                case GrammarKind::CallExpression:
                case GrammarKind::SubscriptExpression:
                case GrammarKind::MemberExpression:
                case GrammarKind::TemplateIdExpression:
                case GrammarKind::TemplateArgument:
                    break;
                case GrammarKind::TypeSpecifier:
                case GrammarKind::NestedNameSpecifier: {
                    const auto [first, last] = m_view.Range(node);
                    for (auto p = std::max(first, begin); p < last && p < end; ++p)
                    {
                        in_type[p - begin] = 1;
                    }

                    break;
                }
                default:
                    return false; // lambda, try, error, template, ...: not modeled
            }

            for (const auto child : m_model.ChildrenOf(node))
            {
                pending.push_back(child);
            }
        }

        for (auto p = begin; p < end; ++p)
        {
            const Tok tok = m_view.At(p);
            switch (tok)
            {
                case Tok::KwNew:
                case Tok::KwDelete:
                case Tok::KwThrow:
                case Tok::KwTry:
                case Tok::KwGoto:
                case Tok::KwAsm:
                case Tok::KwStatic:
                case Tok::KwThreadLocal:
                case Tok::KwReinterpretCast:
                case Tok::KwDynamicCast:
                case Tok::KwConstCast:
                case Tok::KwTypeid:
                case Tok::KwThis:
                case Tok::KwVolatile:
                case Tok::KwCoAwait:
                case Tok::KwCoReturn:
                case Tok::KwCoYield:
                case Tok::Dot:
                case Tok::Arrow:
                case Tok::DotStar:
                case Tok::ArrowStar:
                case Tok::Ellipsis:
                    return false;
                case Tok::KwStaticCast: {
                    // Only a conversion to a builtin type.
                    auto q = p + 1;
                    if (m_view.At(q) != Tok::Lt)
                    {
                        return false;
                    }

                    for (++q; q < end && m_view.At(q) != Tok::Gt; ++q)
                    {
                        if (!IsTypeKeyword(m_view.At(q)))
                        {
                            return false;
                        }
                    }

                    if (q >= end || m_view.At(q + 1) != Tok::LParen)
                    {
                        return false;
                    }

                    break;
                }
                default:
                    break;
            }

            if (m_view.KindAt(p) == TokenKind::StringLiteral ||
                m_view.KindAt(p) == TokenKind::RawStringLiteral)
            {
                return false;
            }

            if (!m_view.IsWord(p) || in_type[p - begin] != 0)
            {
                continue;
            }

            const auto token = m_view.TokenAt(p);
            if (m_symbol_at.contains(token))
            {
                continue; // a name this body declares
            }

            const auto symbol = m_model.ResolveToken(token);
            if (symbol == kNone)
            {
                // `Color::Red`: an enumerator of a scoped enumeration.
                bool enumerator = false;
                if (p >= begin + 2 && m_view.At(p - 1) == Tok::ColonColon && m_view.IsWord(p - 2))
                {
                    const auto owner = m_model.ResolveToken(m_view.TokenAt(p - 2));
                    enumerator       = owner != kNone && m_symbols.kind[owner] == SymbolKind::Enum;
                }

                if (!enumerator)
                {
                    return false;
                }

                continue;
            }

            switch (m_symbols.kind[symbol])
            {
                case SymbolKind::Variable:
                case SymbolKind::Parameter:
                    if (m_flow.OwnerOf(symbol) != id && !UsableInConstantExpression(symbol))
                    {
                        return false; // a global that is not constant
                    }

                    break;
                case SymbolKind::Enumerator:
                    break;
                case SymbolKind::Function:
                    if (!CalleeUsable(symbol, function))
                    {
                        return false;
                    }

                    break;
                case SymbolKind::TypeAlias:
                    if (!Literal(m_types.SymbolType(symbol), false))
                    {
                        return false;
                    }

                    break;
                case SymbolKind::Enum:
                    if (m_view.At(p + 1) != Tok::ColonColon)
                    {
                        return false;
                    }

                    break;
                default:
                    return false;
            }
        }

        for (const auto local : LocalsOf(id))
        {
            if (m_symbols.kind[local] != SymbolKind::Variable)
            {
                continue;
            }

            const auto spec = SpecifiersOf(m_symbols.decl_node[local], m_symbols.decl_token[local]);
            if (!Literal(m_types.SymbolType(local), false) ||
                (spec.mask & (Spec::Static | Spec::ThreadLocal | Spec::Volatile | Spec::Mutable |
                              Spec::Register | Spec::Extern)) != 0)
            {
                return false;
            }
        }

        return true;
    }

    bool ConstantAnalysis::DeclaresSingleName(SymbolId variable) const
    {
        const auto  node  = m_symbols.decl_node[variable];
        const auto& nodes = m_model.Tree().NodesSoA();
        if (node >= nodes.size())
        {
            return false;
        }

        std::size_t names = 0;
        for (const auto child : m_model.ChildrenOf(node))
        {
            names += nodes.Kind(child) == GrammarKind::InitDeclarator ||
                             nodes.Kind(child) == GrammarKind::Declarator
                         ? 1
                         : 0;
        }

        return names == 1;
    }

    ConstantAnalysis::Candidate ConstantAnalysis::ConstexprVariable(SymbolId variable)
    {
        if (variable >= m_symbols.Size() || m_symbols.kind[variable] != SymbolKind::Variable ||
            m_symbols.decl_node[variable] >= m_model.Tree().NodesSoA().size() ||
            !DeclaresSingleName(variable))
        {
            return Candidate::None;
        }

        const auto& nodes  = m_model.Tree().NodesSoA();
        const auto  decl   = m_symbols.decl_node[variable];
        const auto  parent = nodes.Parent(decl);
        if (parent < nodes.size() && (nodes.Kind(parent) == GrammarKind::LoopStatement ||
                                      nodes.Kind(parent) == GrammarKind::IfStatement ||
                                      nodes.Kind(parent) == GrammarKind::SwitchStatement))
        {
            return Candidate::None; // declared in a statement header
        }

        const auto spec = SpecifiersOf(decl, m_symbols.decl_token[variable]);
        if ((spec.mask & (Spec::Constexpr | Spec::Consteval | Spec::Opaque)) != 0)
        {
            return Candidate::None;
        }

        const auto& scopes     = m_model.Scopes();
        const auto  scope_kind = scopes.kind[m_symbols.scope[variable]];
        if (scope_kind == ScopeKind::Class && (spec.mask & Spec::Static) == 0)
        {
            return Candidate::None; // a data member cannot be constexpr unless static
        }

        const auto type = m_types.SymbolType(variable);
        if (m_table.Kind(type) == TypeKind::Const)
        {
            const auto inner = m_table.Arg(type);
            if ((m_table.IsArithmetic(inner) || m_table.Kind(inner) == TypeKind::Enum) &&
                spec.const_count == 1 && ConstantInitializer(variable))
            {
                return Candidate::ReplaceConst;
            }

            return Candidate::None;
        }

        if ((m_table.IsArithmetic(type) || m_table.Kind(type) == TypeKind::Enum) &&
            (scope_kind == ScopeKind::Function || scope_kind == ScopeKind::Block) &&
            m_flow.IsNeverModified(variable) && ConstantInitializer(variable))
        {
            return Candidate::InsertConstexpr;
        }

        return Candidate::None;
    }

} // namespace heimdall::detail
