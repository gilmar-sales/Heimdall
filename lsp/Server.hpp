#pragma once

#include "Document.hpp"
#include "ThreadPool.hpp"

#include <Heimdall/CompileDatabase.hpp>
#include <Heimdall/AnalysisFeatures.hpp>
#include <Heimdall/IncludeAnalyzer.hpp>
#include <Heimdall/IncludeIndex.hpp>
#include <Heimdall/ParseTree.hpp>
#include <Heimdall/RuleEngine.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <simdjson.h>
#include <string>
#include <stop_token>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace heimdall::lsp
{

    class LanguageServer
    {
    public:
        LanguageServer();

        ~LanguageServer();

        bool Run();

    private:
        // Immutable per-version snapshot: published by the document worker, consumed by
        // workers without locking or copying. The ParseTree borrows this buffer
        // through shared ownership (see CachedParse), so replacing the text never
        // invalidates a tree being analyzed concurrently.
        struct DocumentSnapshot
        {
            heimdall::DocumentId document = heimdall::InvalidDocument;
            std::shared_ptr<const std::string> text = std::make_shared<const std::string>();
            std::int64_t version = 0;
            std::shared_ptr<const LineIndex> lines = std::make_shared<const LineIndex>();
            // Lossless tokens of `text`, kept in step with it by incremental re-lexing.
            std::shared_ptr<const std::vector<heimdall::Token>> tokens;
        };

        // A document as it was when a request arrived.
        struct PinnedDocument
        {
            std::string uri;
            DocumentSnapshot snapshot;
        };

        struct RequestJob;
        // Per-request state, visible to the handler running on a pool thread
        // (and to the diagnostics worker, which only sets `stop`).
        struct RequestContext
        {
            heimdall::AnalysisSnapshot analysis;
            std::stop_token stop;
            std::optional<PinnedDocument> pinned;
            // Pins absence too: a later didOpen must not change an earlier request.
            std::string document_uri;
            bool interactive = false;
            std::shared_ptr<const heimdall::ParseTree> parsed;
            std::exception_ptr parse_failure;
        };

        using RequestHandler = std::move_only_function<void(simdjson::dom::element, std::string_view) >;
        struct RequestJob
        {
            std::string body;
            std::string id;
            RequestContext context;
            RequestHandler handler;
        };

        struct HeaderView
        {
            std::shared_ptr<const heimdall::IncludeIndex> index;
            bool complete = true;
        };

        struct IndexJob
        {
            std::string key;
            std::vector<std::filesystem::path> headers;
            const heimdall::CompileCommand* command = nullptr;
            std::shared_ptr<const heimdall::SourceOverlay> overlay;
        };

        struct DiagJob
        {
            std::string uri;
            std::int64_t version = 0;
        }

        ;

        void LoadInitializationOptions(simdjson::dom::element request);

        void Respond(std::string_view id, std::string_view result);

        void PublishDiagnostics(const std::string& uri, std::shared_ptr<const std::string> text,
            std::int64_t version);

        bool DocumentParams(simdjson::dom::element request, std::string_view& uri,
            simdjson::dom::object& document);

        void OpenDocument(simdjson::dom::element request);

        void ChangeDocument(simdjson::dom::element request);

        void CloseDocument(simdjson::dom::element request);

        void FormatDocument(simdjson::dom::element request, std::string_view id);

        void RangeFormatDocument(simdjson::dom::element request, std::string_view id);

        void CodeActions(simdjson::dom::element request, std::string_view id);

        // mode: 0 = references, 1 = prepareRename, 2 = rename.
        void RenameDocument(simdjson::dom::element request, std::string_view id, int mode);

        void CompleteDocument(simdjson::dom::element request, std::string_view id);

        // `#include "` / `#include <` path completion; quoted and angled
        // includes search different directories.
        void RespondIncludeCompletion(
            std::string_view id,
            const std::string& uri,
            const std::string& text,
            const LineIndex& lines,
            std::size_t offset,
            const heimdall::IncludeContext& context,
            const heimdall::CompileCommand* command);

        void HoverDocument(simdjson::dom::element request, std::string_view id);

        void GotoDocument(simdjson::dom::element request, std::string_view id, bool implementation);

        // Non-blocking variant of HeaderScopes for the interactive path.
        // Never sleeps: on a fingerprint miss it enqueues the build and returns
        // the last good (or empty) index with complete=false, so hover/goto
        // answer in microseconds and the client re-requests once indexing lands.
        // Kept as a separate name so call sites cannot accidentally reintroduce
        // the old 25ms-poll loop on pool threads.
        HeaderView AwaitHeaderScopes(const std::string& uri, const std::shared_ptr<const std::string>& text,
            const heimdall::CompileCommand* command);

        HeaderView HeaderScopes(const std::string& uri, const std::shared_ptr<const std::string>& text,
            const heimdall::CompileCommand* command);

        std::shared_ptr<const heimdall::ParseTree> CachedParse(
            const std::string& uri,
            const std::shared_ptr<const std::string>& text,
            std::int64_t version,
            const heimdall::CompileCommand* command);

        heimdall::ParserOptions ParserOptionsFor(const heimdall::CompileCommand* command);

        // Rule-engine diagnostics plus, when semantic analysis is on and the file
        // has a compile command, cpp/no-unused-include (policy already applied).
        heimdall::AnalysisContext AnalysisFor(const std::string& uri,
            std::shared_ptr<const heimdall::ParseTree> tree, heimdall::ParserOptions options);

        std::vector<heimdall::Diagnostic> RuleDiagnostics(const std::string& uri,
            std::shared_ptr<const heimdall::ParseTree> tree,
            const heimdall::CompileCommand* command);

        // Workspace-wide linting: after `initialized`, a background pass publishes
        // diagnostics for every C++ source under the workspace root that is not
        // open in the editor, so the Problems panel shows the project total.
        void WorkspaceScanMain(std::stop_token stop);

        // Returns the number of diagnostics published, or -1 when the file was
        // skipped (open in the editor, unreadable, cancelled).
        int PublishWorkspaceFile(const std::filesystem::path& file, std::stop_token stop);

        static bool IsWorkspaceSource(const std::filesystem::path& file);

        void IndexWorkerMain(std::stop_token stop);

        void DiagWorkerMain(std::stop_token stop);

        void EnqueueDiagnostics(const std::string& uri, std::int64_t version);

        void FlushDiagnostics();

        // The smallest single edit covering every change applied so far.
        struct EditHull
        {
            bool valid = false;
            heimdall::Lexer::TextEdit edit;

            void Add(const heimdall::Lexer::TextEdit& next) noexcept
            {
                edit = valid ? heimdall::Lexer::Compose(edit, next) : next;
                valid = true;
            }
        }

        ;

        static bool ApplyContentChange(
            std::string& current,
            LineIndex& index,
            std::vector<heimdall::Token>& tokens,
            EditHull& hull,
            simdjson::dom::object change);

        // Requires m_index_cache_mu to be held by the caller.
        void TouchGlobalIndex(const std::string& key);

        // Runs `handler` on the pool against a private copy of the message, so
        // the I/O thread goes straight back to reading (and to $/cancelRequest).
        void Dispatch(
            std::string_view body,
            simdjson::dom::element request,
            const std::string& id,
            RequestHandler handler);

        void ExecuteRequest(const std::shared_ptr<RequestJob>& job);

        void QueueDocument(std::string_view body);

        void DrainRequests();

        // Snapshot of `uri`: the one pinned at arrival for the current request,
        // otherwise the latest.
        std::optional<DocumentSnapshot> GetDocument(const std::string& uri);

        const heimdall::CompileCommand* CommandFor(const std::string& uri);

        static std::stop_token CurrentStop();

        static bool RequestCancelled();

        void RespondCancelled(std::string_view id);

        void RespondInternalError(std::string_view id);

        bool IsCurrentVersion(const std::string& uri, std::int64_t version);

        static thread_local const RequestContext* t_context;

        bool m_versioned_edits = false;

        // m_docs_mu guards only m_documents (hot: every request and keystroke),
        // so readers never queue behind cache bookkeeping under the sharded
        // parse/index/macro/profile locks below.
        std::shared_mutex m_docs_mu;
        heimdall::Workspace m_workspace;
        std::unordered_map<std::string, DocumentSnapshot> m_documents;

        std::mutex m_inflight_mu;
        std::unordered_map<std::string, std::stop_source> m_inflight;

        // Sharded cache locks (latency fix for <50ms p95):
        // - m_parse_mu guards only m_parse_cache (hot: every completion/hover/diag).
        // - m_index_cache_mu guards m_include_cache + m_global_indices + m_lru +
        //   m_index_pending. TouchGlobalIndex requires it to be held.
        // - m_macro_mu guards m_macro_cache (grows rarely, read often).
        // - m_profile_mu guards m_include_profiles.
        // - m_init_mu guards m_compile_database + m_rule_options +
        //   m_initialization_error.
        // The previous single m_mu serialized all of these against each other.
        std::mutex m_parse_mu;
        std::mutex m_index_cache_mu;
        std::mutex m_macro_mu;
        std::mutex m_profile_mu;
        std::mutex m_init_mu;
        std::shared_ptr<const heimdall::CompileDatabase> m_compile_database;
        heimdall::RuleOptions m_rule_options;

        // Header discovery cache per open document: the fingerprint covers the
        // file's own `#include` block plus search flags, so repeat keystrokes
        // skip ResolveHeaders entirely (no stat/read/lex). The built index is
        // shared globally by content key: the same <vector> is parsed once even
        // with several files open.
        struct IncludeCacheEntry
        {
            std::string fingerprint;
            std::vector<std::filesystem::path> headers;
            std::string requested_key;
            std::string served_key;
        };

        std::unordered_map<std::string, IncludeCacheEntry> m_include_cache;
        // cpp/no-unused-include: one profile per document, rebuilt only when the
        // include block, the search flags or a header on disk change.
        std::unordered_map<std::string, std::shared_ptr<const heimdall::IncludeProfile>> m_include_profiles;
        struct GlobalIndexEntry
        {
            std::shared_ptr<const heimdall::IncludeIndex> index;
            std::list<std::string>::iterator lru;
        };

        std::unordered_map<std::string, GlobalIndexEntry> m_global_indices;
        std::list<std::string> m_lru;
        static constexpr std::size_t kMaxGlobalIndices = 16;
        std::unordered_set<std::string> m_index_pending;
        std::deque<IndexJob> m_index_queue;

        struct ParseSlot
        {
            struct Waiter
            {
                std::atomic<bool> resumed{false};
                std::function<void() > resume;
                std::optional<std::stop_callback<std::function<void() >>> cancellation;

                void Wake()
                {
                    if (!resumed.exchange(true, std::memory_order_acq_rel))
                    {
                        // A cancelled consumer must release its pinned snapshot
                        // even when the common parse is still running.
                        auto continuation = std::move(resume);
                        continuation();
                    }
                }
            };

            std::mutex mu;
            std::condition_variable_any cv;
            std::shared_ptr<const heimdall::ParseTree> tree;
            std::exception_ptr failure;
            std::vector<std::shared_ptr<Waiter>> waiters;
            bool started = false;
            // Set (release) only after a successful parse is published.
            std::atomic<bool> ready{false};
        };

        // Internal suspension, caught before a request has produced a response.
        struct ParsePending
        {
            std::shared_ptr<ParseSlot> slot;
        };

        struct ParseCacheEntry
        {
            std::int64_t version = -1;
            std::shared_ptr<const std::string> text;
            heimdall::ParserOptions options;
            std::shared_ptr<ParseSlot> slot;
            // Tree of an earlier version of this document and the edit that turns
            // its text into the one at `base_version`: the next parse copies the
            // top-level items outside that edit instead of re-parsing them.
            std::shared_ptr<const heimdall::ParseTree> base;
            heimdall::ParserOptions base_options;
            heimdall::Lexer::TextEdit base_edit;
            std::int64_t base_version = -1;
        };

        std::unordered_map<std::string, ParseCacheEntry> m_parse_cache;
        // Type names of the headers each open document includes, as last indexed (guarded
        // by m_parse_mu). The parser reads them without ever waiting for an index build; a
        // new set invalidates the cached parse, so the next request re-reads the file with it.
        std::unordered_map<std::string, std::shared_ptr<const heimdall::TypeNameOracle>> m_type_names;

        std::unordered_map<const heimdall::CompileCommand*,
            std::shared_ptr<const heimdall::Preprocessor::MacroMap>>
        m_macro_cache;

        std::atomic<bool> m_enable_semantic = false;
        std::atomic<bool> m_workspace_scan = true;
        std::filesystem::path m_workspace_root; // guarded by m_init_mu
        std::jthread m_scan_worker;
        std::string m_initialization_error;
        std::condition_variable_any m_index_cv;
        std::mutex m_index_mu;
        std::mutex m_diag_mu;
        std::condition_variable_any m_diag_cv;
        std::deque<DiagJob> m_diag_queue;
        bool m_diag_busy = false;
        std::string m_diag_running_uri;
        std::stop_source m_diag_stop;
        std::jthread m_index_worker;
        std::jthread m_diag_worker;
        // Shutdown drains document jobs, parse jobs/continuations, then handlers.
        ThreadPool m_pool;
        ThreadPool m_parse_pool;
        ThreadPool m_document_pool;
    };

} // namespace heimdall::lsp
