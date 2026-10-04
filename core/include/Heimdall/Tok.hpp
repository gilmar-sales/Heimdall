#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace heimdall
{

    // Classification of keywords and punctuators, assigned once by the lexer so
    // the parser compares a byte instead of running memcmp on the token text.
    // Tokens that are not a keyword or punctuator (plain identifiers, literals,
    // trivia) are Tok::None.
#define HEIMDALL_TOK_LIST(X) \
    X(LBrace, "{") X(RBrace, "}") X(LBracket, "[") X(RBracket, "]") X(LParen, "(") X(RParen, ")") \
    X(Hash, "#") X(HashHash, "##") X(Lt, "<") X(Gt, ">") X(Percent, "%") X(Colon, ":") \
    X(ColonColon, "::") X(Semi, ";") X(Dot, ".") X(Ellipsis, "...") X(DotStar, ".*") X(Question, "?") \
    X(Star, "*") X(Plus, "+") X(Minus, "-") X(Slash, "/") X(Caret, "^") X(Amp, "&") X(Pipe, "|") \
    X(Tilde, "~") X(Bang, "!") X(Eq, "=") X(Comma, ",") X(Backslash, "\\") \
    X(DigraphHash, "%:") X(DigraphHashHash, "%:%:") X(PercentEq, "%=") X(DigraphRBrace, "%>") \
    X(DigraphRBracket, ":>") X(DigraphLBracket, "<:") X(DigraphLBrace, "<%") \
    X(Arrow, "->") X(ArrowStar, "->*") X(MinusMinus, "--") X(MinusEq, "-=") \
    X(Spaceship, "<=>") X(ShlEq, "<<=") X(Shl, "<<") X(Le, "<=") X(ShrEq, ">>=") X(Shr, ">>") X(Ge, ">=") \
    X(PlusPlus, "++") X(PlusEq, "+=") X(StarEq, "*=") X(SlashEq, "/=") X(AmpAmp, "&&") X(AmpEq, "&=") \
    X(PipePipe, "||") X(PipeEq, "|=") X(CaretEq, "^=") X(EqEq, "==") X(BangEq, "!=") \
    X(KwAlignas, "alignas") X(KwAlignof, "alignof") X(KwAsm, "asm") X(KwAuto, "auto") X(KwBool, "bool") \
    X(KwBreak, "break") X(KwCase, "case") X(KwCatch, "catch") X(KwChar, "char") X(KwChar8, "char8_t") \
    X(KwChar16, "char16_t") X(KwChar32, "char32_t") X(KwClass, "class") X(KwConcept, "concept") \
    X(KwConst, "const") X(KwConsteval, "consteval") X(KwConstexpr, "constexpr") X(KwConstinit, "constinit") \
    X(KwConstCast, "const_cast") X(KwContinue, "continue") X(KwCoAwait, "co_await") \
    X(KwCoReturn, "co_return") X(KwCoYield, "co_yield") X(KwDecltype, "decltype") X(KwDefault, "default") \
    X(KwDelete, "delete") X(KwDo, "do") X(KwDouble, "double") X(KwDynamicCast, "dynamic_cast") \
    X(KwElse, "else") X(KwEnum, "enum") X(KwExplicit, "explicit") X(KwExport, "export") \
    X(KwExtern, "extern") X(KwFalse, "false") X(KwFinal, "final") X(KwFloat, "float") X(KwFor, "for") \
    X(KwFriend, "friend") X(KwGoto, "goto") X(KwIf, "if") X(KwImport, "import") X(KwInline, "inline") \
    X(KwInt, "int") X(KwLong, "long") X(KwModule, "module") X(KwMutable, "mutable") \
    X(KwNamespace, "namespace") X(KwNew, "new") X(KwNoexcept, "noexcept") X(KwNullptr, "nullptr") \
    X(KwOperator, "operator") X(KwOverride, "override") X(KwPrivate, "private") \
    X(KwProtected, "protected") X(KwPublic, "public") X(KwRegister, "register") \
    X(KwReinterpretCast, "reinterpret_cast") X(KwRequires, "requires") X(KwReturn, "return") \
    X(KwShort, "short") X(KwSigned, "signed") X(KwSizeof, "sizeof") X(KwStatic, "static") \
    X(KwStaticAssert, "static_assert") X(KwStaticCast, "static_cast") X(KwStruct, "struct") \
    X(KwSwitch, "switch") X(KwTemplate, "template") X(KwThis, "this") X(KwThreadLocal, "thread_local") \
    X(KwThrow, "throw") X(KwTrue, "true") X(KwTry, "try") X(KwTypedef, "typedef") X(KwTypeid, "typeid") \
    X(KwTypename, "typename") X(KwUnion, "union") X(KwUnsigned, "unsigned") X(KwUsing, "using") \
    X(KwVirtual, "virtual") X(KwVoid, "void") X(KwVolatile, "volatile") X(KwWchar, "wchar_t") \
    X(KwWhile, "while")

    enum class Tok : std::uint8_t
    {
        None = 0,
#define HEIMDALL_TOK_ENUM(name, text) name,
        HEIMDALL_TOK_LIST(HEIMDALL_TOK_ENUM)
#undef HEIMDALL_TOK_ENUM
    };

    namespace tok_detail
    {

        constexpr std::uint32_t kFNVOffsetBasis = 2166136261u;
        constexpr std::uint32_t kFNVPrime = 16777619u;
        constexpr std::size_t kAlphabetSize = 26;
        constexpr std::size_t kByteValueCount = 256;

        struct Entry
        {
            std::string_view text;
            Tok tok = Tok::None;
        };

        constexpr std::size_t kTableSize = 512; // power of two, load factor < 0.4

        constexpr std::size_t Hash(std::string_view text) noexcept
        {
            std::uint32_t h = kFNVOffsetBasis;
            for (const char c: text)
            {
                h = (h ^ static_cast<unsigned char>(c)) * kFNVPrime;
            }

            return h;
        }

        // Open-addressing table built at compile time; a lookup is one hash and,
        // almost always, one comparison.
        constexpr std::array<Entry, kTableSize> BuildTable()
        {
            std::array<Entry, kTableSize> table{};
            const auto insert =[&table](std::string_view text, Tok tok)
            {
                std::size_t slot = Hash(text) & (kTableSize - 1);
                while (table[slot].tok != Tok::None)
                {
                    slot = (slot + 1) & (kTableSize - 1);
                }

                table[slot] = {text, tok};
            };
#define HEIMDALL_TOK_INSERT(name, text) insert(text, Tok::name);
            HEIMDALL_TOK_LIST(HEIMDALL_TOK_INSERT)
#undef HEIMDALL_TOK_INSERT
            return table;
        }

        inline constexpr std::array<Entry, kTableSize> kTable = BuildTable();

    } // namespace tok_detail

    namespace tok_detail
    {

        // Bit n of [first letter] is set when some keyword of length n starts with
        // that letter: rejects most identifiers without hashing them.
        constexpr std::array<std::uint32_t, kAlphabetSize> BuildKeywordShapes()
        {
            std::array<std::uint32_t, kAlphabetSize> shapes{};
            for (const auto & entry: BuildTable())
            {
                if (entry.tok != Tok::None && entry.text[0] >= 'a' && entry.text[0] <= 'z')
                {
                    shapes[static_cast<std::size_t>(entry.text[0] - 'a')] |= 1u << entry.text.size();
                }
            }

            return shapes;
        }

        inline constexpr std::array<std::uint32_t, kAlphabetSize> kKeywordShapes = BuildKeywordShapes();

    } // namespace tok_detail

    namespace tok_detail
    {

        constexpr std::array<Tok, kByteValueCount> BuildSingleChar()
        {
            std::array<Tok, kByteValueCount> table{};
            for (const auto & entry: BuildTable())
            {
                if (entry.tok != Tok::None && entry.text.size() == 1)
                {
                    table[static_cast<unsigned char>(entry.text[0])] = entry.tok;
                }
            }

            return table;
        }

        inline constexpr std::array<Tok, kByteValueCount> kSingleChar = BuildSingleChar();

    } // namespace tok_detail

    // One-character punctuators resolve with a single table load.
    constexpr Tok SingleCharTok(char c) noexcept
    {
        return tok_detail::kSingleChar[static_cast<unsigned char>(c)];
    }

    // True when an identifier of this shape could be a keyword.
    constexpr bool MayBeKeyword(std::string_view text) noexcept
    {
        constexpr std::size_t kMinKeywordLen = 2;
        constexpr std::size_t kMaxKeywordShapeLen = 32;
        return text.size() >= kMinKeywordLen && text.size() < kMaxKeywordShapeLen && text[0] >= 'a' &&
            text[0] <= 'z' &&
            ((tok_detail::kKeywordShapes[static_cast<std::size_t>(text[0] - 'a')] >> text.size()) & 1u) != 0;
    }

    // Tok::None when `text` is not a keyword or punctuator.
    constexpr Tok LookupTok(std::string_view text) noexcept
    {
        constexpr std::size_t kMaxTokLookupLen = 16;
        if (text.empty() || text.size() > kMaxTokLookupLen)
        {
            return Tok::None;
        }

        std::size_t slot = tok_detail::Hash(text) & (tok_detail::kTableSize - 1);
        while (tok_detail::kTable[slot].tok != Tok::None)
        {
            if (tok_detail::kTable[slot].text == text)
            {
                return tok_detail::kTable[slot].tok;
            }

            slot = (slot + 1) & (tok_detail::kTableSize - 1);
        }

        return Tok::None;
    }

    // A string literal resolved to its Tok at compile time, so
    // `Is(i, "template")` becomes a one-byte comparison. Literals that are not
    // keywords or punctuators keep `tok == Tok::None` and are compared by text.
    struct TokLiteral
    {
        template <std::size_t N>
        consteval TokLiteral(const char(&text)[N]) : tok(LookupTok({text, N - 1})), text(text, N - 1) {}

        Tok tok;
        std::string_view text;
    };

} // namespace heimdall
