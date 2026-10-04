#include <Heimdall/HeaderSummary.hpp>

#include <Heimdall/ParseTree.hpp>
#include <Heimdall/SemanticModel.hpp>

#include "detail/ClassHead.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace heimdall
{

    namespace
    {

        constexpr std::size_t kMaxHeaderBytes = 4u << 20;

        std::string ReadFile(const std::filesystem::path& path)
        {
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            if (ec || size > kMaxHeaderBytes)
            {
                return {};
            }

            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                return {};
            }

            std::string content(static_cast<std::size_t>(size), '\0');
            in.read(content.data(), static_cast<std::streamsize>(content.size()));
            content.resize(static_cast<std::size_t>(in.gcount()));
            return content;
        }

        std::uintmax_t SizeOf(const std::filesystem::path& path)
        {
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            return ec ? 0 : size;
        }

        std::int64_t MTimeOf(const std::filesystem::path& path)
        {
            std::error_code ec;
            const auto time = std::filesystem::last_write_time(path, ec);
            return ec ? 0 : static_cast<std::int64_t>(time.time_since_epoch().count());
        }

        bool IsTextualExtension(const std::filesystem::path& path)
        {
            std::string extension = path.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](unsigned char c)
                {
                    return static_cast<char>(std::tolower(c));
            });
            return extension == ".inc" || extension == ".def" || extension == ".inl" || extension == ".tpp" ||
                extension == ".ipp" || extension == ".tcc";
        }

    } // namespace

    class HeaderSummaryBuilder
    {
    public:
        HeaderSummaryBuilder(HeaderSummary& summary, const ParseTree& tree,
            const SemanticModel& model) : m_summary(summary),
            m_tree(tree), m_model(model), m_qualified(model.Scopes().Size()),
            m_state(model.Scopes().Size(), 0) {}

        void Run()
        {
            Intern({}); // id 0 is the empty string
            ScanIncludes();
            ScanSymbols();
        }

    private:
        enum State : std::uint8_t
        {
            Unknown,
            Exported, // namespace chain only: usable
            Hidden    // anonymous namespace, class, function or block on the way up
        };

        std::uint32_t Intern(std::string_view text)
        {
            const auto found = m_ids.find(std::string(text));
            if (found != m_ids.end())
            {
                return found->second;
            }

            const auto id = static_cast<std::uint32_t>(m_summary.m_span_begin.size());
            m_summary.m_span_begin.push_back(static_cast<std::uint32_t>(m_summary.m_pool.size()));
            m_summary.m_pool.append(text);
            m_summary.m_span_end.push_back(static_cast<std::uint32_t>(m_summary.m_pool.size()));
            m_ids.emplace(std::string(text), id);
            return id;
        }

        // `a::b` for a scope made only of named namespaces; false when the scope
        // cannot be named from outside the header.
        bool Qualification(ScopeId scope, std::string& out)
        {
            const auto& scopes = m_model.Scopes();
            if (m_state[scope] == Unknown)
            {
                if (scopes.kind[scope] == ScopeKind::TranslationUnit)
                {
                    m_state[scope] = Exported;
                }
                else if (scopes.kind[scope] != ScopeKind::Namespace || scopes.owner[scope] == kNone)
                {
                    m_state[scope] = Hidden;
                }
                else
                {
                    std::string parent;
                    if (!Qualification(scopes.parent[scope], parent))
                    {
                        m_state[scope] = Hidden;
                    }
                    else
                    {
                        const auto name = m_model.Names().Text(m_model.Symbols().name[scopes.owner[scope]]);
                        m_qualified[scope] = parent.empty() ? std::string(name) : parent + "::" + std::string(name);
                        m_state[scope] = Exported;
                    }
                }
            }

            if (m_state[scope] != Exported)
            {
                return false;
            }

            out = m_qualified[scope];
            return true;
        }

        void ScanIncludes()
        {
            const std::string_view source = m_tree.Source();
            for (const auto & directive: m_tree.Directives())
            {
                if (directive.kind != DirectiveKind::Include)
                {
                    continue;
                }

                const std::string_view body = source.substr(directive.offset, directive.length);
                std::size_t pos = body.find('#');
                if (pos == std::string_view::npos)
                {
                    continue;
                }

                ++pos;
                while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t'))
                {
                    ++pos;
                }

                constexpr std::string_view keyword = "include";
                if (body.substr(pos, keyword.size()) != keyword)
                {
                    continue;
                }

                pos += keyword.size();
                if (pos < body.size() && (std::isalnum(static_cast<unsigned char>(body[pos])) || body[pos] == '_'))
                {
                    continue; // include_next and friends
                }

                const std::size_t open = body.find_first_of("<\"", pos);
                if (open == std::string_view::npos)
                {
                    continue;
                }

                const char closing = body[open] == '<' ? '>' : '"';
                const std::size_t close = body.find(closing, open + 1);
                if (close == std::string_view::npos)
                {
                    continue;
                }

                m_summary.m_include_target.push_back(Intern(body.substr(open, close - open + 1)));
                m_summary.m_include_reexport.push_back(
                    body.substr(close + 1).find("IWYU pragma: export") != std::string_view::npos ? 1 : 0);
            }
        }

        static bool Exportable(SymbolKind kind)
        {
            return kind == SymbolKind::Class || kind == SymbolKind::Enum || kind == SymbolKind::TypeAlias ||
                kind == SymbolKind::Function || kind == SymbolKind::Variable || kind == SymbolKind::Enumerator;
        }

        static ExportKind ExportKindOf(SymbolKind kind)
        {
            switch (kind)
            {
            case SymbolKind::Class:
                return ExportKind::Class;
            case SymbolKind::Enum:
                return ExportKind::Enum;
            case SymbolKind::TypeAlias:
                return ExportKind::TypeAlias;
            case SymbolKind::Function:
                return ExportKind::Function;
            case SymbolKind::Enumerator:
                return ExportKind::Enumerator;
            default:
                return ExportKind::Variable;
            }
        }

        void ScanSymbols()
        {
            const auto& symbols = m_model.Symbols();
            const auto& bases = m_model.Bases();
            const auto virtual_members = detail::ClassesWithVirtualMembers(m_model);
            constexpr std::uint32_t kNotExported = SymbolFlag::Qualified | SymbolFlag::Friend |
                SymbolFlag::Constructor | SymbolFlag::Destructor | SymbolFlag::Operator;
            std::string qualification;
            for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
            {
                const auto kind = symbols.kind[symbol];
                const auto name = symbols.name[symbol];
                if (!Exportable(kind) || name == kNone ||(symbols.flags[symbol] & kNotExported) != 0)
                {
                    continue;
                }

                const std::string_view text = m_model.Names().Text(name);
                if (text.empty() || text.front() == '_' ||!Qualification(symbols.scope[symbol], qualification))
                {
                    continue;
                }

                // `class A;` leaves no symbol, but a class symbol without a body
                // scope would not be a definition either.
                const bool is_class = kind == SymbolKind::Class;
                if (is_class && symbols.member_scope[symbol] == kNone)
                {
                    continue;
                }

                const auto name_id = Intern(text);
                const auto ns_id = Intern(qualification);
                m_summary.m_export_name.push_back(name_id);
                m_summary.m_export_ns.push_back(ns_id);
                m_summary.m_export_kind.push_back(ExportKindOf(kind));
                if (!is_class)
                {
                    continue;
                }

                std::uint8_t flags = 0;
                if (virtual_members[symbol] != 0)
                {
                    flags |= HeaderSummary::kClassVirtual;
                }

                if (detail::ClassHeadIsFinal(m_tree, symbols.decl_token[symbol]))
                {
                    flags |= HeaderSummary::kClassFinal;
                }

                if ((symbols.flags[symbol] & SymbolFlag::Template) != 0)
                {
                    flags |= HeaderSummary::kClassTemplate;
                }

                m_summary.m_class_name.push_back(name_id);
                m_summary.m_class_ns.push_back(ns_id);
                m_summary.m_class_flags.push_back(flags);
                m_summary.m_class_first_base.push_back(static_cast<std::uint32_t>(m_summary.m_base_name.size()));
                std::uint32_t count = 0;
                for (std::uint32_t i = 0; i < symbols.base_count[symbol]; ++i)
                {
                    const auto base_name = bases.name[symbols.first_base[symbol] + i];
                    if (base_name == kNone)
                    {
                        // A base the binder could not name: keep the slot so the
                        // class never looks like it has fewer bases than it has.
                        m_summary.m_base_name.push_back(0);
                    }
                    else
                    {
                        m_summary.m_base_name.push_back(Intern(m_model.Names().Text(base_name)));
                    }

                    ++count;
                }

                m_summary.m_class_base_count.push_back(count);
            }
        }

        HeaderSummary& m_summary;
        const ParseTree& m_tree;
        const SemanticModel& m_model;
        std::unordered_map<std::string, std::uint32_t> m_ids;
        std::vector<std::string> m_qualified;
        std::vector<std::uint8_t> m_state;
    };

    std::shared_ptr<const HeaderSummary> HeaderSummary::FromSource(std::string_view source,
        std::filesystem::path path)
    {
        auto summary = std::make_shared<HeaderSummary>();
        summary->m_path = std::move(path);
        summary->m_readable = true;
        summary->m_private = source.find("IWYU pragma: private") != std::string_view::npos;
        summary->m_textual = IsTextualExtension(summary->m_path);
        const ParseTree tree = ParseTree::Parse(source, ParserOptions{});
        const SemanticModel model = Binder::Bind(tree);
        HeaderSummaryBuilder(*summary, tree, model).Run();
        return summary;
    }

    std::shared_ptr<const HeaderSummary> HeaderSummary::Load(const std::filesystem::path& path)
    {
        struct Cached
        {
            std::uintmax_t size = 0;
            std::int64_t mtime = 0;
            std::shared_ptr<const HeaderSummary> summary;
        };

        static std::mutex mutex;
        static std::unordered_map<std::string, Cached> cache;
        const std::string key = path.string();
        const auto size = SizeOf(path);
        const auto mtime = MTimeOf(path);
        {
            const std::lock_guard<std::mutex> lock(mutex);
            if (const auto found = cache.find(key);
                found != cache.end() && found->second.size == size && found->second.mtime == mtime)
            {
                return found->second.summary;
            }
        }

        std::shared_ptr<const HeaderSummary> summary;
        const std::string content = ReadFile(path);
        std::error_code exists_ec;
        (void) std::filesystem::file_size(path, exists_ec);
        if (exists_ec ||(content.empty() && size != 0))
        {
            auto unreadable = std::make_shared<HeaderSummary>();
            unreadable->m_path = path;
            summary = std::move(unreadable);
        }
        else
        {
            summary = FromSource(content, path);
        }

        const std::lock_guard<std::mutex> lock(mutex);
        if (cache.size() > 8192)
        {
            cache.clear();
        }

        cache[key] = {size, mtime, summary};
        return summary;
    }

} // namespace heimdall
