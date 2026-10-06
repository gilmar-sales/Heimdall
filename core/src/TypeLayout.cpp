#include <Heimdall/TypeLayout.hpp>

#include <algorithm>
#include <array>
#include <filesystem>

namespace heimdall
{

    namespace
    {

        constexpr int kMaxDepth = 24;

        bool IsIdentChar(char c) noexcept
        {
            return (c >= 'a' && c <= 'z') ||(c >= 'A' && c <= 'Z') ||(c >= '0' && c <= '9') || c == '_';
        }

        bool IsSpace(char c) noexcept
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        }

        std::string_view Trim(std::string_view text) noexcept
        {
            while (!text.empty() && IsSpace(text.front()))
            {
                text.remove_prefix(1);
            }

            while (!text.empty() && IsSpace(text.back()))
            {
                text.remove_suffix(1);
            }

            return text;
        }

        std::uint64_t AlignUp(std::uint64_t value, std::uint64_t align) noexcept
        {
            return align <= 1 ? value : (value + align - 1) / align * align;
        }

        std::string JoinPath(const std::vector<std::string>& path, std::size_t count)
        {
            std::string key;
            for (std::size_t i = 0; i < count && i < path.size(); ++i)
            {
                if (i > 0)
                {
                    key += "::";
                }

                key += path[i];
            }

            return key;
        }

        bool StartsWithWord(std::string_view text, std::string_view word) noexcept
        {
            return text.starts_with(word) && (text.size() == word.size() ||!IsIdentChar(text[word.size()]));
        }

        bool EndsWithWord(std::string_view text, std::string_view word) noexcept
        {
            return text.ends_with(word) && (text.size() == word.size() ||!IsIdentChar(text[text.size() - word.size() - 1]));
        }

        // Specifiers that do not change the layout of what they qualify.
        constexpr std::array<std::string_view, 15> kQualifiers = {"const", "volatile", "static", "struct",
            "class",
            "union", "enum", "typename", "constexpr", "inline", "mutable", "thread_local", "extern", "register",
            "__restrict"};

        std::string_view StripQualifiers(std::string_view text) noexcept
        {
            for (bool changed = true; changed;)
            {
                changed = false;
                text = Trim(text);
                for (const std::string_view word : kQualifiers)
                {
                    if (StartsWithWord(text, word))
                    {
                        text.remove_prefix(word.size());
                        changed = true;
                    }
                    else if (EndsWithWord(text, word))
                    {
                        text.remove_suffix(word.size());
                        changed = true;
                    }
                }
            }

            return Trim(text);
        }

        std::optional<std::uint64_t> ParseCount(std::string_view text)
        {
            text = Trim(text);
            while (!text.empty() && (text.back() == 'u' || text.back() == 'U' || text.back() == 'l' || text.back() == 'L'))
            {
                text.remove_suffix(1);
            }

            if (text.empty())
            {
                return std::nullopt;
            }

            std::uint64_t value = 0;
            std::uint64_t base = 10;
            if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
            {
                base = 16;
                text.remove_prefix(2);
            }

            for (const char c : text)
            {
                std::uint64_t digit = 0;
                if (c >= '0' && c <= '9')
                {
                    digit = static_cast<std::uint64_t>(c - '0');
                }
                else if (base == 16 && c >= 'a' && c <= 'f')
                {
                    digit = static_cast<std::uint64_t>(c - 'a' + 10);
                }
                else if (base == 16 && c >= 'A' && c <= 'F')
                {
                    digit = static_cast<std::uint64_t>(c - 'A' + 10);
                }
                else if (c == '\'')
                {
                    continue;
                }
                else
                {
                    return std::nullopt;
                }

                value = value * base + digit;
                if (value >(1ull << 40))
                {
                    return std::nullopt;
                }
            }

            return value;
        }

