#pragma once

#include <Heimdall/ParseTree.hpp>
#include <Heimdall/SemanticModel.hpp>

#include <cstdint>

namespace heimdall::detail
{

    // True when the head of the class whose name is at `name_token` says
    // `final` (`class A final : B {`). Reads tokens from the name up to the base
    // clause or the body.
    inline bool ClassHeadIsFinal(const ParseTree& tree, std::uint32_t name_token)
    {
        const auto& tokens = tree.Tokens();
        for (std::size_t i = std::size_t { name_token } + 1; i < tokens.size(); ++i)
        {
            const Token& token = tokens[i];
            if (token.kind == TokenKind::Whitespace || token.kind == TokenKind::LineComment ||
                token.kind == TokenKind::BlockComment)
            {
                continue;
            }

            if (token.tok == Tok::LBrace || token.tok == Tok::Colon || token.tok == Tok::Semi)
            {
                return false;
            }

            if (token.tok == Tok::KwFinal ||
                (token.kind == TokenKind::Identifier && tree.Text(token) == "final"))
            {
                return true;
            }
        }

        return false;
    }

    // Per class symbol: declares a virtual, override, final or pure member
    // function. Indexed by SymbolId; non-classes are false.
    inline std::vector<std::uint8_t> ClassesWithVirtualMembers(const SemanticModel& model)
    {
        const auto&               symbols = model.Symbols();
        const auto&               scopes  = model.Scopes();
        std::vector<std::uint8_t> result(symbols.Size(), 0);
        constexpr std::uint32_t   virtualish =
            SymbolFlag::Virtual | SymbolFlag::Override | SymbolFlag::Final | SymbolFlag::Pure;
        for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
        {
            if (symbols.kind[symbol] != SymbolKind::Function ||
                (symbols.flags[symbol] & virtualish) == 0)
            {
                continue;
            }

            const auto scope = symbols.scope[symbol];
            if (scopes.kind[scope] == ScopeKind::Class && scopes.owner[scope] != kNone)
            {
                result[scopes.owner[scope]] = 1;
            }
        }

        return result;
    }

} // namespace heimdall::detail
