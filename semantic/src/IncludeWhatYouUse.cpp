#include <Heimdall/ProjectIndex.hpp>
#include <Heimdall/SemanticRules.hpp>

#include "detail/RuleSupport.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace heimdall
{

    namespace
    {

        using detail::Key;
        using detail::Lowercase;
        using detail::Reporter;
        using detail::SortByOffset;

        struct StdRow
        {
            std::string_view names;
            std::string_view headers;
        };

        // Name after `std::` (or its first namespace) -> headers that declare it,
        // preferred first. Any one of them included directly satisfies the use.
        // Deliberately short: names with several owners (size_t, move, pair, begin)
        // and names every translation unit gets from half the library are left out.
        constexpr StdRow kStdRows[] = {
            {"vector", "<vector>"},
            {"string basic_string char_traits to_string stoi stol stoll stoul stoull stof stod stold", "<string>"},
            {"getline", "<string> <istream> <iostream> <sstream> <fstream>"},
            {"string_view basic_string_view", "<string_view>"},
            {"map multimap", "<map>"},
            {"set multiset", "<set>"},
            {"unordered_map unordered_multimap", "<unordered_map>"},
            {"unordered_set unordered_multiset", "<unordered_set>"},
            {"deque", "<deque>"},
            {"list", "<list>"},
            {"forward_list", "<forward_list>"},
            {"array", "<array>"},
            {"stack", "<stack>"},
            {"queue priority_queue", "<queue>"},
            {"span dynamic_extent", "<span>"},
            {"bitset", "<bitset>"},
            {"optional nullopt nullopt_t make_optional", "<optional>"},
            {"variant visit holds_alternative get_if monostate variant_alternative variant_size", "<variant>"},
            {"any any_cast make_any", "<any>"},
            {"tuple make_tuple tie forward_as_tuple tuple_cat", "<tuple>"},
            {"unique_ptr shared_ptr weak_ptr make_unique make_shared allocate_shared enable_shared_from_this "
             "default_delete addressof", "<memory>"},
            {"function bind bind_front ref cref reference_wrapper mem_fn invoke", "<functional>"},
            {"thread this_thread jthread", "<thread>"},
            {"stop_token stop_source stop_callback", "<stop_token> <thread>"},
            {"mutex recursive_mutex timed_mutex recursive_timed_mutex lock_guard unique_lock scoped_lock call_once "
             "once_flag defer_lock adopt_lock try_to_lock", "<mutex>"},
            {"shared_mutex shared_timed_mutex shared_lock", "<shared_mutex>"},
            {"condition_variable condition_variable_any cv_status", "<condition_variable>"},
            {"atomic atomic_flag atomic_ref memory_order atomic_thread_fence", "<atomic>"},
            {"future promise packaged_task async launch shared_future", "<future>"},
            {"latch", "<latch>"},
            {"barrier", "<barrier>"},
            {"counting_semaphore binary_semaphore", "<semaphore>"},
            {"chrono", "<chrono> <thread> <mutex> <condition_variable> <future> <shared_mutex> <filesystem> <ctime>"},
            {"cout cerr cin clog wcout wcerr wcin", "<iostream>"},
            {"endl flush ends", "<ostream> <iostream> <sstream> <fstream>"},
            {"ostream istream iostream ios streambuf",
             "<ostream> <istream> <iostream> <ios> <iosfwd> <sstream> <fstream> <streambuf> <iomanip>"},
            {"ifstream ofstream fstream basic_fstream filebuf", "<fstream>"},
            {"stringstream ostringstream istringstream basic_stringstream stringbuf", "<sstream>"},
            {"setw setfill setprecision setbase put_time quoted get_time", "<iomanip>"},
            {"runtime_error logic_error invalid_argument out_of_range domain_error length_error overflow_error "
             "underflow_error range_error", "<stdexcept>"},
            {"exception bad_exception terminate current_exception rethrow_exception make_exception_ptr exception_ptr "
             "uncaught_exceptions set_terminate nested_exception throw_with_nested rethrow_if_nested", "<exception>"},
            {"bad_alloc bad_array_new_length nothrow nothrow_t new_handler set_new_handler align_val_t launder", "<new>"},
            {"bad_cast bad_typeid type_info", "<typeinfo>"},
            {"bad_function_call", "<functional>"},
            {"bad_variant_access", "<variant>"},
            {"bad_optional_access", "<optional>"},
            {"system_error error_code error_condition errc generic_category system_category", "<system_error>"},
            {"sort stable_sort partial_sort nth_element find find_if find_if_not find_end find_first_of adjacent_find "
             "count count_if copy copy_if copy_n copy_backward fill fill_n transform for_each for_each_n remove_if "
             "remove_copy remove_copy_if unique reverse rotate shuffle sample min max minmax min_element max_element "
             "minmax_element clamp any_of all_of none_of lower_bound upper_bound binary_search equal_range equal "
             "mismatch lexicographical_compare is_sorted merge inplace_merge includes set_union set_intersection "
             "set_difference set_symmetric_difference partition stable_partition is_permutation next_permutation "
             "prev_permutation replace replace_if generate generate_n search search_n swap_ranges iter_swap "
             "push_heap pop_heap make_heap sort_heap", "<algorithm>"},
            {"remove", "<algorithm> <cstdio>"},
            {"accumulate iota reduce inner_product partial_sum adjacent_difference transform_reduce exclusive_scan "
             "inclusive_scan gcd lcm midpoint lerp", "<numeric>"},
            {"byte nullptr_t max_align_t", "<cstddef> <stddef.h>"},
            {"int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t intptr_t uintptr_t intmax_t uintmax_t "
             "int_least8_t int_least16_t int_least32_t int_least64_t uint_least8_t uint_least16_t uint_least32_t "
             "uint_least64_t int_fast8_t int_fast16_t int_fast32_t int_fast64_t uint_fast8_t uint_fast16_t "
             "uint_fast32_t uint_fast64_t", "<cstdint> <stdint.h> <cinttypes> <inttypes.h>"},
            {"memcpy memset memmove memcmp memchr strlen strcmp strncmp strcpy strncpy strcat strncat strchr strrchr "
             "strstr strtok strerror", "<cstring> <string.h>"},
            {"printf fprintf snprintf sprintf sscanf scanf fscanf fopen fclose fread fwrite fputs fgets fputc fgetc "
             "fflush puts putchar getchar fseek ftell rewind perror rename tmpfile FILE", "<cstdio> <stdio.h>"},
            {"malloc calloc realloc free exit abort atexit quick_exit getenv atoi atol atoll atof strtol strtoul "
             "strtoll strtoull strtod strtof rand srand qsort bsearch", "<cstdlib> <stdlib.h>"},
            {"abs", "<cstdlib> <cmath> <stdlib.h> <math.h>"},
            {"sqrt pow sin cos tan asin acos atan atan2 sinh cosh tanh exp log log10 log2 floor ceil round trunc fabs "
             "fmod cbrt hypot isnan isinf isfinite signbit copysign lround llround nearbyint rint fmin fmax fma ldexp "
             "frexp modf expm1 log1p erf tgamma lgamma", "<cmath> <math.h>"},
            {"time clock difftime mktime strftime localtime gmtime asctime time_t tm timespec clock_t",
             "<ctime> <time.h>"},
            {"isalpha isdigit isspace isalnum isupper islower ispunct isxdigit isprint iscntrl isgraph tolower toupper",
             "<cctype> <ctype.h> <locale>"},
            {"numeric_limits", "<limits>"},
            {"enable_if enable_if_t is_same is_same_v is_integral is_integral_v is_floating_point is_floating_point_v "
             "is_pointer is_pointer_v is_reference is_class is_enum is_base_of is_base_of_v is_convertible "
             "is_convertible_v is_constructible is_constructible_v is_default_constructible "
             "is_copy_constructible is_move_constructible is_trivially_copyable is_trivially_copyable_v is_trivial "
             "is_standard_layout is_empty is_abstract is_final is_polymorphic is_signed is_unsigned is_arithmetic "
             "is_scalar is_object is_void is_const is_volatile is_array is_member_pointer remove_reference "
             "remove_reference_t remove_cv remove_cv_t remove_const remove_const_t remove_pointer remove_extent "
             "decay decay_t conditional conditional_t common_type common_type_t underlying_type underlying_type_t "
             "invoke_result invoke_result_t is_invocable is_invocable_v void_t true_type false_type "
             "integral_constant bool_constant add_const add_pointer add_lvalue_reference add_rvalue_reference "
             "make_signed make_unsigned remove_cvref remove_cvref_t is_nothrow_move_constructible "
             "is_nothrow_move_constructible_v is_lvalue_reference is_rvalue_reference", "<type_traits>"},
            {"same_as integral floating_point convertible_to derived_from invocable predicate regular copyable movable "
             "default_initializable constructible_from assignable_from equality_comparable totally_ordered "
             "signed_integral unsigned_integral", "<concepts>"},
            {"random_device mt19937 mt19937_64 default_random_engine uniform_int_distribution uniform_real_distribution "
             "normal_distribution bernoulli_distribution seed_seq minstd_rand discrete_distribution", "<random>"},
            {"regex smatch cmatch regex_match regex_search regex_replace sregex_iterator", "<regex>"},
            {"filesystem", "<filesystem>"},
            {"format format_string vformat", "<format>"},
            {"source_location", "<source_location>"},
            {"bit_cast popcount countl_zero countr_zero bit_ceil bit_floor bit_width rotl rotr has_single_bit endian "
             "byteswap", "<bit>"},
            {"numbers", "<numbers>"},
            {"expected unexpected unexpect", "<expected>"},
            {"complex", "<complex>"},
            {"coroutine_handle suspend_always suspend_never noop_coroutine", "<coroutine>"},
            {"back_inserter front_inserter inserter istream_iterator ostream_iterator move_iterator "
             "make_move_iterator make_reverse_iterator reverse_iterator istreambuf_iterator ostreambuf_iterator",
             "<iterator>"},
        };

        const std::unordered_map<std::string_view, std::vector<std::string_view>> & StdTable()
        {
            static const std::unordered_map<std::string_view, std::vector<std::string_view>> table = []
            {
                std::unordered_map<std::string_view, std::vector<std::string_view>> result;
                const auto words =[](std::string_view text)
                {
                    std::vector<std::string_view> out;
                    while (!text.empty())
                    {
                        const auto space = text.find(' ');
                        out.push_back(text.substr(0, space));
                        text = space == std::string_view::npos ? std::string_view{} : text.substr(space + 1);
                    }

                    return out;
                };
                for (const auto & row: kStdRows)
                {
                    const auto headers = words(row.headers);
                    for (const auto name: words(row.names))
                    {
                        result[name].insert(result[name].end(), headers.begin(), headers.end());
                    }
                }

                return result;
            }();
            return table;
        }

        // `<vector>` -> `vector`, `"a/b.h"` -> `a/b.h`
        std::string_view Inner(std::string_view target)
        {
            return target.size() >= 2 ? target.substr(1, target.size() - 2) : std::string_view{};
        }

        bool EndsWithPath(std::string_view path, std::string_view tail)
        {
            return path.size() >= tail.size() && path.substr(path.size() - tail.size()) == tail &&
                (path.size() == tail.size() || path[path.size() - tail.size() - 1] == '/');
        }

        // `declared` is `a::b` and `qualifier` is `b` (or `a::b`).
        bool NamespaceEndsWith(std::string_view declared, std::string_view qualifier)
        {
            return declared == qualifier ||
                (declared.size() > qualifier.size() && declared.ends_with(qualifier) &&
                declared.substr(declared.size() - qualifier.size() - 2, 2) == "::");
        }

        // Code in namespace `from` sees names of namespace `declared` without
        // qualification when `declared` is `from` or one of its parents.
        bool NamespaceVisible(std::string_view declared, std::string_view from)
        {
            return declared.empty() || declared == from ||
                (from.size() > declared.size() && from.starts_with(declared) &&
                from.substr(declared.size(), 2) == "::");
        }

        // One name the file uses and the header it has to include for it.
        struct MissingUse
        {
            std::uint32_t token = 0;
            std::string display;            // `std::vector`, `lib::Widget`
            std::string key;                // identifies the header: path key or `<vector>`
            std::filesystem::path path;     // project header
            std::string std_target;         // `<vector>` for the standard library
        };

        class Analysis
        {
        public:
            Analysis(const SemanticModel &model, const ProjectContext &context) : m_model(model),
              m_tree(model.Tree()), m_context(context), m_profile(*context.profile), m_sig(model.Significant()),
              m_reporter(model.Tree())
            {
            }

            std::vector<Diagnostic> Run()
            {
                std::vector<Diagnostic> diagnostics;
                m_includes = IncludeAnalyzer::DirectIncludes(m_tree);
                if (!m_profile.includes_known || m_includes.empty() || m_includes.size() != m_profile.entries.size())
                {
                    return diagnostics;
                }

                CollectDirect();
                m_index = ProjectIndex::Build(m_profile);
                CollectLocalNames();
                CollectUsingNamespaces();
                CollectTemplateParameters();

                std::vector<MissingUse> missing;
                std::unordered_set<std::string> reported;
                for (std::size_t p = 0; p < m_sig.size(); ++p)
                {
                    if (!IsIdent(m_sig[p]))
                    {
                        continue;
                    }

                    if (auto use = Classify(p); use && reported.insert(use->key).second)
                    {
                        missing.push_back(std::move(*use));
                    }
                }

                for (const auto & use: missing)
                {
                    const std::optional<std::string> target = use.std_target.empty() ? Spell(use.path)
                                                                                      : std::optional<std::string>(use.std_target);
                    const auto &token = m_tree.Tokens()[use.token];
                    std::string message = "'" + use.display + "' is declared in " +
                        (use.std_target.empty() ? "'" + use.path.filename().generic_string() + "'" : use.std_target) +
                        ", which this file only includes indirectly";
                    TextEdit fix{0, 0, {}};
                    bool has_fix = false;
                    if (target && InsertionPoint(fix.offset))
                    {
                        // The directive's range may or may not take in its line break.
                        const std::string_view source = m_tree.Source();
                        const std::string newline = source.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
                        const bool at_line_start = fix.offset == 0 || source[fix.offset - 1] == '\n';
                        fix.replacement = at_line_start ? "#include " + *target + newline
                                                        : newline + "#include " + *target;
                        has_fix = true;
                        message += "; add #include " + *target;
                    }

                    auto diagnostic = m_reporter.Make(RuleId::IncludeWhatYouUse, "cpp/include-what-you-use",
                        std::move(message), token.offset, token.length, std::move(fix),
                        has_fix ? "Add #include " + *target : std::string());
                    diagnostic.has_fix = has_fix;
                    diagnostics.push_back(std::move(diagnostic));
                }

                SortByOffset(diagnostics);
                return diagnostics;
            }

        private:
            bool IsIdent(std::uint32_t token) const
            {
                const Token &t = m_tree.Tokens()[token];
                return t.kind == TokenKind::Identifier && t.tok == Tok::None;
            }

            bool Is(std::uint32_t token, Tok tok) const
            {
                return m_tree.Tokens()[token].tok == tok;
            }

            std::string_view Text(std::uint32_t token) const
            {
                return m_tree.Text(m_tree.Tokens()[token]);
            }

            // Headers the file includes by name, whether or not under an #if, what
            // its primary header pulls in, and what its direct headers re-export.
            void CollectDirect()
            {
                const std::string stem = Lowercase(m_context.file.stem().string());
                for (const auto & entry: m_profile.entries)
                {
                    m_direct_targets.insert(entry.target);
                    if (entry.conditional)
                    {
                        if (!entry.header.empty())
                        {
                            m_direct_headers.push_back(entry.header);
                            m_direct_keys.insert(Key(entry.header));
                        }

                        continue;
                    }

                    for (const auto & file: entry.closure)
                    {
                        m_closure_names.insert(file.filename().generic_string());
                    }

                    if (entry.header.empty())
                    {
                        continue;
                    }

                    m_direct_headers.push_back(entry.header);
                    m_direct_keys.insert(Key(entry.header));
                    if (Lowercase(entry.header.stem().string()) == stem)
                    {
                        for (const auto & file: entry.closure)
                        {
                            m_primary_keys.insert(Key(file));
                            m_primary_names.insert(file.filename().generic_string());
                        }
                    }

                    if (entry.target.front() == '<' && !m_profile.IsSystemFile(entry.header))
                    {
                        m_prefer_angle = true;
                    }
                }

                // `IWYU pragma: export` on an include of a direct header: its
                // includers may rely on what it forwards.
                for (const auto & header: m_direct_headers)
                {
                    if (m_profile.IsSystemFile(header))
                    {
                        continue;
                    }

                    const auto summary = HeaderSummary::Load(header);
                    for (std::size_t i = 0; i < summary->IncludeCount(); ++i)
                    {
                        if (summary->IncludeReexported(i))
                        {
                            m_reexported.emplace_back(Inner(summary->IncludeTarget(i)));
                        }
                    }
                }
            }

            void CollectLocalNames()
            {
                const auto &symbols = m_model.Symbols();
                for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
                {
                    if (symbols.name[symbol] != kNone)
                    {
                        m_local.insert(std::string(m_model.Names().Text(symbols.name[symbol])));
                    }
                }

                // `class N;` and `friend class N;` name something the file does not
                // need the definition of.
                for (std::size_t p = 1; p < m_sig.size(); ++p)
                {
                    const Tok tok = m_tree.Tokens()[m_sig[p - 1]].tok;
                    if ((tok == Tok::KwClass || tok == Tok::KwStruct || tok == Tok::KwUnion || tok == Tok::KwEnum) &&
                        IsIdent(m_sig[p]))
                    {
                        m_local.insert(std::string(Text(m_sig[p])));
                    }
                }

                const auto &bases = m_model.Bases();
                for (std::size_t i = 0; i < bases.target.size(); ++i)
                {
                    m_external_base = m_external_base || bases.target[i] == kNone;
                }
            }

            // `using namespace a::b;`
            void CollectUsingNamespaces()
            {
                for (std::size_t p = 0; p + 2 < m_sig.size(); ++p)
                {
                    if (!Is(m_sig[p], Tok::KwUsing) || !Is(m_sig[p + 1], Tok::KwNamespace))
                    {
                        continue;
                    }

                    std::string path;
                    for (std::size_t q = p + 2; q < m_sig.size() && !Is(m_sig[q], Tok::Semi); ++q)
                    {
                        if (IsIdent(m_sig[q]))
                        {
                            path += Text(m_sig[q]);
                        }
                        else if (Is(m_sig[q], Tok::ColonColon))
                        {
                            path += "::";
                        }
                    }

                    m_using.insert(std::move(path));
                }
            }

            // Names bound by `template <typename T, class U = int>`.
            void CollectTemplateParameters()
            {
                for (std::size_t p = 0; p + 1 < m_sig.size(); ++p)
                {
                    if (!Is(m_sig[p], Tok::KwTemplate) || !Is(m_sig[p + 1], Tok::Lt))
                    {
                        continue;
                    }

                    int depth = 0;
                    for (std::size_t q = p + 1; q < m_sig.size(); ++q)
                    {
                        const Tok tok = m_tree.Tokens()[m_sig[q]].tok;
                        if (tok == Tok::Lt)
                        {
                            ++depth;
                        }
                        else if (tok == Tok::Gt)
                        {
                            if (--depth == 0)
                            {
                                break;
                            }
                        }
                        else if (tok == Tok::Shr)
                        {
                            depth -= 2;
                            if (depth <= 0)
                            {
                                break;
                            }
                        }
                        else if (depth == 1 && IsIdent(m_sig[q]) && q + 1 < m_sig.size())
                        {
                            const Tok next = m_tree.Tokens()[m_sig[q + 1]].tok;
                            if (next == Tok::Comma || next == Tok::Gt || next == Tok::Eq || next == Tok::Ellipsis)
                            {
                                m_local.insert(std::string(Text(m_sig[q])));
                            }
                        }
                    }
                }
            }

            // Qualifier written before the name at sig position `p` (`a::b` for
            // `a::b::name`); `global` is set for a leading `::`.
            std::string Qualifier(std::size_t p, bool &global) const
            {
                global = false;
                std::vector<std::string_view> parts;
                std::size_t first = p;
                while (first >= 2 && Is(m_sig[first - 1], Tok::ColonColon) && IsIdent(m_sig[first - 2]))
                {
                    parts.push_back(Text(m_sig[first - 2]));
                    first -= 2;
                }

                global = first >= 1 && Is(m_sig[first - 1], Tok::ColonColon);
                std::string out;
                for (auto it = parts.rbegin(); it != parts.rend(); ++it)
                {
                    out += out.empty() ? "" : "::";
                    out += *it;
                }

                return out;
            }

            std::optional<MissingUse> Classify(std::size_t p) const
            {
                const std::uint32_t token = m_sig[p];
                if (p > 0)
                {
                    const Tok before = m_tree.Tokens()[m_sig[p - 1]].tok;
                    if (before == Tok::Dot || before == Tok::Arrow || before == Tok::DotStar || before == Tok::ArrowStar ||
                        before == Tok::KwClass || before == Tok::KwStruct || before == Tok::KwUnion ||
                        before == Tok::KwEnum || before == Tok::KwNamespace)
                    {
                        return std::nullopt;
                    }
                }

                bool global = false;
                const std::string qualifier = Qualifier(p, global);
                const std::string_view name = Text(token);
                if (qualifier == "std" || qualifier.starts_with("std::"))
                {
                    // `std::chrono::seconds` is judged at `chrono`, the first name after std.
                    if (qualifier == "std")
                    {
                        return ClassifyStd(token, name);
                    }

                    return std::nullopt;
                }

                if (m_local.contains(std::string(name)))
                {
                    return std::nullopt;
                }

                return ClassifyProject(token, name, qualifier, global);
            }

            std::optional<MissingUse> ClassifyStd(std::uint32_t token, std::string_view name) const
            {
                const auto &table = StdTable();
                const auto found = table.find(name);
                if (found == table.end())
                {
                    return std::nullopt;
                }

                for (const auto header: found->second)
                {
                    if (m_direct_targets.contains(std::string(header)))
                    {
                        return std::nullopt;
                    }
                }

                // The use must really come through another include: one of the
                // headers has to be in the closures. The first such header is the
                // one to add. Names the primary header brings in are accepted.
                for (const auto header: found->second)
                {
                    const std::string file(Inner(header));
                    if (!m_closure_names.contains(file))
                    {
                        continue;
                    }

                    if (m_primary_names.contains(file))
                    {
                        return std::nullopt;
                    }

                    MissingUse use;
                    use.token = token;
                    use.display = "std::" + std::string(name);
                    use.key = std::string(header);
                    use.std_target = std::string(header);
                    return use;
                }

                return std::nullopt;
            }

            // Namespaces enclosing the token, `a::b`; anonymous ones do not count.
            std::string NamespaceAt(std::uint32_t token) const
            {
                const auto &scopes = m_model.Scopes();
                const auto &symbols = m_model.Symbols();
                const auto &nodes = m_tree.Nodes();
                ScopeId best = kNone;
                std::size_t best_depth = 0;
                for (ScopeId scope = 0; scope < scopes.Size(); ++scope)
                {
                    if (scopes.kind[scope] != ScopeKind::Namespace || scopes.owner[scope] == kNone)
                    {
                        continue;
                    }

                    const auto &node = nodes[scopes.node[scope]];
                    if (token < node.first_token || token >= node.first_token + node.token_count)
                    {
                        continue;
                    }

                    std::size_t depth = 0;
                    for (auto s = scope; s != kNone && s != SemanticModel::TranslationUnitScope; s = scopes.parent[s])
                    {
                        ++depth;
                    }

                    if (depth > best_depth)
                    {
                        best = scope;
                        best_depth = depth;
                    }
                }

                std::vector<std::string_view> parts;
                for (auto s = best; s != kNone && s != SemanticModel::TranslationUnitScope; s = scopes.parent[s])
                {
                    if (scopes.owner[s] != kNone)
                    {
                        parts.push_back(m_model.Names().Text(symbols.name[scopes.owner[s]]));
                    }
                }

                std::string out;
                for (auto it = parts.rbegin(); it != parts.rend(); ++it)
                {
                    out += out.empty() ? "" : "::";
                    out += *it;
                }

                return out;
            }

            std::optional<MissingUse> ClassifyProject(std::uint32_t token, std::string_view name,
                const std::string & qualifier, bool global) const
            {
                const auto providers = m_index.ExportsNamed(name);
                if (providers.empty())
                {
                    return std::nullopt;
                }

                std::string enclosing;
                bool enclosing_known = false;
                std::vector<ProjectIndex::Ref> matches;
                for (const auto & ref: providers)
                {
                    const HeaderSummary &summary = *m_index.Summaries()[ref.summary];
                    const std::string_view declared = summary.ExportNamespace(ref.index);
                    bool matched = false;
                    if (!qualifier.empty())
                    {
                        matched = global ? declared == qualifier : NamespaceEndsWith(declared, qualifier);
                    }
                    else if (global)
                    {
                        matched = declared.empty();
                    }
                    else
                    {
                        // Unqualified: a type is certain to be what a bare name means;
                        // a function or variable could be a member inherited from a
                        // base the file cannot see.
                        const ExportKind kind = summary.Kind(ref.index);
                        const bool is_type = kind == ExportKind::Class || kind == ExportKind::Enum ||
                            kind == ExportKind::TypeAlias;
                        if (!is_type && m_external_base)
                        {
                            continue;
                        }

                        if (!enclosing_known)
                        {
                            enclosing = NamespaceAt(token);
                            enclosing_known = true;
                        }

                        matched = NamespaceVisible(declared, enclosing) || m_using.contains(std::string(declared));
                    }

                    if (matched)
                    {
                        matches.push_back(ref);
                    }
                }

                if (matches.empty())
                {
                    return std::nullopt;
                }

                // Several headers declaring the name (overloads, redeclarations,
                // lookalikes): which one is meant is not ours to guess.
                for (const auto & ref: matches)
                {
                    if (ref.summary != matches.front().summary)
                    {
                        return std::nullopt;
                    }
                }

                const HeaderSummary &summary = *m_index.Summaries()[matches.front().summary];
                if (summary.IsPrivate() || summary.IsTextual() || SatisfiedBy(summary.Path()))
                {
                    return std::nullopt;
                }

                MissingUse use;
                use.token = token;
                use.display = qualifier.empty() ? std::string(name) : qualifier + "::" + std::string(name);
                use.key = Key(summary.Path());
                use.path = summary.Path();
                return use;
            }

            // The file includes `header` itself, gets it through the header it is
            // the implementation of, or a direct include re-exports it.
            bool SatisfiedBy(const std::filesystem::path & header) const
            {
                const std::string key = Key(header);
                if (const auto cached = m_satisfied.find(key); cached != m_satisfied.end())
                {
                    return cached->second;
                }

                bool satisfied = m_direct_keys.contains(key) || m_primary_keys.contains(key) ||
                    std::any_of(m_reexported.begin(), m_reexported.end(),
                    [&](const std::string & tail)
                    {
                        return EndsWithPath(key, tail);
                    });
                // Same file under another spelling (symlink, letter case).
                for (std::size_t i = 0; !satisfied && i < m_direct_headers.size(); ++i)
                {
                    std::error_code ec;
                    satisfied = std::filesystem::equivalent(m_direct_headers[i], header, ec) && !ec;
                }

                m_satisfied.emplace(key, satisfied);
                return satisfied;
            }

            // Where the new include goes: after the last unconditional include.
            bool InsertionPoint(std::size_t & offset) const
            {
                bool found = false;
                for (const auto & include: m_includes)
                {
                    if (!include.conditional)
                    {
                        offset = include.directive_offset + include.directive_length;
                        found = true;
                    }
                }

                return found;
            }

            // How to write the include of a project header so that it resolves.
            std::optional<std::string> Spell(const std::filesystem::path & header) const
            {
                const std::filesystem::path base = m_context.file.has_parent_path() ? m_context.file.parent_path()
                                                                                    : std::filesystem::path();
                std::vector<std::string> candidates;
                const auto add =[&](const std::filesystem::path & dir, bool angled)
                {
                    if (dir.empty())
                    {
                        return;
                    }

                    const auto relative = header.lexically_normal().lexically_relative(dir.lexically_normal());
                    if (relative.empty() || *relative.begin() == "..")
                    {
                        return;
                    }

                    const std::string text = relative.generic_string();
                    candidates.push_back(angled ? "<" + text + ">" : "\"" + text + "\"");
                };

                if (m_context.command != nullptr)
                {
                    for (const auto & dir: m_context.command->include_directories)
                    {
                        add(dir, m_prefer_angle);
                    }

                    for (const auto & dir: m_context.command->quote_directories)
                    {
                        add(dir, false);
                    }
                }

                add(base, false);
                std::stable_sort(candidates.begin(), candidates.end(),
                    [](const std::string & a, const std::string & b)
                    {
                        return a.size() < b.size();
                    });
                for (const auto & candidate: candidates)
                {
                    const auto resolved = IncludeIndex::ResolveIncludeAt(base, "#include " + candidate + "\n", 1,
                        m_context.command);
                    std::error_code ec;
                    if (!resolved.empty() && (Key(resolved) == Key(header) ||
                        (std::filesystem::equivalent(resolved, header, ec) && !ec)))
                    {
                        return candidate;
                    }
                }

                return std::nullopt;
            }

            const SemanticModel &m_model;
            const ParseTree &m_tree;
            const ProjectContext &m_context;
            const IncludeProfile &m_profile;
            const std::pmr::vector<std::uint32_t> &m_sig;
            Reporter m_reporter;
            std::vector<DirectInclude> m_includes;
            ProjectIndex m_index;
            std::unordered_set<std::string> m_direct_targets;
            std::vector<std::filesystem::path> m_direct_headers;
            std::unordered_set<std::string> m_direct_keys;
            std::unordered_set<std::string> m_primary_keys;
            std::unordered_set<std::string> m_primary_names;
            std::unordered_set<std::string> m_closure_names;
            std::vector<std::string> m_reexported;
            std::unordered_set<std::string> m_local;
            std::unordered_set<std::string> m_using;
            bool m_prefer_angle = false;
            bool m_external_base = false;
            mutable std::unordered_map<std::string, bool> m_satisfied;
        };

    } // namespace

    std::vector<Diagnostic> SemanticRules::AnalyzeIncludeWhatYouUse(const SemanticModel &model,
        const ProjectContext &context)
    {
        if (context.profile == nullptr)
        {
            return {};
        }

        return Analysis(model, context).Run();
    }

    std::vector<Diagnostic> SemanticRules::Analyze(const SemanticModel &model, const TypeModel &types,
        const ProjectContext &context)
    {
        auto all = Analyze(model, types);
        for (auto &&part: {AnalyzeFinal(model, context), AnalyzeIncludeWhatYouUse(model, context)})
        {
            all.insert(all.end(), part.begin(), part.end());
        }

        SortByOffset(all);
        return all;
    }

} // namespace heimdall