        // `int`, `unsigned long long`, `long double`, ... as a layout, or nullopt when
        // the words are not purely fundamental.
        std::optional<TypeLayout> Fundamental(std::string_view text, const LayoutTarget& target)
        {
            int longs = 0;
            int shorts = 0;
            std::string_view base;
            bool saw_word = false;
            std::size_t i = 0;
            while (i < text.size())
            {
                while (i < text.size() && IsSpace(text[i]))
                {
                    ++i;
                }

                std::size_t j = i;
                while (j < text.size() && IsIdentChar(text[j]))
                {
                    ++j;
                }

                if (j == i)
                {
                    return std::nullopt;
                }

                const std::string_view word = text.substr(i, j - i);
                i = j;
                saw_word = true;
                if (word == "unsigned" || word == "signed")
                {
                    if (base.empty())
                    {
                        base = "int";
                    }
                }
                else if (word == "long")
                {
                    ++longs;
                }
                else if (word == "short")
                {
                    ++shorts;
                }
                else if (word == "int" || word == "char" || word == "double" || word == "float" || word == "bool" ||
                    word == "wchar_t" || word == "char8_t" || word == "char16_t" || word == "char32_t")
                {
                    if (!base.empty() && base != "int")
                    {
                        return std::nullopt;
                    }

                    // `unsigned char`: the implicit `int` of `unsigned` yields to the real base.
                    if (base.empty() || word != "int")
                    {
                        base = word;
                    }
                }
                else
                {
                    return std::nullopt;
                }
            }

            if (!saw_word)
            {
                return std::nullopt;
            }

            const auto of =[](std::uint64_t size, std::uint64_t align = 0)
            {
                return TypeLayout{size, align == 0 ? size : align, false};
            };
            if (base == "double")
            {
                if (longs > 0)
                {
                    return target.long_double_is_double ? of(8) : of(16);
                }

                return of(8);
            }

            if (base == "float")
            {
                return of(4);
            }

            if (base == "bool" || base == "char" || base == "char8_t")
            {
                return of(1);
            }

            if (base == "char16_t")
            {
                return of(2);
            }

            if (base == "char32_t")
            {
                return of(4);
            }

            if (base == "wchar_t")
            {
                return of(target.long_is_32 && target.pointer_size == 8 ? 2 : 4);
            }

            if (shorts > 0)
            {
                return of(2);
            }

            if (longs >= 2)
            {
                return of(8);
            }

            if (longs == 1)
            {
                return of(target.long_is_32 ? 4 : 8);
            }

            return of(4);
        }

        std::optional<TypeLayout> FixedAlias(std::string_view name, const LayoutTarget& target)
        {
            struct Entry
            {
                std::string_view name;
                std::uint64_t size; // 0: pointer-sized
            };

            static constexpr std::array<Entry, 22> kAliases = {
                {
                    {
                        "int8_t", 1
                    }, {"uint8_t", 1}, {"int16_t", 2},
                        {"uint16_t", 2}, {"int32_t", 4}, {"uint32_t", 4}, {"int64_t", 8}, {"uint64_t", 8}, {"intmax_t", 8},
                        {"uintmax_t", 8}, {"size_t", 0}, {"ssize_t", 0}, {"ptrdiff_t", 0}, {"intptr_t", 0}, {"uintptr_t",
                        0},
                        {"nullptr_t", 0}, {"byte", 1}, {"max_align_t", 16}, {"int_least8_t", 1}, {"int_least16_t", 2},
                        {"int_least32_t", 4}, {"int_least64_t", 8}
            }};
            for (const Entry& entry : kAliases)
            {
                if (entry.name == name)
                {
                    const std::uint64_t size = entry.size == 0 ? target.pointer_size : entry.size;
                    return TypeLayout{size, size, false};
                }
            }

            return std::nullopt;
        }

