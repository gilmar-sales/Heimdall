#include <Heimdall/SemanticRules.hpp>

#include "detail/RuleSupport.hpp"
#include "detail/TokenView.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace heimdall
{

    namespace
    {

        // One `.name = value` of a designated-initializer list: positions of its
        // first and one-past-last significant tokens.
        struct Designator
        {
            std::size_t begin = 0;
            std::size_t end = 0;
            NameId name = kNone;
        };

        class DesignatedOrder
        {
        public:
            explicit DesignatedOrder(const SemanticModel &model) : m_model(model), m_view(model),
              m_symbols(model.Symbols()), m_reporter(model.Tree())
            {
            }

            std::vector<Diagnostic> Run()
            {
                std::vector<Diagnostic> diagnostics;
                for (std::size_t open = 0; open < m_view.Size(); ++open)
                {
                    if (m_view.At(open) == Tok::LBrace && m_view.At(open + 1) == Tok::Dot)
                    {
                        Check(open, diagnostics);
                    }
                }

                detail::SortByOffset(diagnostics);
                return diagnostics;
            }

        private:
            // The class the list initializes: `T{`, `new T{`, `ns::T{` and `T x{` /
            // `T x = {`. Anything else (a function call, an unresolved name) is unknown.
            SymbolId OwnerOf(std::size_t open) const
            {
                if (open == 0)
                {
                    return kNone;
                }

                std::size_t at = open - 1;
                if (m_view.At(at) == Tok::Eq && at > 0)
                {
                    --at;
                }

                if (!m_view.IsWord(at))
                {
                    return kNone;
                }

                const auto direct = ClassAt(at);
                if (direct != kNone)
                {
                    return direct;
                }

                // `T x{...}`: the word before the variable names the class.
                const auto variable = ResolveVariable(at);
                if (variable == kNone || at == 0)
                {
                    return kNone;
                }

                std::size_t type = at - 1;
                while (type > 0 && (m_view.At(type) == Tok::Star || m_view.At(type) == Tok::Amp ||
                    m_view.At(type) == Tok::KwConst))
                {
                    --type;
                }

                return m_view.IsWord(type) ? ClassAt(type) : kNone;
            }

            SymbolId ResolveVariable(std::size_t position) const
            {
                const auto symbol = m_model.ResolveToken(m_view.TokenAt(position));
                if (symbol != kNone)
                {
                    return m_symbols.kind[symbol] == SymbolKind::Variable ? symbol : kNone;
                }

                // The declaring token of a variable is not a use: find it by name.
                const auto name = m_model.Names().Find(m_view.Text(position));
                if (name == kNone)
                {
                    return kNone;
                }

                for (SymbolId id = 0; id < m_symbols.Size(); ++id)
                {
                    if (m_symbols.name[id] == name && m_symbols.kind[id] == SymbolKind::Variable &&
                        m_symbols.decl_token[id] == m_view.TokenAt(position))
                    {
                        return id;
                    }
                }

                return kNone;
            }

            SymbolId ClassAt(std::size_t position) const
            {
                auto symbol = m_model.ResolveToken(m_view.TokenAt(position));
                if (symbol == kNone)
                {
                    const auto name = m_model.Names().Find(m_view.Text(position));
                    symbol = name == kNone ? kNone : m_model.Lookup(SemanticModel::TranslationUnitScope, name);
                }

                if (symbol == kNone || m_symbols.kind[symbol] != SymbolKind::Class ||
                    m_symbols.member_scope[symbol] == kNone || m_symbols.base_count[symbol] != 0 ||
                    (m_symbols.flags[symbol] & SymbolFlag::Template) != 0)
                {
                    return kNone;
                }

                return symbol;
            }

            // Non-static data members in declaration order. Symbols are numbered as
            // the binder meets them, so ascending ids are source order.
            std::vector<NameId> FieldsOf(SymbolId klass) const
            {
                std::vector<NameId> fields;
                for (SymbolId id = 0; id < m_symbols.Size(); ++id)
                {
                    if (m_symbols.scope[id] == m_symbols.member_scope[klass] &&
                        m_symbols.kind[id] == SymbolKind::Variable && (m_symbols.flags[id] & SymbolFlag::Static) == 0)
                    {
                        fields.push_back(m_symbols.name[id]);
                    }
                }

                return fields;
            }

            // Splits the list into designators; empty when it is not purely
            // designated (`{.a = 1, 2}` is ill-formed) or something is off.
            std::vector<Designator> Parse(std::size_t open, std::size_t close) const
            {
                std::vector<Designator> list;
                std::size_t i = open + 1;
                while (i < close)
                {
                    if (m_view.At(i) != Tok::Dot || !m_view.IsWord(i + 1) ||
                        (m_view.At(i + 2) != Tok::Eq && m_view.At(i + 2) != Tok::LBrace))
                    {
                        return {};
                    }

                    Designator entry;
                    entry.begin = i;
                    entry.name = m_model.Names().Find(m_view.Text(i + 1));
                    int depth = 0;
                    std::size_t j = i + 2;
                    for (; j < close; ++j)
                    {
                        const Tok tok = m_view.At(j);
                        if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace)
                        {
                            ++depth;
                        }
                        else if (tok == Tok::RParen || tok == Tok::RBracket || tok == Tok::RBrace)
                        {
                            --depth;
                        }
                        else if (tok == Tok::Comma && depth == 0)
                        {
                            // A comma inside template arguments (`std::pair<int, int>{}`)
                            // is followed by something that is no designator.
                            if (m_view.At(j + 1) == Tok::Dot || j + 1 >= close)
                            {
                                break;
                            }
                        }
                    }

                    entry.end = j;
                    list.push_back(entry);
                    i = j + 1;
                }

                return list;
            }

            void Check(std::size_t open, std::vector<Diagnostic> & out)
            {
                const auto klass = OwnerOf(open);
                if (klass == kNone)
                {
                    return;
                }

                const std::size_t close = m_view.Match(open, m_view.Size());
                if (close >= m_view.Size())
                {
                    return;
                }

                const auto list = Parse(open, close);
                if (list.size() < 2)
                {
                    return;
                }

                const auto fields = FieldsOf(klass);
                std::unordered_map<NameId, std::size_t> rank;
                for (std::size_t i = 0; i < fields.size(); ++i)
                {
                    rank.emplace(fields[i], i);
                }

                std::vector<std::size_t> order;
                for (const auto & entry: list)
                {
                    const auto found = rank.find(entry.name);
                    if (found == rank.end())
                    {
                        return; // not a field of this class (or a nested/inherited one): unknown
                    }

                    order.push_back(found->second);
                }

                std::vector<std::size_t> sorted = order;
                std::sort(sorted.begin(), sorted.end());
                if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
                {
                    return; // a field named twice
                }

                if (sorted == order)
                {
                    return;
                }

                // Rewrite the span from the first entry to the end of the last one: the
                // entries are permuted, the separators between them stay where they are.
                const std::string_view source = m_model.Tree().Source();
                const std::size_t first = m_view.Offset(list.front().begin);
                const std::size_t last = m_view.End(list.back().end - 1);
                std::vector<std::size_t> permutation(list.size());
                for (std::size_t i = 0; i < permutation.size(); ++i)
                {
                    permutation[i] = i;
                }

                std::stable_sort(permutation.begin(), permutation.end(),
                    [&](std::size_t a, std::size_t b)
                    {
                        return order[a] < order[b];
                    });

                const auto text = [&](const Designator & entry)
                {
                    const std::size_t begin = m_view.Offset(entry.begin);
                    return source.substr(begin, m_view.End(entry.end - 1) - begin);
                };

                std::string replacement;
                for (std::size_t i = 0; i < list.size(); ++i)
                {
                    replacement += text(list[permutation[i]]);
                    if (i + 1 < list.size())
                    {
                        const std::size_t gap = m_view.End(list[i].end - 1);
                        replacement += source.substr(gap, m_view.Offset(list[i + 1].begin) - gap);
                    }
                }

                const std::size_t anchor = m_view.Offset(list.front().begin);
                out.push_back(m_reporter.Make(RuleId::DesignatedInitOrder, "cpp/designated-init-order",
                        "designated initializers must follow the declaration order of the members", anchor,
                        m_view.End(list.front().begin + 1) - anchor, TextEdit{first, last - first, std::move(replacement)},
                        "Reorder designated initializers"));
            }

            const SemanticModel &m_model;
            detail::TokenView m_view;
            const SymbolTable &m_symbols;
            detail::Reporter m_reporter;
        };

    } // namespace

    std::vector<Diagnostic> SemanticRules::AnalyzeDesignatedInitOrder(const SemanticModel &model)
    {
        return DesignatedOrder(model).Run();
    }

} // namespace heimdall
