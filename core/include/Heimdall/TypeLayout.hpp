#pragma once

#include <Heimdall/Completion.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace heimdall
{

    // Size and alignment of a type, in bytes (`sizeof` / `alignof`).
    struct TypeLayout
    {
        std::uint64_t size = 0;
        std::uint64_t align = 1;
        // Empty class: contributes no storage as a base (empty base optimization).
        bool empty = false;
    };

    // Data-model knobs that change the answer between platforms.
    struct LayoutTarget
    {
        std::uint64_t pointer_size = 8;
        // `long` is 4 bytes (LLP64: Windows) instead of pointer-sized (LP64).
        bool long_is_32 = false;
        // `long double` is a plain double (MSVC) instead of x87 80-bit padded to 16.
        bool long_double_is_double = false;

        static LayoutTarget FromMacros(const Preprocessor::MacroMap &macros);
    };

    // Computes layouts from the declarations a ScopeIndex records: fundamental
    // types, pointers, references, arrays, enums, aliases, a few standard library
    // types, and records (members in declaration order, bases, unions). Anything
    // that cannot be proven (templates, bitfields, virtuals, `alignas`, packing,
    // unknown names) yields nullopt rather than a guess. Indexes must outlive it.
    class TypeLayoutResolver
    {
    public:
        TypeLayoutResolver(const ScopeIndex *local, const ScopeIndex *external, LayoutTarget target);

        // `text` is a type as written, with pointer/array declarators appended
        // (`const Node * const`, `char[16]`). `context` is the scope path the name
        // is looked up from. A reference is as large as its referent unless
        // `reference_is_pointer` (class members store it as a pointer).
        std::optional<TypeLayout> OfType(std::string_view text, const std::vector<std::string> &context = {},
            bool reference_is_pointer = false) const;

        // A record, enum or alias declared by `name` (qualified or not).
        std::optional<TypeLayout> OfNamed(std::string_view name) const;

        // `offsetof(record, member)`: `record` as written (aliases followed), `member`
        // a direct, non-static data member of it.
        std::optional<std::uint64_t> OffsetOf(std::string_view record, std::string_view member) const;

        // Offset of an indexed data member inside the record that declares it, found
        // by label + declaration site. Nullopt for anything that is not an instance
        // field of a record with a known layout.
        std::optional<std::uint64_t> OffsetOfField(const CompletionItem &field) const;

    private:
        struct TypeEntry
        {
            const CompletionItem *item = nullptr;
            const IndexedScope *scope = nullptr;
            std::vector<std::string> path; // scope path of the entry's parent
        };

        std::optional<TypeLayout> OfTypeImpl(std::string_view text, const std::vector<std::string> &context,
            bool reference_is_pointer, int depth) const;
        std::optional<TypeLayout> OfEntry(const TypeEntry &entry, int depth) const;
        // `want`: a field whose offset is stored in `found` while the record is laid out.
        std::optional<TypeLayout> OfRecord(const TypeEntry &entry, int depth, const CompletionItem *want = nullptr,
            std::optional<std::uint64_t> *found = nullptr) const;
        std::optional<TypeLayout> OfTemplate(std::string_view base, std::string_view args,
            const std::vector<std::string> &context, int depth) const;
        const TypeEntry *Find(std::string_view name, const std::vector<std::string> &context) const;

        LayoutTarget m_target;
        std::unordered_map<std::string, TypeEntry> m_types;
    };

} // namespace heimdall