        std::vector<std::string_view> SplitArgs(std::string_view args)
        {
            std::vector<std::string_view> out;
            int depth = 0;
            std::size_t start = 0;
            for (std::size_t i = 0; i < args.size(); ++i)
            {
                const char c = args[i];
                if (c == '<' || c == '(' || c == '[' || c == '{')
                {
                    ++depth;
                }
                else if (c == '>' || c == ')' || c == ']' || c == '}')
                {
                    --depth;
                }
                else if (c == ',' && depth == 0)
                {
                    out.push_back(Trim(args.substr(start, i - start)));
                    start = i + 1;
                }
            }

            out.push_back(Trim(args.substr(start)));
            return out;
        }

        bool IsSizeKeyword(std::string_view detail, std::string_view keyword) noexcept
        {
            return StartsWithWord(detail, keyword);
        }

    } // namespace

    LayoutTarget LayoutTarget::FromMacros(const Preprocessor::MacroMap& macros)
    {
        const auto has =[&](std::string_view name)
        {
            return macros.find(name) != macros.end();
        };
        LayoutTarget target;
        bool windows = false;
        if (has("_WIN32"))
        {
            windows = true;
        }
        else if (has("__linux__") || has("__unix__") || has("__APPLE__"))
        {
            windows = false;
        }
        else
        {
#ifdef _WIN32
            windows = true;
#endif
        }

        target.long_is_32 = windows;
        if (has("__i386__") || has("_M_IX86") || has("__arm__"))
        {
            target.pointer_size = 4;
            target.long_is_32 = true;
        }

        if (has("_MSC_VER"))
        {
            target.long_double_is_double = true;
        }
        else if (!has("__GNUC__") && !has("__clang__"))
        {
#ifdef _MSC_VER
            target.long_double_is_double = true;
#endif
        }

#if defined(__GLIBCXX__)
        target.native_standard_library =!has("_LIBCPP_VERSION") && !has("_MSVC_STL_VERSION") && !has("_MSC_VER");
        if (const auto abi = macros.find("_GLIBCXX_USE_CXX11_ABI"); abi != macros.end())
        {
            target.native_standard_library = target.native_standard_library &&
                (Trim(abi->second) ==(_GLIBCXX_USE_CXX11_ABI ? "1" : "0"));
        }
#elif defined(_LIBCPP_VERSION)
        target.native_standard_library =!has("__GLIBCXX__") && !has("_MSVC_STL_VERSION") && !has("_MSC_VER");
#elif defined(_MSVC_STL_VERSION)
        target.native_standard_library =!has("__GLIBCXX__") && !has("_LIBCPP_VERSION");
#endif
        return target;
    }

    TypeLayoutResolver::TypeLayoutResolver(const ScopeIndex* local, const ScopeIndex* external,
        LayoutTarget target)
    : m_target(target), m_local(local), m_external(external)
    {
        const ScopeIndex * indexes[] = {local, external};
        for (const ScopeIndex* index : indexes)
        {
            if (index == nullptr)
            {
                continue;
            }

            for (const IndexedScope& scope : *index)
            {
                const std::string key = JoinPath(scope.path, scope.path.size());
                for (const CompletionItem& member : scope.members)
                {
                    // Records and enums are listed by their parent as Namespace members
                    // (detail `struct a::B`); their own scope repeats the name as a Type.
                    const bool tag = member.kind == CompletionKind::Namespace &&
                        (IsSizeKeyword(member.detail, "struct") || IsSizeKeyword(member.detail, "class") ||
                        IsSizeKeyword(member.detail, "union") || IsSizeKeyword(member.detail, "enum"));
                    if ((member.kind != CompletionKind::Type && !tag) ||
                        (member.kind == CompletionKind::Type && !scope.path.empty() && member.label == scope.path.back()))
                    {
                        continue;
                    }

                    std::string name = key.empty() ? member.label : key + "::" + member.label;
                    auto[it, inserted] = m_types.try_emplace(std::move(name));
                    if (inserted ||(!it->second.item->is_definition && member.is_definition))
                    {
                        it->second.item = &member;
                        it->second.path = scope.path;
                    }
                }
            }
        }

        for (const ScopeIndex* index : indexes)
        {
            if (index == nullptr)
            {
                continue;
            }

            for (const IndexedScope& scope : *index)
            {
                if (scope.path.empty() || std::any_of(scope.path.begin(), scope.path.end(),
                    [](const std::string& element)
                    {
                        return element.empty();
                }))
                {
                    continue;
                }

                const auto found = m_types.find(JoinPath(scope.path, scope.path.size()));
                if (found != m_types.end() && found->second.scope == nullptr)
                {
                    found->second.scope = &scope;
                }
            }
        }
    }

    const TypeLayoutResolver::TypeEntry* TypeLayoutResolver::Find(std::string_view name,
        const std::vector<std::string>& context) const
    {
        if (name.starts_with("::"))
        {
            name.remove_prefix(2);
        }

        if (name.empty())
        {
            return nullptr;
        }

        for (std::size_t count = context.size() + 1; count-- > 0;)
        {
            std::string key = JoinPath(context, count);
            if (!key.empty())
            {
                key += "::";
            }

            key += name;
            if (const auto found = m_types.find(key); found != m_types.end())
            {
                return &found->second;
            }
        }

        // Written relative to a scope the caller did not name (`Inner` for `a::Inner`).
        const TypeEntry* best = nullptr;
        const std::string suffix = "::" + std::string(name);
        for (const auto& [key, entry] : m_types)
        {
            if (key.ends_with(suffix) && (best == nullptr ||(!best->item->is_definition && entry.item->is_definition)))
            {
                best = &entry;
            }
        }

        return best;
    }

    std::optional<TypeLayout> TypeLayoutResolver::OfType(std::string_view text,
        const std::vector<std::string>& context,
        bool reference_is_pointer) const
    {
        return OfTypeImpl(text, context, reference_is_pointer, 0);
    }

    std::optional<TypeLayout> TypeLayoutResolver::OfNamed(std::string_view name) const
    {
        return OfType(name);
    }

    const TypeLayoutResolver::TypeEntry* TypeLayoutResolver::FindItem(const CompletionItem& item) const
    {
        if (item.has_location)
        {
            const ScopeIndex * indexes[] = {m_local, m_external};
            for (const auto* index : indexes)
            {
                if (index == nullptr)
                {
                    continue;
                }

                for (const auto& scope : *index)
                {
                    for (const auto& member : scope.members)
                    {
                        if (member.has_location && member.file == item.file && member.offset == item.offset &&
                            member.label == item.label)
                        {
                            std::string name = JoinPath(scope.path, scope.path.size());
                            if (!name.empty())
                            {
                                name += "::";
                            }

                            name += item.label;
                            if (const auto found = m_types.find(name); found != m_types.end())
                            {
                                return &found->second;
                            }
                        }
                    }
                }
            }
        }

        return nullptr;
    }

    std::optional<TypeLayout> TypeLayoutResolver::OfItem(const CompletionItem& item) const
    {
        if (const auto * entry = FindItem(item))
        {
            // Standard library types also have known layouts independent of
            // implementation-private fields that header indexing omits.
            std::string name = JoinPath(entry->path, entry->path.size());
            if (!name.empty())
            {
                name += "::";
            }

            name += item.label;
            return OfType(name);
        }

        return item.type_text.empty() ? OfNamed(item.label) : OfType(item.type_text);
    }

    std::optional<AliasOrigin> TypeLayoutResolver::OriginOf(const CompletionItem& item) const
    {
        if (item.kind != CompletionKind::Type || item.type_text.empty())
        {
            return std::nullopt;
        }

        const auto* entry = FindItem(item);
        std::vector<std::string> context = entry != nullptr ? entry->path : std::vector<std::string> {};
        std::string text = item.type_text;
        std::string documentation;
        for (int depth = 0; depth < kMaxDepth; ++depth)
        {
            const auto* target = Find(StripQualifiers(text), context);
            if (target == nullptr) return AliasOrigin{std::move(text), std::move(documentation)};
            if (!target->item->documentation.empty())
            {
                documentation = target->item->documentation;
            }

            if (target->item->type_text.empty())
            {
                std::string name = JoinPath(target->path, target->path.size());
                if (!name.empty())
                {
                    name += "::";
                }

                name += target->item->label;
                return AliasOrigin{std::move(name), std::move(documentation)};
            }

            text = target->item->type_text;
            context = target->path;
        }

        return std::nullopt;
    }

    std::optional<TypeLayout> TypeLayoutResolver::OfTypeImpl(
        std::string_view text,
        const std::vector<std::string>& context,
        bool reference_is_pointer,
        int depth) const
    {
        if (depth > kMaxDepth)
        {
            return std::nullopt;
        }

        const TypeLayout pointer
        {
            m_target.pointer_size, m_target.pointer_size, false
        };
        std::string_view s = StripQualifiers(text);
        if (s.empty())
        {
            return std::nullopt;
        }

        if (s.find("(*)") != std::string_view::npos)
        {
            return pointer; // function pointer / pointer to array
        }

        if (s.back() == ']')
        {
            const std::size_t open = s.rfind('[');
            if (open == std::string_view::npos)
            {
                return std::nullopt;
            }

            const auto count = ParseCount(s.substr(open + 1, s.size() - open - 2));
            const auto element = OfTypeImpl(s.substr(0, open), context, reference_is_pointer, depth + 1);
            if (!count || *count == 0 ||!element)
            {
                return std::nullopt;
            }

            return TypeLayout{element->size * *count, element->align, false};
        }

        if (s.back() == '*')
        {
            return pointer;
        }

        if (s.back() == '&')
        {
            s.remove_suffix(1);
            if (!s.empty() && s.back() == '&')
            {
                s.remove_suffix(1);
            }

            return reference_is_pointer ? std::optional<TypeLayout>(pointer)
            : OfTypeImpl(s, context, false, depth + 1);
        }

        std::string_view name = s;
        if (name.starts_with("::"))
        {
            name.remove_prefix(2);
        }

        const bool is_std = name.starts_with("std::");
        std::string_view bare = is_std ? name.substr(5) : name;
        const std::size_t angle = bare.find('<');
        if (angle != std::string_view::npos)
        {
            if (bare.back() != '>')
            {
                return std::nullopt;
            }

            const std::string_view base = Trim(bare.substr(0, angle));
            const std::string_view args = bare.substr(angle + 1, bare.size() - angle - 2);
            if (is_std)
            {
                return OfTemplate(base, args, context, depth);
            }

            // `ns::vector<int>` after `using namespace std` is not guessed.
            return std::nullopt;
        }

        if (is_std)
        {
            if (bare == "string" || bare == "wstring" || bare == "u8string" || bare == "u16string" ||
                bare == "u32string")
            {
                return TypeLayout{32, 8, false};
            }

            if (bare == "string_view" || bare == "wstring_view" || bare == "u8string_view" ||
                bare == "u16string_view" || bare == "u32string_view")
            {
                return TypeLayout{2 * m_target.pointer_size, m_target.pointer_size, false};
            }

            // path's representation belongs to the standard library used to
            // build the server. Only use it for the matching native data model.
            if (bare == "filesystem::path" && m_target.native_standard_library && m_target.pointer_size == sizeof(void*) &&
                m_target.long_is_32 ==(sizeof(long) == 4))
            {
                return TypeLayout{sizeof(std::filesystem::path), alignof(std::filesystem::path), false};
            }
        }

        if ((is_std || name.find("::") == std::string_view::npos) && (is_std || bare != "byte"))
        {
            if (const auto fixed = FixedAlias(bare, m_target))
            {
                return fixed;
            }
        }

        if (!is_std)
        {
            if (const auto fundamental = Fundamental(name, m_target))
            {
                return fundamental;
            }
        }

        const TypeEntry* entry = Find(name, context);
        return entry != nullptr ? OfEntry(*entry, depth + 1) : std::nullopt;
    }

    std::optional<TypeLayout> TypeLayoutResolver::OfTemplate(
        std::string_view base,
        std::string_view args,
        const std::vector<std::string>& context,
        int depth) const
    {
        const auto parts = SplitArgs(args);
        const std::uint64_t ptr = m_target.pointer_size;
        if (base == "vector" || base == "basic_string")
        {
            if (base == "vector" && parts.size() > 2)
            {
                return std::nullopt;
            }

            return base == "vector" ? TypeLayout{3 * ptr, ptr, false}: TypeLayout
            {
                32, 8, false
            };
        }

        if (base == "unique_ptr" && parts.size() == 1)
        {
            return TypeLayout{ptr, ptr, false};
        }

        if (base == "shared_ptr" || base == "weak_ptr")
        {
            return TypeLayout{2 * ptr, ptr, false};
        }

        if (base == "basic_string_view" ||(base == "span" && parts.size() == 1))
        {
            return TypeLayout{2 * ptr, ptr, false};
        }

        if (base == "atomic" && parts.size() == 1)
        {
            return OfTypeImpl(parts[0], context, true, depth + 1);
        }

        if (base == "pair" && parts.size() == 2)
        {
            const auto first = OfTypeImpl(parts[0], context, true, depth + 1);
            const auto second = OfTypeImpl(parts[1], context, true, depth + 1);
            if (!first ||!second)
            {
                return std::nullopt;
            }

            const std::uint64_t align = std::max(first->align, second->align);
            const std::uint64_t offset = AlignUp(first->size, second->align);
            return TypeLayout{AlignUp(offset + second->size, align), align, false};
        }

        if (base == "array" && parts.size() == 2)
        {
            const auto element = OfTypeImpl(parts[0], context, true, depth + 1);
            const auto count = ParseCount(parts[1]);
            if (!element ||!count || *count == 0)
            {
                return std::nullopt;
            }

            return TypeLayout{element->size * *count, element->align, false};
        }

        return std::nullopt;
    }

    std::optional<TypeLayout> TypeLayoutResolver::OfEntry(const TypeEntry& entry, int depth) const
    {
        if (depth > kMaxDepth || entry.item == nullptr)
        {
            return std::nullopt;
        }

        const CompletionItem& item = *entry.item;
        if (!item.type_text.empty())
        {
            return OfTypeImpl(item.type_text, entry.path, false, depth + 1);
        }

        if (IsSizeKeyword(item.detail, "enum"))
        {
            // The underlying type rides on the enum's own tag member.
            std::string_view underlying = item.layout_type;
            if (underlying.empty() && entry.scope != nullptr)
            {
                for (const CompletionItem& member : entry.scope->members)
                {
                    if (member.kind == CompletionKind::Type && !entry.scope->path.empty() &&
                        member.label == entry.scope->path.back() && !member.layout_type.empty())
                    {
                        underlying = member.layout_type;
                    }
                }
            }

            return OfTypeImpl(underlying.empty() ? std::string_view("int") : underlying, entry.path, false,
                depth + 1);
        }

        if (IsSizeKeyword(item.detail, "struct") || IsSizeKeyword(item.detail, "class") ||
            IsSizeKeyword(item.detail, "union"))
        {
            return OfRecord(entry, depth);
        }

        return std::nullopt;
    }

    std::optional<std::uint64_t> TypeLayoutResolver::OffsetOf(std::string_view record,
        std::string_view member) const
    {
        std::string_view text = record;
        const TypeEntry* entry = nullptr;
        for (int hops = 0; hops < kMaxDepth; ++hops)
        {
            entry = Find(StripQualifiers(text), {});
            if (entry == nullptr || entry->item == nullptr || entry->item->type_text.empty())
            {
                break;
            }

            text = entry->item->type_text;
        }

        if (entry == nullptr || entry->scope == nullptr)
        {
            return std::nullopt;
        }

        for (const CompletionItem& candidate : entry->scope->members)
        {
            if (candidate.kind == CompletionKind::Variable && candidate.label == member)
            {
                std::optional<std::uint64_t> found;
                if (!OfRecord(*entry, 0, &candidate, &found))
                {
                    return std::nullopt;
                }

                return found;
            }
        }

        return std::nullopt;
    }

    std::optional<std::uint64_t> TypeLayoutResolver::OffsetOfField(const CompletionItem& field) const
    {
        if (field.kind != CompletionKind::Variable ||!field.has_location || field.layout_type.empty() ||
            field.layout_type.starts_with("static "))
        {
            return std::nullopt;
        }

        for (const auto& [key, entry] : m_types)
        {
            if (entry.scope == nullptr || entry.item == nullptr ||!entry.item->type_text.empty() ||
                IsSizeKeyword(entry.item->detail, "enum"))
            {
                continue;
            }

            for (const CompletionItem& member : entry.scope->members)
            {
                if (member.offset == field.offset && member.file == field.file && member.kind == field.kind &&
                    member.label == field.label)
                {
                    std::optional<std::uint64_t> found;
                    if (!OfRecord(entry, 0, &member, &found))
                    {
                        return std::nullopt;
                    }

                    return found;
                }
            }
        }

        return std::nullopt;
    }

    std::optional<TypeLayout> TypeLayoutResolver::OfRecord(
        const TypeEntry& entry,
        int depth,
        const CompletionItem* want,
        std::optional<std::uint64_t>* found) const
    {
        const IndexedScope* scope = entry.scope;
        if (scope == nullptr)
        {
            // Defined without any member the index records: an empty class.
            return entry.item->is_definition ? std::optional<TypeLayout>(TypeLayout{1, 1, true}) : std::nullopt;
        }

        if (scope->layout_unknown ||!scope->template_params.empty())
        {
            return std::nullopt;
        }

        const bool is_union = IsSizeKeyword(entry.item->detail, "union");
        std::uint64_t offset = 0;
        std::uint64_t align = 1;
        bool has_storage = false;
        for (const std::string& base : scope->bases)
        {
            const TypeEntry* found = Find(base, scope->path);
            const auto layout = found != nullptr ? OfEntry(*found, depth + 1) : std::nullopt;
            if (!layout)
            {
                return std::nullopt;
            }

            if (layout->empty)
            {
                continue;
            }

            offset = AlignUp(offset, layout->align) + layout->size;
            align = std::max(align, layout->align);
            has_storage = true;
        }

        std::vector<const CompletionItem*> fields;
        for (const CompletionItem& member : scope->members)
        {
            if (member.kind == CompletionKind::Variable && !member.layout_type.starts_with("static "))
            {
                fields.push_back(&member);
            }
        }

        std::stable_sort(fields.begin(), fields.end(),
            [](const CompletionItem* left, const CompletionItem* right)
            {
                return left->offset < right->offset;
        });
        for (const CompletionItem* field : fields)
        {
            const auto layout = OfTypeImpl(field->layout_type, scope->path, true, depth + 1);
            if (!layout)
            {
                return std::nullopt;
            }

            align = std::max(align, layout->align);
            has_storage = true;
            const std::uint64_t at = is_union ? 0 : AlignUp(offset, layout->align);
            if (want != nullptr && found != nullptr && field->label == want->label && field->offset == want->offset)
            {
                *found = at;
            }

            offset = is_union ? std::max(offset, layout->size) : at + layout->size;
        }

        if (!has_storage)
        {
            return TypeLayout{1, 1, true};
        }

        return TypeLayout{AlignUp(offset, align), align, false};
    }

} // namespace heimdall
