#pragma once

#include <Heimdall/CompileDatabase.hpp>
#include <Heimdall/IncludeIndex.hpp>
#include <Heimdall/ParseTree.hpp>
#include <Heimdall/RuleEngine.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace heimdall
{

    // One literal `#include "x"` / `#include <x>` of a file, in source order.
    // Macro includes and `#include_next` have no literal target to judge.
    struct DirectInclude
    {
        std::size_t directive_offset = 0;
        std::size_t directive_length = 0;
        std::size_t target_offset = 0;
        std::size_t target_length = 0;
        // Header name with its delimiters, as written: `<vector>` or `"a.h"`.
        std::string target;
        // Inside an `#if`/`#ifdef` other than the include guard.
        bool conditional = false;
        // Carries `IWYU pragma: keep` or `IWYU pragma: export`.
        bool keep = false;
    };

    // Names a single header file makes available to its includers: names
    // declared at namespace/global scope, enumerators, and macros.
    struct HeaderSymbols;

    // Everything `cpp/no-unused-include` needs to know about the headers a file
    // includes. Building one resolves and reads every transitive header, so it
    // is meant to be cached: it depends only on the file's `#include` lines
    // (see `fingerprint`) and on the headers on disk (see IncludeAnalyzer::IsFresh),
    // not on the rest of the buffer.
    struct IncludeProfile
    {
        struct Entry
        {
            // Header name with its delimiters, as written: `<vector>` or `"a.h"`.
            std::string target;
            // Files whose names count as provided by this include (the header and
            // what it pulls in, minus what other direct includes already cover).
            std::vector<std::shared_ptr<const HeaderSymbols>> providers;
            // False when the rule must stay silent for this include: header not
            // found, incomplete closure, implicit-use header, primary header...
            bool eligible = false;
            // The header lives outside the compiler's system include directories.
            bool project_header = false;
            // The header's include closure contains the analyzed file itself.
            bool circular = false;
            // The include sits in a conditional block.
            bool conditional = false;
            // The header the directive names, found on disk (also for conditional
            // includes); empty when it could not be found.
            std::filesystem::path header;
            // Every file the include pulls in, `header` first. Empty for
            // conditional includes (never walked) and for headers not found.
            std::vector<std::filesystem::path> closure;
            // The closure walk saw every header it was supposed to.
            bool closure_complete = false;
        };

        struct FileStamp
        {
            std::filesystem::path path;
            std::uintmax_t size = 0;
            std::int64_t mtime = 0;
        };

        std::string fingerprint;
        // Compiler default include directories (normalized): what is "system".
        std::vector<std::filesystem::path> system_dirs;
        // Every direct include was found, so no name can come from a header the
        // profile does not know. Unresolved angled includes in conditional blocks
        // (platform headers) do not count against it.
        bool includes_known = true;
        // The analyzed file is a header (cpp/prefer-forward-declaration applies).
        bool is_header_file = false;
        std::vector<Entry> entries;
        std::vector<FileStamp> stamps;

        bool IsSystemFile(const std::filesystem::path& file) const;
    };

    // Two rules share this analysis:
    //
    // `cpp/prefer-forward-declaration`: in a header, a project include whose
    // names are all classes used only as `N *` / `N &` (no body dereferences a
    // pointer to them): a forward declaration would do and spare every includer
    // the parse of the header. Quick fix only.
    //
    // `cpp/no-unused-include`: a direct `#include` none of whose provided names
    // appears in the file. Deliberately conservative: a header counts as used as
    // soon as any identifier of the file (comments and literals aside, macro
    // bodies and conditional branches included) matches a name declared anywhere
    // in the header's include closure. See docs/rule-engine-roadmap.md for the
    // cases it never reports.
    class IncludeAnalyzer
    {
    public:
        // Literal includes of `tree`, in source order; entry i of an IncludeProfile
        // built from the same include block describes element i.
        static std::vector<DirectInclude> DirectIncludes(const ParseTree& tree);

        static std::shared_ptr<const IncludeProfile> BuildProfile(const std::filesystem::path& file,
            const ParseTree& tree, const CompileCommand* command);

        // Same fingerprint scheme as IncludeIndex::IncludeFingerprint.
        static std::string Fingerprint(const std::filesystem::path& file, const ParseTree& tree,
            const CompileCommand* command);

        // True while none of the headers behind the profile changed on disk.
        static bool IsFresh(const IncludeProfile& profile);

        // Diagnostics (not yet filtered by rule overrides or suppressions; run
        // them through RuleEngine::ApplyPolicy). Each carries an unsafe quick fix
        // that deletes the directive line.
        static std::vector<Diagnostic> Analyze(const ParseTree& tree, const IncludeProfile& profile);
    };

} // namespace heimdall
