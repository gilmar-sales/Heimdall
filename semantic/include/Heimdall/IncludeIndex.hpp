#pragma once

#include <Heimdall/CompileDatabase.hpp>
#include <Heimdall/Completion.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace heimdall
{

struct IncludeLimits
{
    std::size_t max_headers = 256;
    std::size_t max_file_bytes = 1 << 20;
    int max_depth = 16;
};

// Transitive header index for completion: resolves `#include`s (quoted and
// angled, honoring -I/-iquote/-isystem plus compiler default paths) and
// parses each header into qualified scopes, so e.g. `std::` from
// `#include <iostream>` resolves. Headers are read from disk; the including
// buffer itself is only scanned for `#include` lines.
class IncludeIndex
{
  public:
    using Limits = IncludeLimits;

    // Fast path (scan + stat, no parsing) for cache-key computation.
    static std::vector<std::filesystem::path> ResolveHeaders(const std::filesystem::path& base_dir,
                                                             std::string_view text,
                                                             const CompileCommand* command,
                                                             const Limits& limits = Limits {});
    static std::string CacheKey(const std::vector<std::filesystem::path>& headers,
                                const CompileCommand* command);
    // Cheap fingerprint of the file's own `#include` lines plus the search
    // configuration. Lets the LSP skip ResolveHeaders (which stats + reads +
    // lexes every transitive header) when the including file's include block
    // did not change: no disk I/O at all on the fast path.
    static std::string IncludeFingerprint(const std::filesystem::path& base_dir,
                                          std::string_view text, const CompileCommand* command);
    static IncludeIndex Build(const std::vector<std::filesystem::path>& headers,
                              const CompileCommand* command, const Limits& limits = Limits {});
    static IncludeIndex Build(const std::filesystem::path& base_dir, std::string_view text,
                              const CompileCommand* command, const Limits& limits = Limits {});

    // Default compiler system include directories, cached per compiler
    // executable. Empty when undiscoverable (unknown driver, spawn failure).
    static std::vector<std::filesystem::path> SystemIncludes(std::string_view compiler);

    const ScopeIndex& Scopes() const noexcept { return m_scopes; }
    bool Empty() const noexcept { return m_scopes.empty(); }
    // Header files in index order; CompletionItem::file indexes into this.
    const std::vector<std::filesystem::path>& Files() const noexcept { return m_files; }

  private:
    ScopeIndex m_scopes;
    std::vector<std::filesystem::path> m_files;
};

} // namespace heimdall
