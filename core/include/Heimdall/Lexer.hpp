#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <Heimdall/Tok.hpp>

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
        Tok tok = Tok::None;
        std::uint32_t offset = 0;
        std::uint32_t length = 0;
    };

    static_assert(sizeof(Token) == 12);

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

        // Merges two consecutive edits (`second` is expressed in the text that
        // `first` produced) into one edit, relative to the text before `first`,
        // that covers both.
        static TextEdit Compose(const TextEdit &first, const TextEdit &second) noexcept
        {
            const std::size_t first_new_end = first.offset + first.new_length;
            const std::size_t second_end = second.offset + second.old_length;
            const std::size_t offset = first.offset < second.offset ? first.offset : second.offset;
            // Where the merged region ends, in the text before `first`...
            const std::size_t old_end = second_end > first_new_end
            ? second_end - first.new_length + first.old_length
            : first.offset + first.old_length;
            // ...and in the text after `second`.
            const std::size_t new_end = (first_new_end > second_end ? first_new_end : second_end) +
                second.new_length - second.old_length;
            return {offset, old_end - offset, new_end - offset};
        }

        std::vector<Token> Lex() const;
        // Incremental re-lex. `tokens` must be the tokens of the text before
        // `edit`; the lexer's source is the text after it. Only a window around
        // the edit is re-scanned and the tokens after it are shifted, yielding
        // exactly what Lex() would produce.
        void Relex(std::vector<Token> & tokens, const TextEdit &edit) const;
        std::string_view Text(const Token &token) const noexcept;

    private:
        Token ScanToken(std::size_t start) const;
        std::string_view m_source;
    };

} // namespace heimdall
