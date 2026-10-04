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

        std::vector<Token> Lex() const;
        std::string_view Text(const Token &token) const noexcept;

    private:
        std::string_view m_source;
    };

} // namespace heimdall
