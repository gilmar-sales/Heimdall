#include <Heimdall/Lexer.hpp>

#include <array>
#include <cstdint>

namespace heimdall
{

    namespace
    {

        // F10: single 256-entry lookup replaces the IsSpace/IsIdentStart/
        // IsIdentContinue comparison chains on every character.
        namespace CharBits
        {
            constexpr std::uint8_t kSpace = 1 << 0;
            constexpr std::uint8_t kIdentStart = 1 << 1;
            constexpr std::uint8_t kIdentCont = 1 << 2;
            constexpr std::uint8_t kDigit = 1 << 3;
            constexpr std::uint8_t kHex = 1 << 4;
            constexpr std::uint8_t kPunct = 1 << 5;
        } // namespace CharBits

        constexpr std::array<std::uint8_t, 256> BuildCharClass()
        {
            std::array<std::uint8_t, 256> table{};
            for (int i = 0; i < 256; ++i)
            {
                const char c = static_cast<char>(i);
                std::uint8_t bits = 0;
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
                {
                    bits |= CharBits::kSpace;
                }

                const bool ident_start = (c >= 'a' && c <= 'z') ||(c >= 'A' && c <= 'Z') || c == '_' || i >= 0x80;
                if (ident_start)
                {
                    bits |= CharBits::kIdentStart | CharBits::kIdentCont;
                }

                if (c >= '0' && c <= '9')
                {
                    bits |= CharBits::kIdentCont | CharBits::kDigit | CharBits::kHex;
                }

                if ((c >= 'a' && c <= 'f') ||(c >= 'A' && c <= 'F'))
                {
                    bits |= CharBits::kHex;
                }

                constexpr std::string_view punct = "{}[]#()<>%:;.?*+-/^&|~!=,\\\"'";
                if (punct.find(c) != std::string_view::npos)
                {
                    bits |= CharBits::kPunct;
                }

                table[i] = bits;
            }

            return table;
        }

        constexpr std::array<std::uint8_t, 256> kCharClass = BuildCharClass();

        constexpr bool IsSpace(char c)
        {
            return (kCharClass[static_cast<unsigned char>(c)] & CharBits::kSpace) != 0;
        }

        constexpr bool IsDigit(char c)
        {
            return (kCharClass[static_cast<unsigned char>(c)] & CharBits::kDigit) != 0;
        }

        constexpr bool IsIdentStart(char c)
        {
            return (kCharClass[static_cast<unsigned char>(c)] & CharBits::kIdentStart) != 0;
        }

        constexpr bool IsIdentContinue(char c)
        {
            return (kCharClass[static_cast<unsigned char>(c)] & CharBits::kIdentCont) != 0;
        }

        constexpr bool IsHexDigit(char c)
        {
            return (kCharClass[static_cast<unsigned char>(c)] & CharBits::kHex) != 0;
        }

        std::size_t ScanQuoted(std::string_view s, std::size_t i, char quote)
        {
            ++i;
            while (i < s.size())
            {
                if (s[i] == '\n' || s[i] == '\r')
                {
                    break;
                }

                if (s[i] == '\\')
                {
                    i += i + 1 < s.size() ? 2 : 1;
                }
                else if (s[i++] == quote)
                {
                    break;
                }
            }

            return i;
        }

        std::size_t ScanRawString(std::string_view s, std::size_t quote)
        {
            const std::size_t delim_start = quote + 1;
            const std::size_t open = s.find('(', delim_start);
            if (open == std::string_view::npos || open - delim_start > 16)
            {
                return ScanQuoted(s, quote, '"');
            }

            const std::string_view delimiter = s.substr(delim_start, open - delim_start);
            std::size_t pos = open + 1;
            while ((pos = s.find(')', pos)) != std::string_view::npos)
            {
                if (pos + delimiter.size() + 1 < s.size() && s[pos + 1 + delimiter.size()] == '"' &&
                    s.substr(pos + 1, delimiter.size()) == delimiter)
                {
                    return pos + delimiter.size() + 2;
                }

                ++pos;
            }

            return s.size();
        }

        bool IsPunctuatorStart(char c)
        {
            return (kCharClass[static_cast<unsigned char>(c)] & CharBits::kPunct) != 0;
        }

        std::size_t PunctuatorLength(std::string_view source, std::size_t offset)
        {
            const std::string_view tail = source.substr(offset);
            auto is =[tail](std::string_view punctuator)
            {
                return tail.starts_with(punctuator);
            };
            switch (source[offset])
            {
            case '%':
                if (is("%:%:"))
                {
                    return 4;
                }

                if (is("%=") || is("%>") || is("%:"))
                {
                    return 2;
                }

                break;
            case '#':
                if (is("##"))
                {
                    return 2;
                } break;
            case ':':
                if (is("::") || is(":>"))
                {
                    return 2;
                } break;
            case '.':
                if (is("...") || is(".*"))
                {
                    return is("...") ? 3 : 2;
                } break;
            case '-':
                if (is("->*"))
                {
                    return 3;
                }

                if (is("->") || is("--") || is("-="))
                {
                    return 2;
                }

                break;
            case '<':
                if (is("<=>"))
                {
                    return 3;
                }

                if (is("<<="))
                {
                    return 3;
                }

                if (is("<<") || is("<=") || is("<:") || is("<%"))
                {
                    return 2;
                }

                break;
            case '>':
                if (is(">>="))
                {
                    return 3;
                }

                if (is(">>") || is(">="))
                {
                    return 2;
                }

                break;
            case '+':
                if (is("++") || is("+="))
                {
                    return 2;
                } break;
            case '*':
                if (is("*="))
                {
                    return 2;
                } break;
            case '/':
                if (is("/="))
                {
                    return 2;
                } break;
            case '&':
                if (is("&&") || is("&="))
                {
                    return 2;
                } break;
            case '|':
                if (is("||") || is("|="))
                {
                    return 2;
                } break;
            case '^':
                if (is("^="))
                {
                    return 2;
                } break;
            case '=':
                if (is("=="))
                {
                    return 2;
                } break;
            case '!':
                if (is("!="))
                {
                    return 2;
                } break;
            default:
                break;
            }

            return 1;
        }

    } // namespace

