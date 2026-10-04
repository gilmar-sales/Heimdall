#pragma once

#include "Document.hpp"
#include "ThreadPool.hpp"

#include <Heimdall/CompileDatabase.hpp>
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
        // Immutable per-version snapshot: published by the I/O thread, consumed by
        // workers without locking or copying. The ParseTree borrows this buffer
        // through shared ownership (see CachedParse), so replacing the text never
        // invalidates a tree being analyzed concurrently.
        struct DocumentSnapshot
        {
            std::shared_ptr<const std::string> text = std::make_shared<const std::string>();
            std::int64_t version = 0;
            std::shared_ptr<const LineIndex> lines = std::make_shared<const LineIndex>();
        };

        // A document as it was when a request arrived.
        struct PinnedDocument
        {
            std::string uri;
            DocumentSnapshot snapshot;
        };

        // Per-request state, visible to the handler running on a pool thread
        // (and to the diagnostics worker, which only sets `stop`).
        struct RequestContext
        {
            std::stop_token stop;
            std::optional<PinnedDocument> pinned;
        };

        using RequestHandler = std::function<void(simdjson::dom::element, std::string_view)>;

        struct HeaderView
        {
            std::shared_ptr<const heimdall::IncludeIndex> index;
            bool complete = true;
        };

        struct IndexJob
        {
            std::string key;
            std::vector<std::filesystem::path> headers;
            const heimdall::CompileCommand * command = nullptr;
        };

        struct DiagJob
        {
            std::string uri;
            std::int64_t version = 0;
        };

        void LoadInitializationOptions(simdjson::dom::element request);
        void Respond(std::string_view id, std::string_view result);
        void PublishDiagnostics(const std::string & uri, std::shared_ptr<const std::string> text,
            std::int64_t version);
        bool DocumentParams(simdjson::dom::element request, std::string_view & uri,
            simdjson::dom::object & document);
        void OpenDocument(simdjson::dom::element request);
        void ChangeDocument(simdjson::dom::element request);
        void CloseDocument(simdjson::dom::element request);
        void FormatDocument(simdjson::dom::element request, std::string_view id);
        void RangeFormatDocument(simdjson::dom::element request, std::string_view id);
        void CodeActions(simdjson::dom::element request, std::string_view id);
        void CompleteDocument(simdjson::dom::element request, std::string_view id);
        void HoverDocument(simdjson::dom::element request, std::string_view id);
        void GotoDocument(simdjson::dom::element request, std::string_view id, bool implementation);
        // HeaderScopes, but waits (bounded, cancellable) for a background index
        // build that is still running: hover and go-to answer once, so they must
        // not report "nothing found" just because the first request beat the index.
        HeaderView AwaitHeaderScopes(const std::string & uri, const std::shared_ptr<const std::string> & text,
            const heimdall::CompileCommand * command);
        HeaderView HeaderScopes(const std::string & uri, const std::shared_ptr<const std::string> & text,
            const heimdall::CompileCommand * command);
        std::shared_ptr<const heimdall::ParseTree> CachedParse(const std::string & uri,
            const std::shared_ptr<const std::string> & text,
            std::int64_t version,
            const heimdall::CompileCommand * command);
        heimdall::ParserOptions ParserOptionsFor(const heimdall::CompileCommand * command);

        void IndexWorkerMain(std::stop_token stop);
        void DiagWorkerMain(std::stop_token stop);
        void EnqueueDiagnostics(const std::string & uri, std::int64_t version);
        void FlushDiagnostics();
        static bool ApplyContentChange(std::string & current, LineIndex &index,
            simdjson::dom::object change);
        void TouchGlobalIndex(const std::string & key);
        // Runs `handler` on the pool against a private copy of the message, so
        // the I/O thread goes straight back to reading (and to $/cancelRequest).
        void Dispatch(std::string_view body, simdjson::dom::element request, const std::string & id,
            RequestHandler handler);
        // Snapshot of `uri`: the one pinned at arrival for the current request,
        // otherwise the latest.
        std::optional<DocumentSnapshot> GetDocument(const std::string & uri);
        const heimdall::CompileCommand * CommandFor(const std::string & uri);
        static std::stop_token CurrentStop();
        static bool RequestCancelled();
        void RespondCancelled(std::string_view id);
        bool IsCurrentVersion(const std::string & uri, std::int64_t version);

        static thread_local const RequestContext * t_context;

        // m_docs_mu guards only m_documents (hot: every request and keystroke),
        // so readers never queue behind cache bookkeeping under m_mu.
        std::shared_mutex m_docs_mu;
        std::unordered_map<std::string, DocumentSnapshot> m_documents;

        std::mutex m_inflight_mu;
        std::unordered_map<std::string, std::stop_source> m_inflight;

        std::mutex m_mu;
        std::optional<heimdall::CompileDatabase> m_compile_database;
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
            std::once_flag once;
            std::shared_ptr<const heimdall::ParseTree> tree;
        };

        struct ParseCacheEntry
        {
            std::int64_t version = -1;
            std::shared_ptr<const std::string> text;
            heimdall::ParserOptions options;
            std::shared_ptr<ParseSlot> slot;
        };

        std::unordered_map<std::string, ParseCacheEntry> m_parse_cache;

        std::unordered_map<const heimdall::CompileCommand *,
            std::shared_ptr<const heimdall::Preprocessor::MacroMap>>
        m_macro_cache;

        std::atomic<bool> m_enable_semantic = false;
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
        // Last member: destroyed (and joined) first, while everything the
        // handlers touch is still alive.
        ThreadPool m_pool;
    };

} // namespace heimdall::lsp
