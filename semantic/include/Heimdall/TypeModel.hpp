#pragma once

#include <Heimdall/SemanticModel.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <unordered_map>

namespace heimdall
{

    // Types are 32-bit indices into a hash-consed table (see
    // docs/semantic-engine-architecture.md): equal types have equal ids, so type
    // comparison is integer comparison. `TypeTable::Unknown` (0) means "not known":
    // template-dependent, unresolved, or beyond what this engine models.
    using TypeId = std::uint32_t;

    enum class TypeKind : std::uint8_t
    {
        Unknown,
        Builtin,  // arg = BuiltinType
        Class,    // arg = SymbolId of a class declared in this translation unit
        Enum,     // arg = SymbolId of an enum declared in this translation unit
        External, // arg = id in TypeModel::ExternalNames(): a library type such as `std::vector<int>`
        Pointer,  // arg = pointee
        LRef,     // arg = referee
        RRef,     // arg = referee
        Const,    // arg = qualified type
        Array     // arg = element, extent = bound or kNone
    };

    enum class BuiltinType : std::uint8_t
    {
        Void,
        NullptrT,
        Bool,
        Char,
        SChar,
        UChar,
        WChar,
        Char8,
        Char16,
        Char32,
        Short,
        UShort,
        Int,
        UInt,
        Long,
        ULong,
        LongLong,
        ULongLong,
        SizeT, // `std::size_t`: unsigned, width depends on the platform
        Float,
        Double,
        LongDouble
    };

    // Structure of arrays, like the Binder's tables. Creation is non-const and
    // only happens while the Typer runs; a finished TypeModel is immutable.
    class TypeTable
    {
    public:
        static constexpr TypeId Unknown = 0;

        explicit TypeTable(std::pmr::memory_resource *resource);

        TypeId Builtin(BuiltinType type);
        TypeId Class(SymbolId symbol);
        TypeId Enum(SymbolId symbol);
        TypeId External(NameId name);
        TypeId Pointer(TypeId pointee);
        TypeId LRef(TypeId referee);
        TypeId RRef(TypeId referee);
        TypeId Const(TypeId type);
        TypeId Array(TypeId element, std::uint32_t extent);

        std::size_t Size() const noexcept
        {
            return m_kind.size();
        }
        TypeKind Kind(TypeId type) const noexcept
        {
            return type < m_kind.size() ? m_kind[type] : TypeKind::Unknown;
        }
        std::uint32_t Arg(TypeId type) const noexcept
        {
            return type < m_arg.size() ? m_arg[type] : kNone;
        }
        std::uint32_t Extent(TypeId type) const noexcept
        {
            return type < m_extent.size() ? m_extent[type] : kNone;
        }
        bool IsKnown(TypeId type) const noexcept
        {
            return Kind(type) != TypeKind::Unknown;
        }

        // Removes references: the type of an expression naming the object.
        TypeId Value(TypeId type) const noexcept;
        // Removes references and top-level const.
        TypeId Strip(TypeId type) const noexcept;
        // Arrays become pointers to their element; everything else is unchanged.
        TypeId Decay(TypeId type);

        bool IsBuiltin(TypeId type, BuiltinType expected) const noexcept
        {
            return Kind(type) == TypeKind::Builtin && Arg(type) == static_cast<std::uint32_t>(expected);
        }
        bool IsBool(TypeId type) const noexcept
        {
            return IsBuiltin(type, BuiltinType::Bool);
        }
        // Integer and character types (not `bool`).
        bool IsInteger(TypeId type) const noexcept;
        bool IsFloating(TypeId type) const noexcept;
        // `bool`, integers, floating-point types.
        bool IsArithmetic(TypeId type) const noexcept;
        bool IsPointer(TypeId type) const noexcept
        {
            return Kind(type) == TypeKind::Pointer;
        }
        bool IsArray(TypeId type) const noexcept
        {
            return Kind(type) == TypeKind::Array;
        }
        // Element of an array or pointee of a pointer.
        TypeId Element(TypeId type) const noexcept
        {
            const auto kind = Kind(type);
            return kind == TypeKind::Array || kind == TypeKind::Pointer ? Arg(type) : Unknown;
        }
        // The type, or the elements of the array, are const-qualified.
        bool IsConstQualified(TypeId type) const noexcept;

    private:
        struct Key
        {
            std::uint32_t arg;
            std::uint32_t extent;
            TypeKind kind;
            bool operator== (const Key &) const noexcept = default;
        };
        struct KeyHash
        {
            std::size_t operator() (const Key &key) const noexcept
            {
                std::uint64_t hash = key.arg * 0x9E3779B97F4A7C15ull;
                hash ^= (static_cast<std::uint64_t>(key.extent) << 8 | static_cast<std::uint64_t>(key.kind)) *
                    0xC2B2AE3D27D4EB4Full;
                return static_cast<std::size_t>(hash ^ (hash >> 29));
            }
        };

        TypeId Intern(TypeKind kind, std::uint32_t arg, std::uint32_t extent = kNone);

        std::pmr::vector<TypeKind> m_kind;
        std::pmr::vector<std::uint32_t> m_arg;
        std::pmr::vector<std::uint32_t> m_extent;
        std::pmr::unordered_map<Key, TypeId, KeyHash> m_index;
    };

    // Result of typing one SemanticModel: the declared type of every symbol and
    // the value type of every expression node. Immutable once built, so it can be
    // shared across threads without locks. The SemanticModel (and the tree behind
    // it) must outlive it.
    class TypeModel
    {
    public:
        explicit TypeModel(const SemanticModel &model, std::size_t arena_hint = 32 * 1024);
        TypeModel(TypeModel &&) noexcept = default;

        const SemanticModel &Model() const noexcept
        {
            return *m_model;
        }
        const TypeTable &Types() const noexcept
        {
            return m_types;
        }
        const InternPool &ExternalNames() const noexcept
        {
            return m_externals;
        }
        // Variables, parameters and fields: the declared type. Functions: the
        // return type. Type aliases: the aliased type. Anything else, or anything
        // the engine cannot know: Unknown.
        TypeId SymbolType(SymbolId symbol) const noexcept
        {
            return symbol < m_symbol_type.size() ? m_symbol_type[symbol] : TypeTable::Unknown;
        }
        // Type of the value an expression node produces (never a reference).
        TypeId NodeType(std::uint32_t node) const noexcept
        {
            return node < m_node_type.size() ? m_node_type[node] : TypeTable::Unknown;
        }
        // Human-readable spelling for diagnostics: `int`, `const char*`, `Foo`.
        std::string Spell(TypeId type) const;
        // `std::vector<int>` -> `std::vector`; empty for anything else.
        std::string_view ExternalHead(TypeId type) const;
        std::size_t ArenaBytes() const noexcept
        {
            return m_arena->Used();
        }

    private:
        friend class Typer;
        friend class TyperImpl;

        std::unique_ptr<Arena> m_arena;
        const SemanticModel *m_model;
        InternPool m_externals;
        TypeTable m_types;
        std::pmr::vector<TypeId> m_symbol_type;
        std::pmr::vector<TypeId> m_node_type;
    };

    // `std::vector`, `std::map`, `std::string`...: library containers whose
    // `size()` and `empty()` the Typer knows. `head` is TypeModel::ExternalHead().
    bool IsStdContainerHead(std::string_view head);
    // The subset with `operator[]` over contiguous or random-access elements.
    bool IsStdIndexableHead(std::string_view head);

    // Computes the TypeModel in one pass over the symbols and expression nodes.
    class Typer
    {
    public:
        static TypeModel Type(const SemanticModel &model);
    };

} // namespace heimdall
