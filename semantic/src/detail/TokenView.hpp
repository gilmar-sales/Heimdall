#pragma once

#include <Heimdall/SemanticModel.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace heimdall::detail
{

    // The model's tokens, with the helpers every rule below needs.
    class TokenView
    {
    public:
        explicit TokenView(const SemanticModel& model)
        : m_model(model), m_tokens(model.Tree().Tokens()), m_sig(model.Significant()) {}

        std::size_t Size() const
        {
            return m_sig.size();
        }

        std::uint32_t TokenAt(std::size_t position) const
        {
            return m_sig[position];
        }

        Tok At(std::size_t position) const
        {
            return position < m_sig.size() ? m_tokens[m_sig[position]].tok : Tok::None;
        }

        bool IsWord(std::size_t position) const
        {
            return position < m_sig.size() && m_tokens[m_sig[position]].kind == TokenKind::Identifier &&
                m_tokens[m_sig[position]].tok == Tok::None;
        }

        std::string_view Text(std::size_t position) const
        {
            return position < m_sig.size() ? m_model.Tree().Text(m_tokens[m_sig[position]]) : std::string_view {};
        }

        TokenKind KindAt(std::size_t position) const
        {
            return position < m_sig.size() ? m_tokens[m_sig[position]].kind : TokenKind::Unknown;
        }

        bool IsLiteralToken(std::size_t position) const
        {
            if (position >= m_sig.size())
            {
                return false;
            }

            const auto kind = m_tokens[m_sig[position]].kind;
            return kind == TokenKind::Number || kind == TokenKind::StringLiteral ||
                kind == TokenKind::CharacterLiteral || kind == TokenKind::RawStringLiteral;
        }

        std::size_t Offset(std::size_t position) const
        {
            return m_tokens[m_sig[position]].offset;
        }

        std::size_t End(std::size_t position) const
        {
            return m_tokens[m_sig[position]].offset + m_tokens[m_sig[position]].length;
        }

        // First position holding raw token `token` or a later one.
        std::size_t PositionOf(std::uint32_t token) const
        {
            return static_cast<std::size_t>(std::lower_bound(m_sig.begin(), m_sig.end(),
                token) - m_sig.begin());
        }

        // [begin, end) positions of a node's significant tokens.
        std::pair<std::size_t, std::size_t> Range(std::uint32_t node) const
        {
            const auto n = m_model.Tree().NodesSoA()[node];
            const auto begin = PositionOf(n.GetFirstToken());
            return {begin, std::max(begin, PositionOf(n.GetFirstToken() + n.GetTokenCount()))};
        }

        // `0`, `0L`, `0u`, `0UL`...: a null pointer constant.
        bool IsZeroLiteral(std::size_t position) const
        {
            if (position >= m_sig.size() || m_tokens[m_sig[position]].kind != TokenKind::Number)
            {
                return false;
            }

            const auto text = Text(position);
            return!text.empty() && text[0] == '0' &&
                text.find_first_not_of("uUlL", 1) == std::string_view::npos;
        }

        // Position of the bracket closing the one at `open`, or `limit`.
        std::size_t Match(std::size_t open, std::size_t limit) const
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

        // Position of the `>` closing the `<` at `open`, or `limit`; `>>` is not
        // split, so nested templates ending in `>>` give up.
        std::size_t MatchAngle(std::size_t open, std::size_t limit) const
        {
            std::size_t depth = 0;
            for (std::size_t i = open; i < limit && i < m_sig.size(); ++i)
            {
                const Tok tok = At(i);
                if (tok == Tok::Lt)
                {
                    ++depth;
                }
                else if (tok == Tok::Gt && --depth == 0)
                {
                    return i;
                }
                else if (tok == Tok::Shr || tok == Tok::Semi || tok == Tok::LBrace)
                {
                    break;
                }
            }

            return limit;
        }

        // The tokens in [begin, end) as one string, for exact comparisons.
        std::string Spell(std::size_t begin, std::size_t end) const
        {
            std::string result;
            for (std::size_t i = begin; i < end; ++i)
            {
                result += Text(i);
                result += ' ';
            }

            return result;
        }

    private:
        const SemanticModel& m_model;
        const std::vector<Token>& m_tokens;
        const std::pmr::vector<std::uint32_t>& m_sig;
    };

} // namespace heimdall::detail
