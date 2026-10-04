#pragma once

#include "Document.hpp"

#include <Heimdall/CompileDatabase.hpp>
#include <Heimdall/IncludeIndex.hpp>
#include <Heimdall/ParseTree.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <simdjson.h>
#include <string>
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
        bool WasCancelled(const std::string & id);
        bool IsCurrentVersion(const std::string & uri, std::int64_t version);

        std::mutex m_mu;
        std::unordered_map<std::string, DocumentSnapshot> m_documents;
        std::optional<heimdall::CompileDatabase> m_compile_database;

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

        std::unordered_set<std::string> m_cancelled;
        std::atomic<bool> m_enable_semantic = false;
        std::string m_initialization_error;
        std::condition_variable_any m_index_cv;
        std::mutex m_index_mu;
        std::mutex m_diag_mu;
        std::condition_variable_any m_diag_cv;
        std::deque<DiagJob> m_diag_queue;
        bool m_diag_busy = false;
        std::jthread m_index_worker;
        std::jthread m_diag_worker;
        std::vector<std::jthread> m_system_threads;
    };

} // namespace heimdall::lsp
