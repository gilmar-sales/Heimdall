#pragma once

#include <Heimdall/CompileDatabase.hpp>
#include <Heimdall/Completion.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
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
    // Also follow `#include_next` (searching the directories after the one
    // that supplied the including header). Off by default: completion only
    // needs the first definition of each name.
    bool follow_include_next = false;
};

// Where an include completion candidate was found.
enum class IncludeOrigin
{
    Local,    // the including file's own directory (`"..."` only)
    Quote,    // -iquote (`"..."` only)
    Include,  // -I / -isystem / -idirafter
    System,   // compiler default include directories
    Absolute  // the typed path was absolute
};

// One entry offered after `#include "` or `#include <`. `label` is the next
// path segment; directories end in `/`.
struct IncludeCandidate
{
    std::string label;
    bool directory = false;
    IncludeOrigin origin = IncludeOrigin::Include;
    std::filesystem::path location;
};

// The cursor sits inside the delimiters of an `#include` line.
struct IncludeContext
{
    bool angled = false;
    // Offset of the first character after the opening `<` or `"`.
    std::size_t typed_offset = 0;
};

// Out-parameter of ResolveHeaders: whether the walk saw every header it was
// supposed to. Unresolved includes inside system headers are ignored (they are
// platform branches); anything else that is missing, unreadable or cut by a
// limit makes `complete` false.
struct ResolveReport
{
    bool complete = true;
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
                                                             const Limits& limits = Limits {},
                                                             ResolveReport* report = nullptr);
    // Completion of the path typed after `#include "` (angled = false) or
    // `#include <` (angled = true). `typed` is the text between the opening
    // delimiter and the cursor, e.g. `Heimdall/Le`. Quoted includes also search
    // `base_dir` and the -iquote directories; angled ones never do, exactly as
    // the compiler resolves them. Duplicates across directories collapse to the
    // first one found; sources, binaries and dotfiles are skipped.
    static std::vector<IncludeCandidate> CompleteIncludePath(const std::filesystem::path& base_dir, bool angled,
                                                             std::string_view typed,
                                                             const CompileCommand* command,
                                                             std::size_t max_results = 1000);
    // Include context when `offset` is after `#include <` / `#include "` (also
    // `#include_next`) and before any closing delimiter on that line.
    static std::optional<IncludeContext> IncludeContextAt(std::string_view text, std::size_t offset);
    static std::string CacheKey(const std::vector<std::filesystem::path>& headers,
                                const CompileCommand* command);
    // Cheap fingerprint of the file's own `#include` lines plus the search
    // configuration. Lets the LSP skip ResolveHeaders (which stats + reads +
    // lexes every transitive header) when the including file's include block
    // did not change: no disk I/O at all on the fast path.
    static std::string IncludeFingerprint(const std::filesystem::path& base_dir,
                                          std::string_view text, const CompileCommand* command);
    // Header named by the `#include` line containing `offset`, or empty when
    // that line is not an include or the header cannot be found.
    static std::filesystem::path ResolveIncludeAt(const std::filesystem::path& base_dir,
                                                  std::string_view text, std::size_t offset,
                                                  const CompileCommand* command);
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
