#include <Heimdall/LineTable.hpp>
#include <Heimdall/SemanticRules.hpp>

#include <algorithm>
#include <string>

namespace heimdall
{

    namespace
    {

        constexpr std::uint32_t kExcluded = SymbolFlag::Static | SymbolFlag::Constructor | SymbolFlag::Destructor |
            SymbolFlag::Qualified | SymbolFlag::Friend | SymbolFlag::Override | SymbolFlag::Final |
            SymbolFlag::Template;
        constexpr std::uint32_t kVirtualish = SymbolFlag::Virtual | SymbolFlag::Override | SymbolFlag::Final;

        bool IsTrivia(const Token &token)
        {
            return token.kind == TokenKind::Whitespace || token.kind == TokenKind::LineComment ||
                token.kind == TokenKind::BlockComment;
        }

        // Visited marks shared by every query of one rule run: a query bumps the
        // epoch instead of clearing the array, so a query costs only the classes
        // it actually walks.
        struct Visited
        {
            explicit Visited(std::size_t symbols) : stamp(symbols, 0) {}
            bool Mark(SymbolId symbol)
            {
                return stamp[symbol] != epoch && (stamp[symbol] = epoch, true);
            }

            std::vector<std::uint32_t> stamp;
            std::uint32_t epoch = 0;
        };

        // A virtual function with the same name and signature in any ancestor
        // whose base chain is resolved. Returns the owning class symbol.
        SymbolId FindOverridden(const SemanticModel &model, Visited &visited, SymbolId derived, NameId name,
            std::uint64_t signature)
        {
            ++visited.epoch;
            const auto &symbols = model.Symbols();
            const auto &bases = model.Bases();
            std::vector<SymbolId> pending;
            const auto push_bases =[&](SymbolId klass)
            {
                for (std::uint32_t i = 0; i < symbols.base_count[klass]; ++i)
                {
                    if (const auto target = bases.target[symbols.first_base[klass] + i]; target != kNone)
                    {
                        pending.push_back(target);
                    }
                }
            };

            push_bases(derived);
            visited.Mark(derived);
            while (!pending.empty())
            {
                const auto klass = pending.back();
                pending.pop_back();
                if (!visited.Mark(klass))
                {
                    continue;
                }

                for (auto member = model.LookupLocal(symbols.member_scope[klass], name); member != kNone;
                    member = symbols.next_same_name[member])
                {
                    if (symbols.kind[member] == SymbolKind::Function && symbols.signature[member] == signature &&
                        (symbols.flags[member] & kVirtualish) != 0 &&
                        (symbols.flags[member] & (SymbolFlag::Static | SymbolFlag::Constructor)) == 0)
                    {
                        return klass;
                    }
                }

                push_bases(klass);
            }

            return kNone;
        }

        // End of the declarator: just after the parameter list and its
        // cv/ref/noexcept/trailing-return parts, where `override` is written.
        bool OverrideInsertionOffset(const ParseTree &tree, std::uint32_t name_token, std::size_t & offset)
        {
            const auto &tokens = tree.Tokens();
            const auto next =[&](std::size_t from)
            {
                while (from < tokens.size() && IsTrivia(tokens[from]))
                {
                    ++from;
                }

                return from;
            };

            std::size_t i = next(name_token + 1);
            if (i >= tokens.size() || tokens[i].tok != Tok::LParen)
            {
                return false;
            }

            std::size_t depth = 0;
            std::size_t last = i;
            for (; i < tokens.size(); i = next(i + 1))
            {
                if (tokens[i].tok == Tok::LParen)
                {
                    ++depth;
                }
                else if (tokens[i].tok == Tok::RParen && --depth == 0)
                {
                    last = i;
                    break;
                }
            }

            if (i >= tokens.size())
            {
                return false;
            }

            for (i = next(i + 1); i < tokens.size(); i = next(i + 1))
            {
                const Tok tok = tokens[i].tok;
                if (tok == Tok::Semi || tok == Tok::LBrace || tok == Tok::Eq || tok == Tok::KwFinal ||
                    tok == Tok::Colon || tok == Tok::KwTry || tok == Tok::KwRequires)
                {
                    break;
                }

                if (tok == Tok::KwNoexcept)
                {
                    const auto open = next(i + 1);
                    if (open < tokens.size() && tokens[open].tok == Tok::LParen)
                    {
                        std::size_t nested = 0;
                        for (i = open; i < tokens.size(); i = next(i + 1))
                        {
                            if (tokens[i].tok == Tok::LParen)
                            {
                                ++nested;
                            }
                            else if (tokens[i].tok == Tok::RParen && --nested == 0)
                            {
                                break;
                            }
                        }

                        if (i >= tokens.size())
                        {
                            return false;
                        }
                    }
                }

                last = i;
            }

            offset = tokens[last].offset + tokens[last].length;
            return true;
        }

    } // namespace

    std::vector<Diagnostic> SemanticRules::AnalyzeOverride(const SemanticModel &model)
    {
        std::vector<Diagnostic> diagnostics;
        const auto &symbols = model.Symbols();
        const auto &scopes = model.Scopes();
        const auto &tree = model.Tree();
        LineTable lines;
        bool lines_built = false;
        Visited visited(symbols.Size());
        for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
        {
            if (symbols.kind[symbol] != SymbolKind::Function ||(symbols.flags[symbol] & kExcluded) != 0 ||
                symbols.signature[symbol] == 0 || symbols.name[symbol] == kNone)
            {
                continue;
            }

            const auto scope = symbols.scope[symbol];
            if (scopes.kind[scope] != ScopeKind::Class || scopes.owner[scope] == kNone)
            {
                continue;
            }

            const auto klass = scopes.owner[scope];
            const auto base = FindOverridden(model, visited, klass, symbols.name[symbol],
                symbols.signature[symbol]);
            if (base == kNone)
            {
                continue;
            }

            const auto token = symbols.decl_token[symbol];
            std::size_t insert_at = 0;
            if (!OverrideInsertionOffset(tree, token, insert_at))
            {
                continue;
            }

            if (!lines_built)
            {
                lines.Build(tree.Source());
                lines_built = true;
            }

            const auto &name_token = tree.Tokens()[token];
            const auto position = lines.Lookup(name_token.offset);
            const std::string name(model.Names().Text(symbols.name[symbol]));
            Diagnostic diagnostic{RuleId::ModernizeOverride, Severity::Warning, "cpp/modernize-override",
                "'" + name + "' overrides a virtual function of '" +
                    std::string(model.Names().Text(symbols.name[base])) + "'; add 'override'",
                name_token.offset, name_token.length, position.line, position.column, true,
                TextEdit{insert_at, 0, " override"}};
            // The match is textual (name and parameter spelling): offered as a
            // quick fix, not applied in batch.
            diagnostic.fix_is_safe = false;
            diagnostic.fix_title = "Add 'override' to " + name;
            diagnostics.push_back(std::move(diagnostic));
        }

        std::sort(diagnostics.begin(), diagnostics.end(),
            [](const Diagnostic &a, const Diagnostic &b)
            {
                return a.offset < b.offset;
        });
        return diagnostics;
    }

} // namespace heimdall
