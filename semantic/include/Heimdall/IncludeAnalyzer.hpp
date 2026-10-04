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
        };

        struct FileStamp
        {
            std::filesystem::path path;
            std::uintmax_t size = 0;
            std::int64_t mtime = 0;
        };

        std::string fingerprint;
        std::vector<Entry> entries;
        std::vector<FileStamp> stamps;
    };

    // `cpp/no-unused-include`: a direct `#include` none of whose provided names
    // appears in the file. Deliberately conservative: a header counts as used as
    // soon as any identifier of the file (comments and literals aside, macro
    // bodies and conditional branches included) matches a name declared anywhere
    // in the header's include closure. See docs/rule-engine-roadmap.md for the
    // cases it never reports.
    class IncludeAnalyzer
    {
    public:
        static std::shared_ptr<const IncludeProfile> BuildProfile(const std::filesystem::path & file,
            const ParseTree &tree, const CompileCommand *command);

        // Same fingerprint scheme as IncludeIndex::IncludeFingerprint.
        static std::string Fingerprint(const std::filesystem::path & file, const ParseTree &tree,
            const CompileCommand *command);

        // True while none of the headers behind the profile changed on disk.
        static bool IsFresh(const IncludeProfile &profile);

        // Diagnostics (not yet filtered by rule overrides or suppressions; run
        // them through RuleEngine::ApplyPolicy). Each carries an unsafe quick fix
        // that deletes the directive line.
        static std::vector<Diagnostic> Analyze(const ParseTree &tree, const IncludeProfile &profile);
    };

} // namespace heimdall
