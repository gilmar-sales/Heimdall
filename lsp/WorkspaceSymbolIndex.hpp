#pragma once

#include "Document.hpp"

#include <Heimdall/SymbolOutline.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace heimdall::lsp
{

    /// A declaration as `workspace/symbol` reports it, borrowed from its `IndexedFile`.
    struct SymbolView
    {
        std::string_view name;
        /// Qualified path of the enclosing scopes; anonymous scopes are not part of it.
        std::string_view container;
        heimdall::OutlineKind kind = heimdall::OutlineKind::Variable;
        /// The name, already in LSP units of the text that was indexed, so a later edit
        /// cannot shift it.
        Position start;
        Position end;
    };

    /// Compact record of one symbol; the strings live in the owning `IndexedFile`.
    struct IndexedSymbol
    {
        std::uint32_t nameOffset    = 0;
        std::uint32_t nameLength    = 0;
        std::uint32_t container     = 0;
        std::uint32_t startLine     = 0;
        std::uint32_t startCharacter = 0;
        std::uint32_t endLine       = 0;
        std::uint32_t endCharacter  = 0;
        heimdall::OutlineKind kind  = heimdall::OutlineKind::Variable;
    };

    /// Immutable symbols of one file revision, stored as parallel columns so a query scans
    /// the cheap filter columns before it touches any text.
    struct IndexedFile
    {
        std::string uri;
        /// Every symbol name, back to back.
        std::string names;
        /// Distinct containers; entry 0 is the empty (global) container.
        std::vector<std::string> containers = {std::string()};
        std::vector<IndexedSymbol> symbols;
        /// Characters present in the name / in the qualified name (see `CharacterMask`).
        std::vector<std::uint64_t> nameMasks;
        std::vector<std::uint64_t> qualifiedMasks;

        [[nodiscard]] SymbolView View(std::size_t index) const noexcept;
    };

    using IndexedFilePtr = std::shared_ptr<const IndexedFile>;

    /// A query result; keeps the file revision alive while the response is built.
    struct SymbolHit
    {
        IndexedFilePtr file;
        std::size_t index = 0;

        [[nodiscard]] SymbolView Symbol() const noexcept
        {
            return file->View(index);
        }
    };

    /// Converts an outline to LSP positions. Anonymous scopes only act as containers and are
    /// not searchable, so they are left out.
    [[nodiscard]] IndexedFilePtr BuildIndexedFile(std::string uri,
                                                  std::span<const heimdall::OutlineSymbol> symbols,
                                                  const LineIndex& lines);

    /// One bit per character class of `text` (letters and digits case-folded, `_`, `:`, the
    /// rest hashed). Whatever matches a query contains every character of the query, so a
    /// symbol whose mask lacks one of the query's bits can be rejected without reading it.
    [[nodiscard]] std::uint64_t CharacterMask(std::string_view text) noexcept;

    /// Syntactic symbols of the workspace for `workspace/symbol`.
    ///
    /// Files come in two layers: what is on disk, and what is in an open editor buffer. A
    /// buffer shadows the disk entry of the same file. Every entry is an immutable
    /// `IndexedFile` that is swapped in whole, and a query copies the entry pointers under a
    /// short lock and then scans without holding it, so indexing never blocks a query and a
    /// query never sees half of a file.
    class WorkspaceSymbolIndex
    {
    public:
        static constexpr std::size_t kDefaultLimit = 200;

        void SetDisk(const std::string& key, IndexedFilePtr file);

        void RemoveDisk(const std::string& key);

        /// Drops `key` and every entry below it, for a folder that was deleted or moved.
        void RemoveDiskUnder(const std::string& key);

        void SetOpen(const std::string& key, IndexedFilePtr file);

        void ClearOpen(const std::string& key);

        /// Case-insensitive match of `query` against symbol names and qualified names, best
        /// first: exact name, name prefix, name substring, subsequence of the name, subsequence
        /// of the qualified name. Equal ranks order by name length, name, container, uri and
        /// position, so results are deterministic. A cancelled query yields nothing.
        [[nodiscard]] std::vector<SymbolHit> Query(std::string_view query,
                                                   std::size_t limit    = kDefaultLimit,
                                                   std::stop_token stop = {}) const;

        [[nodiscard]] std::size_t FileCount() const;

    private:
        mutable std::shared_mutex mMutex;
        std::unordered_map<std::string, IndexedFilePtr> mDisk;
        std::unordered_map<std::string, IndexedFilePtr> mOpen;
    };

} // namespace heimdall::lsp
