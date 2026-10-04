#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace heimdall
{

    enum class TokenKind : std::uint8_t
    {
        Whitespace,
        Identifier,
        Number,
        StringLiteral,
        CharacterLiteral,
        RawStringLiteral,
        LineComment,
        BlockComment,
        Punctuation,
        Unknown
    };

    struct Token
    {
        TokenKind kind;
        std::uint32_t offset;
        std::uint32_t length;
    };

    class Lexer
    {
    public:
        explicit Lexer(std::string_view source) : m_source(source) {}

        // A replacement of `old_length` bytes at `offset` by `new_length` bytes.
        struct TextEdit
        {
            std::size_t offset = 0;
            std::size_t old_length = 0;
            std::size_t new_length = 0;
        };

        std::vector<Token> Lex() const;
        // Incremental re-lex. `tokens` must be the tokens of the text before
        // `edit`; the lexer's source is the text after it. Only a window around
        // the edit is re-scanned and the tokens after it are shifted, yielding
        // exactly what Lex() would produce.
        void Relex(std::vector<Token> &tokens, const TextEdit &edit) const;
        std::string_view Text(const Token &token) const noexcept;

    private:
        Token ScanToken(std::size_t start) const;
        std::string_view m_source;
    };

} // namespace heimdall
