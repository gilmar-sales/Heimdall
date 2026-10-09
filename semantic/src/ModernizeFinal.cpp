#include <Heimdall/ProjectIndex.hpp>
#include <Heimdall/SemanticRules.hpp>

#include "detail/ClassHead.hpp"
#include "detail/RuleSupport.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace heimdall
{

    namespace
    {

        // Deepest base chain followed; deeper is Unknown, not a stack overflow.
        constexpr std::size_t kMaxDepth = 256;

        using detail::IsTrivia;
        using detail::Reporter;
        using detail::SortByOffset;

        // Only a source file keeps its definitions to itself: nothing includes it.
        bool IsSourceFile(const std::filesystem::path& file)
        {
            const std::string extension = detail::Lowercase(file.extension().string());
            return extension == ".cpp" || extension == ".cc" || extension == ".cxx" ||
                   extension == ".c++" || extension == ".cp";
        }

        // ---------------------------------------------------------------------
        // cpp/modernize-final

        class FinalAnalysis
        {
          public:
            FinalAnalysis(const SemanticModel& model, const ProjectContext& context) :
                m_model(model), m_tree(model.Tree()), m_symbols(model.Symbols()),
                m_scopes(model.Scopes()), m_source_file(IsSourceFile(context.file)),
                m_virtual(detail::ClassesWithVirtualMembers(model)),
                m_children(model.Symbols().Size()), m_reporter(model.Tree())
            {
                if (context.profile != nullptr)
                {
                    m_index = std::make_unique<ProjectIndex>(ProjectIndex::Build(*context.profile));
                }

                const auto& bases = model.Bases();
                for (std::size_t i = 0; i < bases.derived.size(); ++i)
                {
                    if (bases.target[i] != kNone)
                    {
                        m_children[bases.target[i]].push_back(bases.derived[i]);
                    }
                    else if (bases.name[i] != kNone)
                    {
                        m_unresolved_bases.insert(bases.name[i]);
                    }
                }

                m_pure.assign(m_symbols.Size(), 0);
                m_first_member.assign(m_symbols.Size(), kNone);
                m_next_member.assign(m_symbols.Size(), kNone);
                for (SymbolId symbol = m_symbols.Size(); symbol-- > 0;)
                {
                    const auto scope = m_symbols.scope[symbol];
                    if (m_scopes.kind[scope] != ScopeKind::Class || m_scopes.owner[scope] == kNone)
                    {
                        continue;
                    }

                    const auto owner      = m_scopes.owner[scope];
                    m_next_member[symbol] = m_first_member[owner];
                    m_first_member[owner] = symbol;
                    if (m_symbols.kind[symbol] == SymbolKind::Function &&
                        (m_symbols.flags[symbol] & SymbolFlag::Pure) != 0)
                    {
                        m_pure[owner] = 1;
                    }
                }
            }

            std::vector<Diagnostic> Run()
            {
                std::vector<Diagnostic> diagnostics;
                for (SymbolId klass = 0; klass < m_symbols.Size(); ++klass)
                {
                    if (!IsPlainClass(klass) || !Closed(klass))
                    {
                        continue;
                    }

                    if (IsLeaf(klass))
                    {
                        ReportClass(klass, diagnostics);
                    }
                    else
                    {
                        ReportMethods(klass, diagnostics);
                    }
                }

                SortByOffset(diagnostics);
                return diagnostics;
            }

          private:
            enum class Tri : std::uint8_t
            {
                No,
                Yes,
                Unknown
            }

            ;

            std::string_view NameOf(SymbolId symbol) const
            {
                return m_model.Names().Text(m_symbols.name[symbol]);
            }

            // A class or struct with a body, not a union, not a template, not
            // already `final`.
            bool IsPlainClass(SymbolId klass) const
            {
                if (m_symbols.kind[klass] != SymbolKind::Class ||
                    m_symbols.member_scope[klass] == kNone || m_symbols.name[klass] == kNone ||
                    (m_symbols.flags[klass] & SymbolFlag::Template) != 0)
                {
                    return false;
                }

                const auto& tokens = m_tree.Tokens();
                for (std::size_t i = m_symbols.decl_token[klass], steps = 0; i > 0 && steps < 16;)
                {
                    --i;
                    if (IsTrivia(tokens[i]))
                    {
                        continue;
                    }

                    ++steps;
                    if (tokens[i].tok == Tok::KwUnion)
                    {
                        return false;
                    }

                    if (tokens[i].tok == Tok::KwClass || tokens[i].tok == Tok::KwStruct)
                    {
                        break;
                    }
                }

                return !detail::ClassHeadIsFinal(m_tree, m_symbols.decl_token[klass]);
            }

            // Every class that could derive from `klass` is visible: it sits in an
            // anonymous namespace, or the file is a source file that no one includes.
            bool Closed(SymbolId klass) const
            {
                if (m_source_file)
                {
                    return true;
                }

                for (ScopeId scope = m_symbols.scope[klass];
                     scope != kNone && scope != SemanticModel::TranslationUnitScope;
                     scope = m_scopes.parent[scope])
                {
                    if (m_scopes.kind[scope] == ScopeKind::Namespace &&
                        m_scopes.owner[scope] == kNone)
                    {
                        return true;
                    }
                }

                return false;
            }

            // Nothing derives from `klass`, as far as the model, the unresolved
            // base names of the file and the included headers can tell.
            bool IsLeaf(SymbolId klass) const
            {
                if (!m_children[klass].empty() ||
                    m_unresolved_bases.contains(m_symbols.name[klass]))
                {
                    return false;
                }

                return m_index == nullptr || !m_index->HasDerived(NameOf(klass));
            }

            Tri Polymorphic(SymbolId klass, std::vector<SymbolId>& visiting) const
            {
                if (m_virtual[klass] != 0)
                {
                    return Tri::Yes;
                }

                if (visiting.size() >= kMaxDepth ||
                    std::find(visiting.begin(), visiting.end(), klass) != visiting.end())
                {
                    return Tri::Unknown;
                }

                visiting.push_back(klass);
                const auto& bases  = m_model.Bases();
                Tri         result = Tri::No;
                for (std::uint32_t i = 0; i < m_symbols.base_count[klass] && result != Tri::Yes;
                     ++i)
                {
                    const auto slot = m_symbols.first_base[klass] + i;
                    Tri        base = Tri::Unknown;
                    if (bases.target[slot] != kNone)
                    {
                        base = Polymorphic(bases.target[slot], visiting);
                    }
                    else if (bases.name[slot] != kNone && m_index != nullptr)
                    {
                        switch (m_index->IsPolymorphic(m_model.Names().Text(bases.name[slot])))
                        {
                            case ProjectIndex::Tri::Yes:
                                base = Tri::Yes;
                                break;
                            case ProjectIndex::Tri::No:
                                base = Tri::No;
                                break;
                            case ProjectIndex::Tri::Unknown:
                                break;
                        }
                    }

                    if (base == Tri::Yes)
                    {
                        result = Tri::Yes;
                    }
                    else if (base == Tri::Unknown)
                    {
                        result = Tri::Unknown;
                    }
                }

                visiting.pop_back();
                return result;
            }

            void ReportClass(SymbolId klass, std::vector<Diagnostic>& out)
            {
                // An abstract class exists to be derived from; only a class with
                // virtual functions (its own or inherited) gains from `final`.
                if (m_pure[klass] != 0)
                {
                    return;
                }

                std::vector<SymbolId> visiting;
                if (Polymorphic(klass, visiting) != Tri::Yes)
                {
                    return;
                }

                const auto&       token = m_tree.Tokens()[m_symbols.decl_token[klass]];
                const std::string name(NameOf(klass));
                out.push_back(m_reporter.Make(
                    RuleId::ModernizeFinal, "cpp/modernize-final",
                    "class '" + name + "' has no derived classes; mark it 'final'", token.offset,
                    token.length, TextEdit { token.offset + token.length, 0, " final" },
                    "Mark class " + name + " final"));
            }

            // Some class below `klass` may have derived classes the file does not
            // show: a base nobody resolved that carries the name, or a header class
            // that lists it. Marked on the class and on every ancestor.
            void MarkOpenBelow()
            {
                m_open_below.assign(m_symbols.Size(), 0);
                const auto&           bases = m_model.Bases();
                std::vector<SymbolId> pending;
                for (SymbolId klass = 0; klass < m_symbols.Size(); ++klass)
                {
                    if (m_symbols.kind[klass] != SymbolKind::Class ||
                        m_symbols.name[klass] == kNone)
                    {
                        continue;
                    }

                    if (m_unresolved_bases.contains(m_symbols.name[klass]) ||
                        (m_index != nullptr && m_index->HasDerived(NameOf(klass))))
                    {
                        m_open_below[klass] = 1;
                        pending.push_back(klass);
                    }
                }

                while (!pending.empty())
                {
                    const auto klass = pending.back();
                    pending.pop_back();
                    for (std::uint32_t i = 0; i < m_symbols.base_count[klass]; ++i)
                    {
                        const auto target = bases.target[m_symbols.first_base[klass] + i];
                        if (target != kNone && m_open_below[target] == 0)
                        {
                            m_open_below[target] = 1;
                            pending.push_back(target);
                        }
                    }
                }
            }

            // Some class below `klass` has a member called `name`.
            bool RedeclaredBelow(SymbolId klass, NameId name)
            {
                if (m_seen.empty())
                {
                    m_seen.assign(m_symbols.Size(), 0);
                }

                ++m_epoch;
                std::vector<SymbolId> pending { klass };
                m_seen[klass] = m_epoch;
                while (!pending.empty())
                {
                    const auto current = pending.back();
                    pending.pop_back();
                    for (const auto child : m_children[current])
                    {
                        if (m_seen[child] == m_epoch)
                        {
                            continue;
                        }

                        m_seen[child] = m_epoch;
                        // The name alone decides: a differently spelled signature in a
                        // derived class could still be an override.
                        if (m_model.LookupLocal(m_symbols.member_scope[child], name) != kNone)
                        {
                            return true;
                        }

                        pending.push_back(child);
                    }
                }

                return false;
            }

            void ReportMethods(SymbolId klass, std::vector<Diagnostic>& out)
            {
                if (m_open_below.empty())
                {
                    MarkOpenBelow();
                }

                if (m_open_below[klass] != 0)
                {
                    return;
                }

                for (auto method = FirstMember(klass); method != kNone; method = NextMember(method))
                {
                    const auto flags = m_symbols.flags[method];
                    if (m_symbols.kind[method] != SymbolKind::Function ||
                        (flags & SymbolFlag::Override) == 0 ||
                        (flags & (SymbolFlag::Final | SymbolFlag::Static | SymbolFlag::Constructor |
                                  SymbolFlag::Destructor | SymbolFlag::Template)) != 0)
                    {
                        continue;
                    }

                    if (RedeclaredBelow(klass, m_symbols.name[method]))
                    {
                        continue;
                    }

                    std::size_t insert_at = 0;
                    if (!AfterOverride(m_symbols.decl_token[method], insert_at))
                    {
                        continue;
                    }

                    const auto&       token = m_tree.Tokens()[m_symbols.decl_token[method]];
                    const std::string name(NameOf(method));
                    out.push_back(m_reporter.Make(
                        RuleId::ModernizeFinal, "cpp/modernize-final",
                        "'" + name + "' is never overridden in a derived class; mark it 'final'",
                        token.offset, token.length, TextEdit { insert_at, 0, " final" },
                        "Mark " + name + " final"));
                }
            }

            SymbolId FirstMember(SymbolId klass) const { return m_first_member[klass]; }

            SymbolId NextMember(SymbolId member) const { return m_next_member[member]; }

            // Offset just after the `override` of the declaration whose name token
            // is `name_token`.
            bool AfterOverride(std::uint32_t name_token, std::size_t& offset) const
            {
                const auto& tokens = m_tree.Tokens();
                int         depth  = 0;
                for (std::size_t i = std::size_t { name_token } + 1; i < tokens.size(); ++i)
                {
                    if (IsTrivia(tokens[i]))
                    {
                        continue;
                    }

                    const Tok tok = tokens[i].tok;
                    if (tok == Tok::LParen)
                    {
                        ++depth;
                    }
                    else if (tok == Tok::RParen)
                    {
                        --depth;
                    }
                    else if (depth == 0 && tok == Tok::KwOverride)
                    {
                        offset = tokens[i].offset + tokens[i].length;
                        return true;
                    }
                    else if (depth == 0 &&
                             (tok == Tok::Semi || tok == Tok::LBrace || tok == Tok::Eq))
                    {
                        return false;
                    }
                }

                return false;
            }

            const SemanticModel&               m_model;
            const ParseTree&                   m_tree;
            const SymbolTable&                 m_symbols;
            const ScopeTable&                  m_scopes;
            bool                               m_source_file;
            std::vector<std::uint8_t>          m_virtual;
            std::vector<std::uint8_t>          m_pure;
            std::vector<std::vector<SymbolId>> m_children;
            std::unordered_set<NameId>         m_unresolved_bases;
            std::unique_ptr<ProjectIndex>      m_index;
            // Members of each class in declaration order: first member per class,
            // chained through m_next_member (the symbol table has no per-scope list).
            std::vector<std::uint8_t>  m_open_below;
            std::vector<std::uint32_t> m_seen;
            std::uint32_t              m_epoch = 0;
            std::vector<SymbolId>      m_first_member;
            std::vector<SymbolId>      m_next_member;
            Reporter                   m_reporter;
        };

    } // namespace

    std::vector<Diagnostic> SemanticRules::AnalyzeFinal(const SemanticModel&  model,
                                                        const ProjectContext& context)
    {
        return FinalAnalysis(model, context).Run();
    }

} // namespace heimdall
