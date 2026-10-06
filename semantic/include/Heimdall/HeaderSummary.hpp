#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace heimdall
{
    class SemanticModel;

    enum class ExportKind : std::uint8_t
    {
        Class,
        Enum,
        TypeAlias,
        Function,
        Variable,
        Enumerator
    };

    // What one header offers to the files that include it (see
    // docs/semantic-engine-architecture.md, section 8): the names it declares at
    // namespace or global scope, its classes with their bases, and its direct
    // includes. Built once from a bound SemanticModel of the header and then
    // shared, immutable, between every document that includes it.
    //
    // Text lives in one pool owned by the summary, so a summary stays valid
    // after the header's buffer and parse tree are gone. Everything is stored in
    // parallel arrays of 32-bit ids (structure of arrays).
    class HeaderSummary
    {
    public:
        // Process-wide cache keyed by path and validated by size and mtime: the
        // same header is bound once however many documents include it.
        static std::shared_ptr<const HeaderSummary> Load(const std::filesystem::path& path);

        // Uncached summary of `source`, for tests and for headers not on disk.
        static std::shared_ptr<const HeaderSummary> FromSource(std::string_view source,
            std::filesystem::path path = {});

        // Reuses an already-bound model instead of parsing/binding a second AST.
        static std::shared_ptr<const HeaderSummary> FromModel(const SemanticModel& model,
            std::filesystem::path path = {});

        // Conservative semantic fingerprint: all source bytes participate, since
        // inline bodies, macros and __LINE__ can change the exported meaning.
        std::uint64_t SemanticFingerprint() const noexcept
        {
            return m_fingerprint;
        }

        const std::filesystem::path& Path() const noexcept
        {
            return m_path;
        }

        // False when the file could not be read (missing, over the size limit).
        bool Readable() const noexcept
        {
            return m_readable;
        }

        // Carries `// IWYU pragma: private`: not meant to be included directly.
        bool IsPrivate() const noexcept
        {
            return m_private;
        }

        // The header is a textual include (`.inc`, `.def`, `.inl`, `.tpp`...):
        // it cannot stand on its own, so it is never suggested as an include.
        bool IsTextual() const noexcept
        {
            return m_textual;
        }

        // ---- exported names ----------------------------------------------
        std::size_t ExportCount() const noexcept
        {
            return m_export_name.size();
        }

        std::string_view ExportName(std::size_t i) const noexcept
        {
            return Text(m_export_name[i]);
        }

        // Enclosing namespaces as written, `a::b`; empty for the global namespace.
        std::string_view ExportNamespace(std::size_t i) const noexcept
        {
            return Text(m_export_ns[i]);
        }

        ExportKind Kind(std::size_t i) const noexcept
        {
            return m_export_kind[i];
        }

        // ---- direct includes ---------------------------------------------
        std::size_t IncludeCount() const noexcept
        {
            return m_include_target.size();
        }

        // Header name with its delimiters, as written: `<vector>` or `"a.h"`.
        std::string_view IncludeTarget(std::size_t i) const noexcept
        {
            return Text(m_include_target[i]);
        }

        // Marked `// IWYU pragma: export`: includers may rely on it.
        bool IncludeReexported(std::size_t i) const noexcept
        {
            return m_include_reexport[i] != 0;
        }

        // ---- classes -----------------------------------------------------
        // Classes, structs and unions with a body, at namespace or global scope
        // (not in an anonymous namespace).
        std::size_t ClassCount() const noexcept
        {
            return m_class_name.size();
        }

        std::string_view ClassName(std::size_t c) const noexcept
        {
            return Text(m_class_name[c]);
        }

        std::string_view ClassNamespace(std::size_t c) const noexcept
        {
            return Text(m_class_ns[c]);
        }

        // Declares a virtual, override, final or pure member function.
        bool ClassHasVirtual(std::size_t c) const noexcept
        {
            return (m_class_flags[c] & kClassVirtual) != 0;
        }

        bool ClassIsFinal(std::size_t c) const noexcept
        {
            return (m_class_flags[c] & kClassFinal) != 0;
        }

        bool ClassIsTemplate(std::size_t c) const noexcept
        {
            return (m_class_flags[c] & kClassTemplate) != 0;
        }

        std::size_t BaseCount(std::size_t c) const noexcept
        {
            return m_class_base_count[c];
        }

        // Last identifier of the written base name (`ns::Base<T>` gives `Base`).
        std::string_view BaseName(std::size_t c, std::size_t k) const noexcept
        {
            return Text(m_base_name[m_class_first_base[c] + k]);
        }

        std::size_t PoolBytes() const noexcept
        {
            return m_pool.size();
        }

    private:
        friend class HeaderSummaryBuilder;
        static constexpr std::uint8_t kClassVirtual = 1;
        static constexpr std::uint8_t kClassFinal = 2;
        static constexpr std::uint8_t kClassTemplate = 4;

        std::string_view Text(std::uint32_t id) const noexcept
        {
            return std::string_view(m_pool).substr(m_span_begin[id], m_span_end[id] - m_span_begin[id]);
        }

        std::filesystem::path m_path;
        bool m_readable = false;
        bool m_private = false;
        bool m_textual = false;
        std::uint64_t m_fingerprint = 0;

        std::string m_pool;
        std::vector<std::uint32_t> m_span_begin;
        std::vector<std::uint32_t> m_span_end;

        std::vector<std::uint32_t> m_export_name;
        std::vector<std::uint32_t> m_export_ns;
        std::vector<ExportKind> m_export_kind;

        std::vector<std::uint32_t> m_include_target;
        std::vector<std::uint8_t> m_include_reexport;

        std::vector<std::uint32_t> m_class_name;
        std::vector<std::uint32_t> m_class_ns;
        std::vector<std::uint8_t> m_class_flags;
        std::vector<std::uint32_t> m_class_first_base;
        std::vector<std::uint32_t> m_class_base_count;
        std::vector<std::uint32_t> m_base_name;
    };

} // namespace heimdall