    std::vector<Token> Lexer::Lex() const
    {
        std::vector<Token> tokens;
        // ~1 token per 4 source bytes on typical C++ (was size/3: over-reserved
        // ~8 B of Token storage per source byte up front).
        tokens.reserve(m_source.size() / 4 + 1);

        std::size_t i = 0;
        while (i < m_source.size())
        {
            const std::size_t start = i;
            const char c = m_source[i];
            TokenKind kind = TokenKind::Unknown;

            if (IsSpace(c))
            {
                kind = TokenKind::Whitespace;
                do
                {
                    ++i;
                } while (i < m_source.size() && IsSpace(m_source[i]));
            }
            else if (c == '/' && i + 1 < m_source.size() && m_source[i + 1] == '/')
            {
                kind = TokenKind::LineComment;
                i += 2;
                while (i < m_source.size() && m_source[i] != '\n' && m_source[i] != '\r')
                {
                    ++i;
                }
            }
            else if (c == '/' && i + 1 < m_source.size() && m_source[i + 1] == '*')
            {
                kind = TokenKind::BlockComment;
                i += 2;
                while (i + 1 < m_source.size() && !(m_source[i] == '*' && m_source[i + 1] == '/'))
                {
                    ++i;
                }

                i = i + 1 < m_source.size() ? i + 2 : m_source.size();
            }
            else
            {
                std::size_t quote = i;
                std::size_t raw_quote = std::string_view::npos;
                if (c == 'R' && i + 1 < m_source.size() && m_source[i + 1] == '"')
                {
                    raw_quote = i + 1;
                }
                else if ((c == 'u' || c == 'U' || c == 'L') && i + 2 < m_source.size() &&
                    m_source[i + 1] == 'R' && m_source[i + 2] == '"')
                {
                    raw_quote = i + 2;
                }
                else if (c == 'u' && i + 3 < m_source.size() && m_source[i + 1] == '8' &&
                    m_source[i + 2] == 'R' && m_source[i + 3] == '"')
                {
                    raw_quote = i + 3;
                }

                if (raw_quote != std::string_view::npos)
                {
                    kind = TokenKind::RawStringLiteral;
                    i = ScanRawString(m_source, raw_quote);
                }
                else if ((c == 'u' || c == 'U' || c == 'L') && i + 1 < m_source.size() &&
                    (m_source[i + 1] == '"' || m_source[i + 1] == '\''))
                {
                    quote = i + 1;
                    kind = m_source[quote] == '"' ? TokenKind::StringLiteral : TokenKind::CharacterLiteral;
                    i = ScanQuoted(m_source, quote, m_source[quote]);
                }
                else if (c == 'u' && i + 2 < m_source.size() && m_source[i + 1] == '8' &&
                    (m_source[i + 2] == '"' || m_source[i + 2] == '\''))
                {
                    quote = i + 2;
                    kind = m_source[quote] == '"' ? TokenKind::StringLiteral : TokenKind::CharacterLiteral;
                    i = ScanQuoted(m_source, quote, m_source[quote]);
                }
                else if (c == '"' || c == '\'')
                {
                    kind = c == '"' ? TokenKind::StringLiteral : TokenKind::CharacterLiteral;
                    i = ScanQuoted(m_source, i, c);
                }
                else if (IsIdentStart(c))
                {
                    kind = TokenKind::Identifier;
                    do
                    {
                        ++i;
                    } while (i < m_source.size() && IsIdentContinue(m_source[i]));
                }
                else if (IsDigit(c) ||(c == '.' && i + 1 < m_source.size() && IsDigit(m_source[i + 1])))
                {
                    kind = TokenKind::Number;
                    ++i;
                    while (i < m_source.size())
                    {
                        const char n = m_source[i];
                        if (IsIdentContinue(n) || n == '\'' || n == '.')
                        {
                            ++i;
                        }
                        else if ((n == '+' || n == '-') && i > start &&
                            (m_source[i - 1] == 'e' || m_source[i - 1] == 'E' || m_source[i - 1] == 'p' ||
                            m_source[i - 1] == 'P'))
                        {
                            ++i;
                        }
                        else if (n == 'x' || n == 'X')
                        {
                            ++i;
                        }
                        else if (IsHexDigit(n) && (m_source[start] == '0' && start + 1 < m_source.size() &&
                            (m_source[start + 1] == 'x' || m_source[start + 1] == 'X')))
                        {
                            ++i;
                        }
                        else
                        {
                            break;
                        }
                    }
                }
                else if (IsPunctuatorStart(c))
                {
                    kind = TokenKind::Punctuation;
                    i += PunctuatorLength(m_source, i);
                }
                else
                {
                    ++i;
                }
            }

            tokens.push_back({kind, static_cast<std::uint32_t>(start),
                    static_cast<std::uint32_t>(i - start)});
        }

        return tokens;
    }

    std::string_view Lexer::Text(const Token &token) const noexcept
    {
        return m_source.substr(token.offset, token.length);
    }

} // namespace heimdall
