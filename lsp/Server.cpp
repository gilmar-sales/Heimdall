#include "Server.hpp"

#include "Document.hpp"
#include "JsonRpc.hpp"

#include <Heimdall/Formatter.hpp>
#include <Heimdall/ParseTree.hpp>
#include <Heimdall/RuleEngine.hpp>
#include <Heimdall/IncludeAnalyzer.hpp>
#include <Heimdall/SemanticAnalyzer.hpp>
#include <Heimdall/Completion.hpp>
#include <Heimdall/Navigation.hpp>
#include <Heimdall/RuleConfig.hpp>

#include <algorithm>
#include <chrono>
#include <shared_mutex>
#include <stop_token>
#include <fstream>
#include <filesystem>

namespace heimdall::lsp
{

    namespace
    {

        // Interactive handlers + two for background work (compiler probing).
        std::size_t PoolSize()
        {
            return std::clamp<std::size_t>(std::thread::hardware_concurrency(), 3, 8);
        }

    } // namespace

    thread_local const LanguageServer::RequestContext * LanguageServer::t_context = nullptr;

    LanguageServer::LanguageServer() : m_pool(PoolSize()) {}
    LanguageServer::~LanguageServer() = default;

    bool LanguageServer::Run()
    {
        m_index_worker = std::jthread([this](std::stop_token stop)
            {
                IndexWorkerMain(stop);
        });
        m_diag_worker = std::jthread([this](std::stop_token stop)
            {
                DiagWorkerMain(stop);
        });

        m_pool.Submit([]
            {
                heimdall::IncludeIndex::SystemIncludes("c++");
            }, ThreadPool::Priority::Background);

        auto stop_workers =[&]
        {
            {
                const std::lock_guard<std::mutex> lock(m_inflight_mu);
                for (auto &[key, source]: m_inflight)
                {
                    source.request_stop();
                }
            }
            m_pool.Shutdown();
            m_index_worker.request_stop();
            m_diag_worker.request_stop();
            m_index_cv.notify_all();
            m_diag_cv.notify_all();
        };

        std::string body;

        simdjson::dom::parser parser;
        while (ReadMessage(body))
        {
            simdjson::dom::element request;
            if (parser.parse(body).get(request))
            {
                continue;
            }

            simdjson::dom::object object;
            if (request.get_object().get(object))
            {
                continue;
            }

            std::string_view method;
            if (!GetString(object, "method", method))
            {
                continue;
            }

            simdjson::dom::element id;
            const bool has_id =!object["id"].get(id);
            std::string id_json = has_id ? simdjson::minify(id) : "null";

            if (method == "initialize")
            {
                LoadInitializationOptions(request);
                Respond(id_json,
                    "{\"capabilities\":{\"textDocumentSync\":2,\"documentFormattingProvider\":true,"
                    "\"documentRangeFormattingProvider\":true,"
                    "\"codeActionProvider\":true,\"hoverProvider\":true,"
                    "\"definitionProvider\":true,\"implementationProvider\":true,"
                    "\"completionProvider\":{\"triggerCharacters\":[\".\",\">\",\":\",\"#\"],"
                    "\"resolveProvider\":false}},"
                    "\"serverInfo\":{\"name\":\"Heimdall\",\"version\":\"0.1.0\"}}");
            }
            else if (method == "initialized") {}
            else if (method == "shutdown")
            {
                // Answer every request received so far before acknowledging.
                m_pool.WaitIdle();
                FlushDiagnostics();
                Respond(id_json, "null");
            }
            else if (method == "exit")
            {
                stop_workers();
                return true;
            }
            else if (method == "textDocument/didOpen")
            {
                OpenDocument(request);
            }
            else if (method == "textDocument/didChange")
            {
                ChangeDocument(request);
            }
            else if (method == "textDocument/didClose")
            {
                CloseDocument(request);
            }
            else if (method == "textDocument/formatting")
            {
                Dispatch(body, request, id_json,[this](simdjson::dom::element request, std::string_view id)
                    {
                        FormatDocument(request, id);
                });
            }
            else if (method == "textDocument/rangeFormatting")
            {
                Dispatch(body, request, id_json,[this](simdjson::dom::element request, std::string_view id)
                    {
                        RangeFormatDocument(request, id);
                });
            }
            else if (method == "textDocument/codeAction")
            {
                Dispatch(body, request, id_json,[this](simdjson::dom::element request, std::string_view id)
                    {
                        CodeActions(request, id);
                });
            }
            else if (method == "textDocument/completion")
            {
                Dispatch(body, request, id_json,[this](simdjson::dom::element request, std::string_view id)
                    {
                        CompleteDocument(request, id);
                });
            }
            else if (method == "textDocument/definition")
            {
                Dispatch(body, request, id_json,[this](simdjson::dom::element request, std::string_view id)
                    {
                        GotoDocument(request, id, false);
                });
            }
            else if (method == "textDocument/implementation")
            {
                Dispatch(body, request, id_json,[this](simdjson::dom::element request, std::string_view id)
                    {
                        GotoDocument(request, id, true);
                });
            }
            else if (method == "textDocument/hover")
            {
                Dispatch(body, request, id_json,[this](simdjson::dom::element request, std::string_view id)
                    {
                        HoverDocument(request, id);
                });
            }
            else if (method == "$/cancelRequest")
            {
                simdjson::dom::object params;
                if (GetObject(request, "params", params))
                {
                    simdjson::dom::element cancel_id;
                    if (!params["id"].get(cancel_id))
                    {
                        // The request is running (or queued) on the pool; the
                        // I/O thread is free, so the token flips right away.
                        const std::lock_guard<std::mutex> lock(m_inflight_mu);
                        if (const auto found = m_inflight.find(simdjson::minify(cancel_id));
                            found != m_inflight.end())
                        {
                            found->second.request_stop();
                        }
                    }
                }
            }
            else if (has_id)
            {
                Respond(id_json, "null");
            }
        }

        stop_workers();
        return false;
    }

    void LanguageServer::Dispatch(std::string_view body, simdjson::dom::element request,
        const std::string & id, RequestHandler handler)
    {
        auto context = std::make_shared<RequestContext>();
        std::stop_source source;
        context->stop = source.get_token();

        // Pin the document as of arrival: a didChange received after this
        // request must not change the text its position refers to.
        std::string_view uri;
        simdjson::dom::object text_document;
        if (DocumentParams(request, uri, text_document))
        {
            std::string uri_string(uri);
            if (auto snapshot = GetDocument(uri_string))
            {
                context->pinned = PinnedDocument{std::move(uri_string), std::move(*snapshot)};
            }
        }

        {
            const std::lock_guard<std::mutex> lock(m_inflight_mu);
            m_inflight[id] = source;
        }

        // `request` points into the I/O thread's parser buffer, which the next
        // message overwrites: the worker re-parses its own copy of the body.
        m_pool.Submit([this, body = std::string(body), id, context, handler = std::move(handler)]
            {
                struct Finish
                {
                    LanguageServer &server;
                    const std::string & id;
                    ~Finish()
                    {
                        t_context = nullptr;
                        const std::lock_guard<std::mutex> lock(server.m_inflight_mu);
                        server.m_inflight.erase(id);
                }
                } finish
                {
                    *this, id
            };

                simdjson::dom::parser parser;
                simdjson::dom::element element;
                if (parser.parse(body).get(element))
                {
                    Respond(id, "null");
                    return;
            }

                t_context = context.get();
                handler(element, id);
        });
    }

    std::optional<LanguageServer::DocumentSnapshot> LanguageServer::GetDocument(const std::string & uri)
    {
        if (t_context != nullptr && t_context->pinned && t_context->pinned->uri == uri)
        {
            return t_context->pinned->snapshot;
        }

        const std::shared_lock<std::shared_mutex> lock(m_docs_mu);
        if (const auto found = m_documents.find(uri); found != m_documents.end())
        {
            return found->second;
        }

        return std::nullopt;
    }

    const heimdall::CompileCommand * LanguageServer::CommandFor(const std::string & uri)
    {
        // m_compile_database is written once, by `initialize` on the I/O thread,
        // before any document exists; later readers are ordered after it by the
        // queues that hand them work.
        if (m_compile_database == std::nullopt)
        {
            return nullptr;
        }

        const std::filesystem::path path = PathFromUri(uri);
        const heimdall::CompileCommand * exact = m_compile_database->Find(path);
        return exact != nullptr ? exact : m_compile_database->FindOrNearest(path);
    }

    std::stop_token LanguageServer::CurrentStop()
    {
        return t_context != nullptr ? t_context->stop : std::stop_token {};
    }

    bool LanguageServer::RequestCancelled()
    {
        return t_context != nullptr && t_context->stop.stop_requested();
    }

    void LanguageServer::RespondCancelled(std::string_view id)
    {
        // LSP RequestCancelled.
        Send("{\"jsonrpc\":\"2.0\",\"id\":" + std::string(id) +
            ",\"error\":{\"code\":-32800,\"message\":\"Request cancelled\"}}");
    }

    void LanguageServer::LoadInitializationOptions(simdjson::dom::element request)
    {
        simdjson::dom::object params;
        simdjson::dom::object options;
        const bool has_params = GetObject(request, "params", params);

        std::filesystem::path workspace_root;
        if (has_params)
        {
            simdjson::dom::array folders;
            if (!params["workspaceFolders"].get_array().get(folders))
            {
                for (const auto folder_element: folders)
                {
                    simdjson::dom::object folder;
                    std::string_view uri;
                    if (!folder_element.get_object().get(folder) && GetString(folder, "uri", uri))
                    {
                        workspace_root = PathFromUri(uri);
                        break;
                    }
                }
            }

            if (workspace_root.empty())
            {
                std::string_view uri;
                std::string_view root_path;
                if (GetString(params, "rootUri", uri) && !uri.empty())
                {
                    workspace_root = PathFromUri(uri);
                }
                else if (GetString(params, "rootPath", root_path) && !root_path.empty())
                {
                    workspace_root = std::filesystem::path(root_path);
                }
            }
        }

        // A default-constructed simdjson object is not safe to index: only touch
        // `params` / `options` when they were actually present.
        const bool has_options = has_params && GetObject(params, "initializationOptions", options);
        if (has_options)
        {
            if (workspace_root.empty())
            {
                std::string_view configured_root;
                if (GetString(options, "workspaceRoot", configured_root) && !configured_root.empty())
                {
                    workspace_root = std::filesystem::path(configured_root);
                }
            }
        }

        std::error_code cwd_error;
        const auto cwd = std::filesystem::current_path(cwd_error);
        if (workspace_root.empty())
        {
            workspace_root = cwd_error ? std::filesystem::path{}: cwd;
        }

        if (!workspace_root.empty())
        {
            auto loaded_rules = heimdall::FindRuleOptions(workspace_root);
            if (!loaded_rules)
            {
                const std::lock_guard<std::mutex> lock(m_mu);
                m_initialization_error = loaded_rules.error();
            }
            else if (*loaded_rules)
            {
                m_rule_options = std::move(* *loaded_rules);
            }
            else if (!cwd_error && std::filesystem::absolute(workspace_root).lexically_normal() !=
                std::filesystem::absolute(cwd).lexically_normal())
            {
                auto cwd_rules = heimdall::FindRuleOptions(cwd);
                if (!cwd_rules)
                {
                    const std::lock_guard<std::mutex> lock(m_mu);
                    m_initialization_error = cwd_rules.error();
                }
                else if (*cwd_rules)
                {
                    m_rule_options = std::move(* *cwd_rules);
                }
            }
        }

        bool enabled = false;
        if (has_options && options["enableSemantic"].get_bool().get(enabled))
        {
            enabled = false;
        }

        m_enable_semantic.store(enabled, std::memory_order_relaxed);

        std::string_view path;
        if (!has_options ||!GetString(options, "compileCommands", path) || path.empty())
        {
            return;
        }

        auto database = heimdall::CompileDatabase::Load(std::filesystem::path(path));
        if (!database)
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            m_initialization_error = database.error();
            return;
        }

        m_compile_database = std::move(*database);

        std::unordered_set<std::string> drivers = {"c++"};
        for (const auto & command: m_compile_database->Commands())
        {
            if (!command.arguments.empty() && !command.arguments.front().empty())
            {
                drivers.insert(command.arguments.front());
            }
        }

        m_pool.Submit([drivers = std::move(drivers)]
            {
                for (const auto & driver: drivers)
                {
                    heimdall::IncludeIndex::SystemIncludes(driver);
            }
            }, ThreadPool::Priority::Background);
    }

    void LanguageServer::Respond(std::string_view id, std::string_view result)
    {
        Send("{\"jsonrpc\":\"2.0\",\"id\":" + std::string(id) + ",\"result\":" + std::string(result) + "}");
    }

    void LanguageServer::PublishDiagnostics(const std::string & uri,
        std::shared_ptr<const std::string> text,
        std::int64_t version)
    {
        // Single-pass pipeline: one lex + preprocess + grammar pass per version,
        // shared by the rule engine, the semantic pass and the syntax errors
        // (was: 4 lexes + 2 preprocesses of the same buffer per keystroke).
        const heimdall::CompileCommand * command = CommandFor(uri);
        const auto tree = CachedParse(uri, text, version, command);
        if (!tree || RequestCancelled() ||!IsCurrentVersion(uri, version))
        {
            return;
        }

        const auto diagnostics = RuleDiagnostics(uri, *tree, command);
        if (!IsCurrentVersion(uri, version))
        {
            return;
        }

        std::vector<heimdall::SemanticDiagnostic> semantic_diagnostics;
        if (m_enable_semantic.load(std::memory_order_relaxed) && command != nullptr)
        {
            semantic_diagnostics = heimdall::SemanticAnalyzer().AnalyzeUnusedLocals(*tree, command);
            if (!IsCurrentVersion(uri, version))
            {
                return;
            }
        }

        // Names that a using-directive makes ambiguous (needs header scopes so
        // `using namespace std;` clashes with other namespaces are seen too).
        const HeaderView ambiguity_headers = HeaderScopes(uri, text, command);
        const auto ambiguities = heimdall::Navigation::FindAmbiguities(
            *tree, ambiguity_headers.index ? &ambiguity_headers.index->Scopes() : nullptr);
        if (!IsCurrentVersion(uri, version))
        {
            return;
        }

        // One line-index build per publish; every diagnostic position below is
        // O(log L) (was: an O(file size) scan per diagnostic).
        LineIndex lines;
        lines.Build(*text);
        std::string message = "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":";
        QuoteJson(uri, message);
        message += ",\"version\":" + std::to_string(version) + ",\"diagnostics\":[";
        bool first = true;
        for (const auto & diagnostic: diagnostics)
        {
            if (!first)
            {
                message += ',';
            }

            first = false;
            const Position start = lines.ToPosition(diagnostic.offset);
            const Position end = lines.ToPosition(diagnostic.offset + diagnostic.length);
            message += "{\"range\":{\"start\":";
            AppendPosition(start, message);
            message += ",\"end\":";
            AppendPosition(end, message);
            message += "},\"severity\":2,\"code\":";
            QuoteJson(diagnostic.code, message);
            message += ",\"source\":\"heimdall\",\"message\":";
            QuoteJson(diagnostic.message, message);
            message += '}';
        }

        for (const auto & diagnostic: semantic_diagnostics)
        {
            if (!first)
            {
                message += ',';
            }

            first = false;
            const Position start = lines.ToPosition(diagnostic.offset);
            const Position end = lines.ToPosition(diagnostic.offset + diagnostic.length);
            message += "{\"range\":{\"start\":";
            AppendPosition(start, message);
            message += ",\"end\":";
            AppendPosition(end, message);
            message += "},\"severity\":2,\"code\":";
            QuoteJson(diagnostic.code, message);
            message += ",\"source\":\"heimdall\",\"message\":";
            QuoteJson(diagnostic.message, message);
            message += '}';
        }

        for (const auto & ambiguity: ambiguities)
        {
            if (!first)
            {
                message += ',';
            }

            first = false;
            std::string text_message = "reference to '" + ambiguity.name + "' is ambiguous; candidates: ";
            for (std::size_t i = 0; i < ambiguity.candidates.size(); ++i)
            {
                text_message +=(i == 0 ? "" : ", ") + ambiguity.candidates[i];
            }

            message += "{\"range\":{\"start\":";
            AppendPosition(lines.ToPosition(ambiguity.offset), message);
            message += ",\"end\":";
            AppendPosition(lines.ToPosition(ambiguity.offset + ambiguity.length), message);
            message += "},\"severity\":1,\"code\":\"semantic/ambiguous-reference\",\"source\":\"heimdall\",\"message\":";
            QuoteJson(text_message, message);
            message += '}';
        }

        for (const auto & diagnostic: tree->Diagnostics())
        {
            if (!first)
            {
                message += ',';
            }

            first = false;
            const Position start = lines.ToPosition(diagnostic.offset);
            message += "{\"range\":{\"start\":";
            AppendPosition(start, message);
            message += ",\"end\":";
            AppendPosition(start, message);
            message += "},\"severity\":1,\"code\":\"syntax/parse-error\",\"source\":\"heimdall\",\"message\":";
            QuoteJson(diagnostic.message, message);
            message += '}';
        }

        message += "]}}";
        std::string init_error;
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            init_error = m_initialization_error;
            m_initialization_error.clear();
        }
        if (!init_error.empty())
        {
            std::string notification = "{\"jsonrpc\":\"2.0\",\"method\":\"window/showMessage\",\"params\":{\"type\":2,\"message\":";
            QuoteJson(init_error, notification);
            notification += "}}";
            Send(notification);
        }

        Send(message);
    }

    bool LanguageServer::DocumentParams(simdjson::dom::element request, std::string_view & uri,
        simdjson::dom::object & document)
    {
        simdjson::dom::object params;
        if (!GetObject(request, "params", params))
        {
            return false;
        }

        if (!GetObject(params, "textDocument", document))
        {
            return false;
        }

        return GetString(document, "uri", uri);
    }

    void LanguageServer::OpenDocument(simdjson::dom::element request)
    {
        std::string_view uri;
        simdjson::dom::object text_document;
        if (!DocumentParams(request, uri, text_document))
        {
            return;
        }

        std::string_view text;
        if (!GetString(text_document, "text", text))
        {
            return;
        }

        std::int64_t version = 0;
        if (const auto error = text_document["version"].get_int64().get(version); error)
        {
            version = 0;
        }

        const std::string uri_string(uri);
        DocumentSnapshot snapshot;
        snapshot.text = std::make_shared<const std::string>(text);
        snapshot.version = version;
        auto lines = std::make_shared<LineIndex>();
        lines->Build(*snapshot.text);
        snapshot.lines = std::move(lines);
        {
            const std::lock_guard<std::shared_mutex> lock(m_docs_mu);
            m_documents[uri_string] = std::move(snapshot);
        }
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            m_parse_cache.erase(uri_string);
        }
        EnqueueDiagnostics(uri_string, version);
    }

    bool LanguageServer::ApplyContentChange(std::string & current, LineIndex &index,
        simdjson::dom::object change)
    {
        std::string_view text;
        if (!GetString(change, "text", text))
        {
            return false;
        }

        simdjson::dom::object range;
        if (!GetObject(change, "range", range))
        {
            // Full-document sync (textDocumentSync = 1 fallback).
            current.assign(text);
            index.Build(current);
            return true;
        }

        // Incremental sync (textDocumentSync = 2): splice the range.
        simdjson::dom::object start_object;
        simdjson::dom::object end_object;
        if (!GetObject(range, "start", start_object) ||!GetObject(range, "end", end_object))
        {
            return false;
        }

        auto number =[](simdjson::dom::object object, const char *key)->std::size_t
        {
            std::uint64_t unsigned_value = 0;
            if (!object[key].get_uint64().get(unsigned_value))
            {
                return static_cast<std::size_t>(unsigned_value);
            }

            std::int64_t signed_value = 0;
            if (!object[key].get_int64().get(signed_value) && signed_value > 0)
            {
                return static_cast<std::size_t>(signed_value);
            }

            return 0;
        };
        const Position start = {number(start_object, "line"), number(start_object, "character")};
        const Position end = {number(end_object, "line"), number(end_object, "character")};
        // F3: the caller keeps one index across the batch (was: a fresh O(N)
        // build per change). Offsets resolve against the running buffer; the
        // index is refreshed once per applied change below.
        std::size_t start_offset = index.OffsetFromPosition(start);
        std::size_t end_offset = index.OffsetFromPosition(end);
        if (start_offset > current.size())
        {
            start_offset = current.size();
        }

        if (end_offset > current.size())
        {
            end_offset = current.size();
        }

        if (end_offset < start_offset)
        {
            end_offset = start_offset;
        }

        current.replace(start_offset, end_offset - start_offset, text);
        index.Build(current);
        return true;
    }

    void LanguageServer::ChangeDocument(simdjson::dom::element request)
    {
        std::string_view uri;
        simdjson::dom::object text_document;
        if (!DocumentParams(request, uri, text_document))
        {
            return;
        }

        const std::string uri_string(uri);
        simdjson::dom::object params;
        if (!GetObject(request, "params", params))
        {
            return;
        }

        simdjson::dom::array changes;
        if (params["contentChanges"].get_array().get(changes))
        {
            return;
        }

        std::int64_t new_version = 0;
        if (const auto error = text_document["version"].get_int64().get(new_version); error)
        {
            new_version = 0;
        }

        std::shared_ptr<const std::string> base_text;
        {
            const std::shared_lock<std::shared_mutex> lock(m_docs_mu);
            const auto found = m_documents.find(uri_string);
            if (found == m_documents.end())
            {
                return;
            }

            base_text = found->second.text;
        }
        std::string current(*base_text);
        LineIndex batch_index;
        batch_index.Build(current);
        for (simdjson::dom::element change: changes)
        {
            simdjson::dom::object change_object;
            if (change.get_object().get(change_object))
            {
                continue;
            }

            ApplyContentChange(current, batch_index, change_object);
        }

        auto new_text = std::make_shared<const std::string>(std::move(current));
        auto new_lines = std::make_shared<const LineIndex>(std::move(batch_index));
        std::int64_t version = new_version;
        {
            const std::lock_guard<std::shared_mutex> lock(m_docs_mu);
            const auto found = m_documents.find(uri_string);
            if (found == m_documents.end())
            {
                return;
            }

            found->second.version = new_version;
            found->second.text = std::move(new_text);
            found->second.lines = std::move(new_lines);
            version = found->second.version;
        }
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            m_parse_cache.erase(uri_string);
        }
        EnqueueDiagnostics(uri_string, version);
    }

    void LanguageServer::CloseDocument(simdjson::dom::element request)
    {
        std::string_view uri;
        simdjson::dom::object text_document;
        if (!DocumentParams(request, uri, text_document))
        {
            return;
        }

        {
            const std::lock_guard<std::shared_mutex> lock(m_docs_mu);
            m_documents.erase(std::string(uri));
        }
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            m_include_cache.erase(std::string(uri));
            m_include_profiles.erase(std::string(uri));
            m_parse_cache.erase(std::string(uri));
        }
        {
            // Nothing to publish for a closed document: abandon a running pass.
            const std::lock_guard<std::mutex> lock(m_diag_mu);
            if (m_diag_running_uri == uri)
            {
                m_diag_stop.request_stop();
            }
        }
        std::string message = "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":";
        QuoteJson(uri, message);
        message += ",\"diagnostics\":[]}}";
        Send(message);
    }

    void LanguageServer::FormatDocument(simdjson::dom::element request, std::string_view id)
    {
        std::string_view uri;
        simdjson::dom::object text_document;
        if (!DocumentParams(request, uri, text_document))
        {
            Respond(id, "[]");
            return;
        }

        const std::string uri_string(uri);
        const auto document = GetDocument(uri_string);
        if (!document)
        {
            Respond(id, "[]");
            return;
        }

        const std::shared_ptr<const std::string> text = document->text;
        const std::int64_t version = document->version;
        const std::shared_ptr<const LineIndex> lines = document->lines;
        const heimdall::CompileCommand * command = CommandFor(uri_string);
        const auto tree = CachedParse(uri_string, text, version, command);
        if (!tree)
        {
            RespondCancelled(id);
            return;
        }

        const std::string formatted = heimdall::Formatter().Format(*tree);
        if (formatted == *text)
        {
            Respond(id, "[]");
            return;
        }

        std::string response = "[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":";
        AppendPosition(lines->ToPosition(text->size()), response);
        response += "},\"newText\":";
        QuoteJson(formatted, response);
        response += "}]";
        Respond(id, response);
    }

    std::vector<heimdall::Diagnostic> LanguageServer::RuleDiagnostics(const std::string & uri,
        const heimdall::ParseTree & tree, const heimdall::CompileCommand * command)
    {
        const heimdall::RuleEngine engine(m_rule_options);
        auto diagnostics = engine.Analyze(tree);
        if (!m_enable_semantic.load(std::memory_order_relaxed) || command == nullptr)
        {
            return diagnostics;
        }

        const std::filesystem::path file_path = PathFromUri(uri);
        const std::string fingerprint = heimdall::IncludeAnalyzer::Fingerprint(file_path, tree, command);
        std::shared_ptr<const heimdall::IncludeProfile> profile;
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            if (const auto found = m_include_profiles.find(uri); found != m_include_profiles.end())
            {
                profile = found->second;
            }
        }

        // The profile reads every transitive header: keep it across keystrokes.
        if (!profile || profile->fingerprint != fingerprint || !heimdall::IncludeAnalyzer::IsFresh(*profile))
        {
            profile = heimdall::IncludeAnalyzer::BuildProfile(file_path, tree, command);
            const std::lock_guard<std::mutex> lock(m_mu);
            m_include_profiles[uri] = profile;
        }

        auto unused = engine.ApplyPolicy(heimdall::IncludeAnalyzer::Analyze(tree, *profile), tree);
        if (!unused.empty())
        {
            diagnostics.insert(diagnostics.end(), std::make_move_iterator(unused.begin()),
                std::make_move_iterator(unused.end()));
            std::stable_sort(diagnostics.begin(), diagnostics.end(),
                [](const heimdall::Diagnostic &a, const heimdall::Diagnostic &b) { return a.offset < b.offset; });
        }

        return diagnostics;
    }

    void LanguageServer::CodeActions(simdjson::dom::element request, std::string_view id)
    {
        std::string_view uri;
        simdjson::dom::object text_document;
        if (!DocumentParams(request, uri, text_document))
        {
            Respond(id, "[]");
            return;
        }

        const std::string uri_string(uri);
        const auto document = GetDocument(uri_string);
        if (!document)
        {
            Respond(id, "[]");
            return;
        }

        const std::shared_ptr<const std::string> text = document->text;
        const std::int64_t version = document->version;
        const std::shared_ptr<const LineIndex> lines = document->lines;
        const heimdall::CompileCommand * command = CommandFor(uri_string);
        const auto tree = CachedParse(uri_string, text, version, command);
        if (!tree)
        {
            RespondCancelled(id);
            return;
        }

        const auto diagnostics = RuleDiagnostics(uri_string, *tree, command);
        std::string response = "[";
        bool first = true;
        for (const auto & diagnostic: diagnostics)
        {
            if (!diagnostic.has_fix)
            {
                continue;
            }

            if (!first)
            {
                response += ',';
            }

            first = false;
            response += "{\"title\":";
            QuoteJson(diagnostic.fix_title.empty() ? "Fix " + diagnostic.code + ": " + diagnostic.message :
                diagnostic.fix_title, response);
            response += ",\"kind\":\"quickfix\"";
            if (diagnostic.fix_is_safe)
            {
                response += ",\"isPreferred\":true";
            }

            response += ",\"edit\":{\"changes\":{";
            QuoteJson(uri_string, response);
            response += ":[{\"range\":{\"start\":";
            AppendPosition(lines->ToPosition(diagnostic.fix.offset), response);
            response += ",\"end\":";
            AppendPosition(lines->ToPosition(diagnostic.fix.offset + diagnostic.fix.length), response);
            response += "},\"newText\":";
            QuoteJson(diagnostic.fix.replacement, response);
            response += "}]}}}";
        }

        response += ']';
        Respond(id, response);
    }

    namespace
    {

        int ToLspKind(heimdall::CompletionKind kind)
        {
            switch (kind)
            {
            case heimdall::CompletionKind::Function:
                return 3;
            case heimdall::CompletionKind::Variable:
                return 6;
            case heimdall::CompletionKind::Type:
                return 7;
            case heimdall::CompletionKind::Namespace:
                return 9;
            case heimdall::CompletionKind::Macro:
                return 21;
            case heimdall::CompletionKind::Directive:
            case heimdall::CompletionKind::Keyword:
                return 14;
            }

            return 14;
        }

        std::uint64_t PositionNumber(simdjson::dom::object position, const char *key)
        {
            std::uint64_t unsigned_value = 0;
            if (!position[key].get_uint64().get(unsigned_value))
            {
                return unsigned_value;
            }

            std::int64_t signed_value = 0;
            if (!position[key].get_int64().get(signed_value) && signed_value > 0)
            {
                return static_cast<std::uint64_t>(signed_value);
            }

            return 0;
        }

    } // namespace

    void LanguageServer::CompleteDocument(simdjson::dom::element request, std::string_view id)
    {
        if (RequestCancelled())
        {
            RespondCancelled(id);
            return;
        }

        simdjson::dom::object params;
        if (!GetObject(request, "params", params))
        {
            Respond(id, "{\"isIncomplete\":false,\"items\":[]}");
            return;
        }

        simdjson::dom::object text_document;
        if (!GetObject(params, "textDocument", text_document))
        {
            Respond(id, "{\"isIncomplete\":false,\"items\":[]}");
            return;
        }

        std::string_view uri;
        if (!GetString(text_document, "uri", uri))
        {
            Respond(id, "{\"isIncomplete\":false,\"items\":[]}");
            return;
        }

        const std::string uri_string(uri);
        const auto document = GetDocument(uri_string);
        if (!document)
        {
            Respond(id, "{\"isIncomplete\":false,\"items\":[]}");
            return;
        }

        const std::shared_ptr<const std::string> text = document->text;
        const std::int64_t version = document->version;
        const std::shared_ptr<const LineIndex> lines = document->lines;
        simdjson::dom::object position;
        if (!GetObject(params, "position", position))
        {
            Respond(id, "{\"isIncomplete\":false,\"items\":[]}");
            return;
        }

        const Position cursor = {static_cast<std::size_t>(PositionNumber(position, "line")),
            static_cast<std::size_t>(PositionNumber(position, "character"))};
        const std::size_t offset = lines->OffsetFromPosition(cursor);

        const heimdall::CompileCommand * command = CommandFor(uri_string);
        const heimdall::ParserOptions parser_options = ParserOptionsFor(command);
        const HeaderView headers = HeaderScopes(uri_string, text, command);
        const auto tree = CachedParse(uri_string, text, version, command);
        if (!tree)
        {
            RespondCancelled(id);
            return;
        }

        if (RequestCancelled())
        {
            RespondCancelled(id);
            return;
        }

        const auto items = heimdall::CompletionEngine::Complete(
            *tree, parser_options, offset, headers.index ? &headers.index->Scopes() : nullptr);
        const std::string prefix = heimdall::CompletionEngine::PrefixAt(*text, offset);
        const Position start = lines->ToPosition(offset - prefix.size());
        const Position end = lines->ToPosition(offset);

        std::string response = headers.complete ? "{\"isIncomplete\":false,\"items\":["
        : "{\"isIncomplete\":true,\"items\":[";
        bool first = true;
        for (const auto & item: items)
        {
            if (!first)
            {
                response += ',';
            }

            first = false;
            response += "{\"label\":";
            QuoteJson(item.label, response);
            response += ",\"kind\":" + std::to_string(ToLspKind(item.kind)) + ",\"detail\":";
            QuoteJson(item.detail, response);
            if (!item.documentation.empty())
            {
                response += ",\"documentation\":{\"kind\":\"markdown\",\"value\":";
                QuoteJson(item.documentation, response);
                response += '}';
            }

            response += ",\"textEdit\":{\"range\":{\"start\":";
            AppendPosition(start, response);
            response += ",\"end\":";
            AppendPosition(end, response);
            response += "},\"newText\":";
            QuoteJson(item.label, response);
            response += "}}";
        }

        response += "]}";
        Respond(id, response);
    }

    void LanguageServer::TouchGlobalIndex(const std::string & key)
    {
        auto found = m_global_indices.find(key);
        if (found == m_global_indices.end())
        {
            return;
        }

        m_lru.erase(found->second.lru);
        m_lru.push_front(key);
        found->second.lru = m_lru.begin();
    }

    LanguageServer::HeaderView LanguageServer::AwaitHeaderScopes(const std::string & uri,
        const std::shared_ptr<const std::string> & text,
        const heimdall::CompileCommand * command)
    {
        using namespace std::chrono_literals;
        HeaderView view = HeaderScopes(uri, text, command);
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!view.complete && !RequestCancelled() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(25ms);
            view = HeaderScopes(uri, text, command);
        }

        return view;
    }

    LanguageServer::HeaderView LanguageServer::HeaderScopes(const std::string & uri,
        const std::shared_ptr<const std::string> & text,
        const heimdall::CompileCommand * command)
    {
        // Two-level header cache. The fingerprint covers only the file's own
        // `#include` block plus search flags: repeat keystrokes hit it with zero
        // disk I/O (previously every completion/hover re-lexed every transitive
        // header from disk just to compute the cache key). A fingerprint miss
        // re-resolves, then re-stats the resolved set; only a real change rebuilds.
        // Built indexes are shared globally by content key, so the same <vector>
        // is parsed once across all open documents (was: once per document).
        // Builds run on a background worker: a miss answers immediately with the
        // last good index (or an empty one) and `isIncomplete=true`; the client
        // re-requests once indexing lands.
        const std::filesystem::path file_path = PathFromUri(uri);
        const std::filesystem::path base_dir =
            file_path.has_parent_path() ? file_path.parent_path() : std::filesystem::path();
        const std::string fingerprint = heimdall::IncludeIndex::IncludeFingerprint(base_dir, *text,
            command);

        std::vector<std::filesystem::path> headers;
        std::string requested_key;
        std::string served_key;
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            auto &entry = m_include_cache[uri];
            if (entry.fingerprint != fingerprint)
            {
                entry.fingerprint.clear(); // mark resolving; restored below
                headers.clear();
                requested_key.clear();
                served_key = entry.served_key;
                // Resolve under the caller's eye: do the disk I/O outside m_mu.
            }
            else
            {
                headers = entry.headers;
                requested_key = entry.requested_key;
                served_key = entry.served_key;
            }

            if (!entry.fingerprint.empty())
            {
                // Fast path: fingerprint hit.
                if (!requested_key.empty())
                {
                    if (const auto global = m_global_indices.find(requested_key);
                        global != m_global_indices.end())
                    {
                        TouchGlobalIndex(requested_key);
                        return {global->second.index, true};
                    }

                    if (m_index_pending.contains(requested_key))
                    {
                        if (!served_key.empty())
                        {
                            if (const auto stale = m_global_indices.find(served_key);
                                stale != m_global_indices.end())
                            {
                                TouchGlobalIndex(served_key);
                                return {stale->second.index, false};
                            }
                        }

                        return {nullptr, false};
                    }

                    // Requested but neither built nor pending (evicted): rebuild.
                    m_index_pending.insert(requested_key);
                    {
                        const std::lock_guard<std::mutex> queue_lock(m_index_mu);
                        m_index_queue.push_back({requested_key, headers, command});
                    }
                    m_index_cv.notify_one();
                }

                if (!served_key.empty())
                {
                    if (const auto stale = m_global_indices.find(served_key);
                        stale != m_global_indices.end())
                    {
                        TouchGlobalIndex(served_key);
                        return {stale->second.index, false};
                    }
                }

                return {nullptr, requested_key.empty()};
            }
        }
        // Slow path: fingerprint changed. Resolve + stat outside the lock.
        headers = heimdall::IncludeIndex::ResolveHeaders(base_dir, *text, command);
        if (headers.empty())
        {
            // No headers: nothing to build, trivially complete (and shared, since
            // the key would be identical for every header-less file).
            const std::lock_guard<std::mutex> lock(m_mu);
            auto &entry = m_include_cache[uri];
            entry.fingerprint = fingerprint;
            entry.headers = headers;
            entry.requested_key.clear();
            entry.served_key.clear();
            return {nullptr, true};
        }

        requested_key = heimdall::IncludeIndex::CacheKey(headers, command);
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            auto &entry = m_include_cache[uri];
            entry.fingerprint = fingerprint;
            entry.headers = headers;
            entry.requested_key = requested_key;
            if (!requested_key.empty())
            {
                if (const auto global = m_global_indices.find(requested_key);
                    global != m_global_indices.end())
                {
                    entry.served_key = requested_key;
                    TouchGlobalIndex(requested_key);
                    return {global->second.index, true};
                }

                if (!m_index_pending.contains(requested_key))
                {
                    m_index_pending.insert(requested_key);
                    {
                        const std::lock_guard<std::mutex> queue_lock(m_index_mu);
                        m_index_queue.push_back({requested_key, headers, command});
                    }
                    m_index_cv.notify_one();
                }
            }
            else
            {
                entry.served_key.clear();
                return {nullptr, true};
            }

            served_key = entry.served_key;
            if (!served_key.empty())
            {
                if (const auto stale = m_global_indices.find(served_key); stale != m_global_indices.end())
                {
                    TouchGlobalIndex(served_key);
                    return {stale->second.index, false};
                }
            }

            return {nullptr, false};
        }
    }

    heimdall::ParserOptions LanguageServer::ParserOptionsFor(const heimdall::CompileCommand * command)
    {
        heimdall::ParserOptions options;
        if (command == nullptr)
        {
            return options;
        }

        options.standard = command->standard;
        const std::lock_guard<std::mutex> lock(m_mu);
        if (const auto found = m_macro_cache.find(command); found != m_macro_cache.end())
        {
            options.shared_macros = found->second;
            return options;
        }

        auto macros = std::make_shared<heimdall::Preprocessor::MacroMap>(command->defines);
        for (const auto & name: command->undefines)
        {
            macros->erase(name);
        }

        m_macro_cache.emplace(command, macros);
        options.shared_macros = std::move(macros);
        return options;
    }

    std::shared_ptr<const heimdall::ParseTree> LanguageServer::CachedParse(
        const std::string & uri, const std::shared_ptr<const std::string> & text, std::int64_t version,
        const heimdall::CompileCommand * command)
    {
        heimdall::ParserOptions options = ParserOptionsFor(command);
        std::shared_ptr<ParseSlot> slot;
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            if (const auto found = m_parse_cache.find(uri);
                found != m_parse_cache.end() && found->second.version == version &&
                found->second.text.get() == text.get() && found->second.slot)
            {
                slot = found->second.slot;
            }
            else
            {
                slot = std::make_shared<ParseSlot>();
                auto &entry = m_parse_cache[uri];
                entry.version = version;
                entry.text = text;
                entry.options = options;
                entry.slot = slot;
            }
        }
        // Always go through call_once: its return synchronizes with the thread
        // that filled the slot. (Peeking at slot->tree first was a data race
        // with the worker still inside the initializer.) A cancelled parse
        // throws out of the initializer, which leaves the flag unset so the next
        // caller retries with its own token instead of inheriting a partial tree.
        struct ParseCancelled {};

        try
        {
            const std::stop_token stop = CurrentStop();
            std::call_once(slot->once,[&]
                {
                    auto tree = std::make_shared<heimdall::ParseTree>(
                    heimdall::ParseTree::Parse(*text, options, stop));
                    if (tree->Cancelled())
                    {
                        throw ParseCancelled{};
                }

                    tree->HoldSource(text);
                    slot->tree = std::move(tree);
            });
        }
        catch (const ParseCancelled &)
        {
            return nullptr;
        }

        return slot->tree;
    }

    bool LanguageServer::IsCurrentVersion(const std::string & uri, std::int64_t version)
    {
        const std::shared_lock<std::shared_mutex> lock(m_docs_mu);
        const auto found = m_documents.find(uri);
        return found != m_documents.end() && found->second.version == version;
    }

    void LanguageServer::HoverDocument(simdjson::dom::element request, std::string_view id)
    {
        simdjson::dom::object params;
        if (!GetObject(request, "params", params))
        {
            Respond(id, "null");
            return;
        }

        simdjson::dom::object text_document;
        if (!GetObject(params, "textDocument", text_document))
        {
            Respond(id, "null");
            return;
        }

        std::string_view uri;
        if (!GetString(text_document, "uri", uri))
        {
            Respond(id, "null");
            return;
        }

        const std::string uri_string(uri);
        const auto document = GetDocument(uri_string);
        if (!document)
        {
            Respond(id, "null");
            return;
        }

        const std::shared_ptr<const std::string> text = document->text;
        const std::int64_t version = document->version;
        const std::shared_ptr<const LineIndex> lines = document->lines;
        simdjson::dom::object position;
        if (!GetObject(params, "position", position))
        {
            Respond(id, "null");
            return;
        }

        const Position cursor = {static_cast<std::size_t>(PositionNumber(position, "line")),
            static_cast<std::size_t>(PositionNumber(position, "character"))};
        const std::size_t offset = lines->OffsetFromPosition(cursor);

        const heimdall::CompileCommand * command = CommandFor(uri_string);
        const heimdall::ParserOptions parser_options = ParserOptionsFor(command);
        const HeaderView headers = AwaitHeaderScopes(uri_string, text, command);
        const auto tree = CachedParse(uri_string, text, version, command);
        if (!tree)
        {
            RespondCancelled(id);
            return;
        }

        if (RequestCancelled())
        {
            RespondCancelled(id);
            return;
        }

        const auto hovered = heimdall::CompletionEngine::Hover(
            *tree, parser_options, offset, headers.index ? &headers.index->Scopes() : nullptr);
        if (!hovered.has_value())
        {
            Respond(id, "null");
            return;
        }

        std::string line = hovered->label;
        if (!hovered->detail.empty() && hovered->detail != hovered->label)
        {
            line = hovered->detail.find(hovered->label) == std::string::npos
            ? hovered->label + ": " + hovered->detail
            : hovered->detail;
        }

        std::string value = "```cpp\n" + line + "\n```";
        if (!hovered->documentation.empty())
        {
            value += "\n\n" + hovered->documentation;
        }

        std::string response = "{\"contents\":{\"kind\":\"markdown\",\"value\":";
        QuoteJson(value, response);
        response += "}}";
        Respond(id, response);
    }

    void LanguageServer::RangeFormatDocument(simdjson::dom::element request, std::string_view id)
    {
        std::string_view uri;
        simdjson::dom::object text_document;
        if (!DocumentParams(request, uri, text_document))
        {
            Respond(id, "[]");
            return;
        }

        const std::string uri_string(uri);
        simdjson::dom::object params;
        if (!GetObject(request, "params", params))
        {
            Respond(id, "[]");
            return;
        }

        simdjson::dom::object range;
        if (!GetObject(params, "range", range))
        {
            Respond(id, "[]");
            return;
        }

        simdjson::dom::object start_object;
        simdjson::dom::object end_object;
        if (!GetObject(range, "start", start_object) ||!GetObject(range, "end", end_object))
        {
            Respond(id, "[]");
            return;
        }

        std::size_t start_line = static_cast<std::size_t>(PositionNumber(start_object, "line"));
        std::size_t end_line = static_cast<std::size_t>(PositionNumber(end_object, "line"));
        if (PositionNumber(end_object, "character") == 0 && end_line > start_line)
        {
            --end_line;
        }

        const auto document = GetDocument(uri_string);
        if (!document)
        {
            Respond(id, "[]");
            return;
        }

        const std::shared_ptr<const std::string> text = document->text;
        const std::shared_ptr<const LineIndex> lines = document->lines;
        // Diff the fully formatted buffer, then keep only hunks overlapping
        // the requested lines: context outside the range still informs the
        // formatting (brace depth, continuation) but is never rewritten.
        const auto edits = heimdall::Formatter().FormatEdits(*text);
        std::string response = "[";
        bool first = true;
        for (const auto & edit: edits)
        {
            if (edit.start_line > end_line || edit.end_line <= start_line)
            {
                continue;
            }

            if (RequestCancelled())
            {
                RespondCancelled(id);
                return;
            }

            if (!first)
            {
                response += ',';
            }

            first = false;
            response += "{\"range\":{\"start\":";
            AppendPosition(lines->ToPosition(edit.start_offset), response);
            response += ",\"end\":";
            AppendPosition(lines->ToPosition(edit.end_offset), response);
            response += "},\"newText\":";
            QuoteJson(edit.replacement, response);
            response += '}';
        }

        response += ']';
        Respond(id, response);
    }

    void LanguageServer::IndexWorkerMain(std::stop_token stop)
    {
        while (!stop.stop_requested())
        {
            IndexJob job;
            {
                std::unique_lock<std::mutex> lock(m_index_mu);
                m_index_cv.wait(lock, stop,[&]
                    {
                        return!m_index_queue.empty();
                });
                if (stop.stop_requested())
                {
                    return;
                }

                job = std::move(m_index_queue.front());
                m_index_queue.pop_front();
            }
            auto built =
                std::make_shared<const heimdall::IncludeIndex>(heimdall::IncludeIndex::Build(job.headers,
                job.command));
            {
                const std::lock_guard<std::mutex> lock(m_mu);
                m_index_pending.erase(job.key);
                // LRU eviction: the global index used to grow without bounds.
                if (!m_global_indices.contains(job.key) && m_global_indices.size() >= kMaxGlobalIndices &&
                    !m_lru.empty())
                {
                    m_global_indices.erase(m_lru.back());
                    m_lru.pop_back();
                }

                m_lru.remove(job.key);
                m_lru.push_front(job.key);
                m_global_indices[job.key] = {std::move(built), m_lru.begin()};
                for (auto &[uri, entry]: m_include_cache)
                {
                    if (entry.requested_key == job.key)
                    {
                        entry.served_key = job.key;
                    }
                }
            }
        }
    }

    void LanguageServer::EnqueueDiagnostics(const std::string & uri, std::int64_t version)
    {
        {
            const std::lock_guard<std::mutex> lock(m_diag_mu);
            m_diag_queue.push_back({uri, version});
            // The running pass (if for this document) is now obsolete.
            if (m_diag_running_uri == uri)
            {
                m_diag_stop.request_stop();
            }
        }
        m_diag_cv.notify_one();
    }

    void LanguageServer::FlushDiagnostics()
    {
        std::unique_lock<std::mutex> lock(m_diag_mu);
        m_diag_cv.wait(lock,[&]
            {
                return m_diag_queue.empty() && !m_diag_busy;
        });
    }

    void LanguageServer::DiagWorkerMain(std::stop_token stop)
    {
        using namespace std::chrono_literals;
        while (!stop.stop_requested())
        {
            DiagJob job;
            bool stale = false;
            {
                std::unique_lock<std::mutex> lock(m_diag_mu);
                m_diag_cv.wait(lock, stop,[&]
                    {
                        return!m_diag_queue.empty();
                });
                if (stop.stop_requested())
                {
                    return;
                }

                job = m_diag_queue.front();
                m_diag_queue.pop_front();
                // Coalesce bursts: a newer queued job for the same uri makes
                // this one stale before it even starts.
                for (const auto & queued: m_diag_queue)
                {
                    if (queued.uri == job.uri)
                    {
                        stale = true;
                        break;
                    }
                }

                if (!stale)
                {
                    m_diag_busy = true;
                }
                else
                {
                    m_diag_cv.notify_all();
                }
            }
            if (stale)
            {
                continue;
            }

            std::stop_token job_stop;
            {
                // Trailing-edge debounce: a keystroke arriving within the window
                // supersedes this version instead of paying for a full publish.
                std::unique_lock<std::mutex> lock(m_diag_mu);
                m_diag_cv.wait_for(lock, 50ms,[&]
                    {
                        if (stop.stop_requested())
                        {
                            return true;
                    }

                        for (const auto & queued: m_diag_queue)
                        {
                            if (queued.uri == job.uri)
                            {
                                return true;
                        }
                    }

                        return false;
                });
                if (stop.stop_requested())
                {
                    m_diag_busy = false;
                    m_diag_cv.notify_all();
                    return;
                }

                bool superseded = false;
                for (const auto & queued: m_diag_queue)
                {
                    if (queued.uri == job.uri)
                    {
                        superseded = true;
                        break;
                    }
                }

                if (superseded)
                {
                    m_diag_busy = false;
                    m_diag_cv.notify_all();
                    continue;
                }

                // Registered under the same lock EnqueueDiagnostics takes, so a
                // keystroke can never slip between "not superseded" and "running".
                m_diag_stop = std::stop_source();
                m_diag_running_uri = job.uri;
                job_stop = m_diag_stop.get_token();
            }
            {
                // Drop versions that are already obsolete (closed or re-edited).
                std::shared_ptr<const std::string> text;
                std::int64_t version = -1;
                if (const auto document = GetDocument(job.uri))
                {
                    text = document->text;
                    version = document->version;
                }

                if (text && version == job.version)
                {
                    struct Scope
                    {
                        RequestContext context;
                        Scope(std::stop_token stop)
                        {
                            context.stop = std::move(stop);
                            t_context = &context;
                        }
                        ~Scope()
                        {
                            t_context = nullptr;
                        }
                    } scope(job_stop);
                    PublishDiagnostics(job.uri, std::move(text), version);
                }

                const std::lock_guard<std::mutex> lock(m_diag_mu);
                m_diag_running_uri.clear();
                m_diag_busy = false;
                m_diag_cv.notify_all();
            }
        }
    }

    namespace
    {
        // Source files that may hold the body of something declared in `owner`:
        // siblings sharing its stem (`foo.hpp` -> `foo.cpp`), compile-database
        // entries with that stem, and the open documents.
        constexpr std::string_view kSourceExtensions[] = {".cpp", ".cc", ".cxx", ".c++", ".cp", ".C"};

        bool IsSourceExtension(const std::filesystem::path & path)
        {
            const std::string extension = path.extension().string();
            for (const auto candidate: kSourceExtensions)
            {
                if (extension == candidate)
                {
                    return true;
                }
            }

            return false;
        }

        std::optional<std::string> ReadWholeFile(const std::filesystem::path & path)
        {
            constexpr std::uintmax_t kMaxBytes = 4u << 20;
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error || size > kMaxBytes)
            {
                return std::nullopt;
            }

            std::ifstream stream(path, std::ios::binary);
            if (!stream)
            {
                return std::nullopt;
            }

            std::string content(static_cast<std::size_t>(size), '\0');
            stream.read(content.data(), static_cast<std::streamsize>(size));
            content.resize(static_cast<std::size_t>(stream.gcount()));
            return content;
        }

        // One file's text plus its line index, kept alive together (LineIndex
        // borrows the text).
        struct LoadedFile
        {
            std::string uri;
            std::shared_ptr<const std::string> text;
            std::unique_ptr<LineIndex> lines;
        };

        void AppendLocation(const LoadedFile &file, std::size_t offset, std::size_t length,
            std::string & out)
        {
            if (!out.empty() && out.back() != '[')
            {
                out += ',';
            }

            out += "{\"uri\":";
            QuoteJson(file.uri, out);
            out += ",\"range\":{\"start\":";
            AppendPosition(file.lines->ToPosition(offset), out);
            out += ",\"end\":";
            AppendPosition(file.lines->ToPosition(offset + length), out);
            out += "}}";
        }
    } // namespace

    void LanguageServer::GotoDocument(simdjson::dom::element request, std::string_view id,
        bool implementation)
    {
        simdjson::dom::object params;
        simdjson::dom::object text_document;
        simdjson::dom::object position;
        std::string_view uri;
        if (!GetObject(request, "params", params) ||!GetObject(params, "textDocument", text_document) ||
            !GetString(text_document, "uri", uri) ||!GetObject(params, "position", position))
        {
            Respond(id, "null");
            return;
        }

        const std::string uri_string(uri);
        const auto document = GetDocument(uri_string);
        if (!document)
        {
            Respond(id, "null");
            return;
        }

        const std::shared_ptr<const std::string> text = document->text;
        const std::int64_t version = document->version;
        const std::shared_ptr<const LineIndex> lines = document->lines;

        const Position cursor = {static_cast<std::size_t>(PositionNumber(position, "line")),
            static_cast<std::size_t>(PositionNumber(position, "character"))};
        const std::size_t offset = lines->OffsetFromPosition(cursor);

        const heimdall::CompileCommand * command = CommandFor(uri_string);

        // On an `#include` line, navigate to the header file itself.
        if (!implementation)
        {
            const std::filesystem::path self_file = PathFromUri(uri_string);
            const heimdall::CompileCommand * include_command = command;
            if (include_command == nullptr)
            {
                if (m_compile_database != std::nullopt)
                {
                    include_command = m_compile_database->FindOrNearest(self_file);
                }
            }

            const std::filesystem::path header = heimdall::IncludeIndex::ResolveIncludeAt(
                self_file.has_parent_path() ? self_file.parent_path() : std::filesystem::path(), *text, offset,
                include_command);
            if (!header.empty())
            {
                std::string response = "[{\"uri\":";
                QuoteJson(UriFromPath(header), response);
                response += ",\"range\":{\"start\":{\"line\":0,\"character\":0},"
                "\"end\":{\"line\":0,\"character\":0}}}]";
                Respond(id, response);
                return;
            }
        }

        const HeaderView headers = AwaitHeaderScopes(uri_string, text, command);
        const auto tree = CachedParse(uri_string, text, version, command);
        if (!tree)
        {
            RespondCancelled(id);
            return;
        }

        if (RequestCancelled())
        {
            RespondCancelled(id);
            return;
        }

        const heimdall::ScopeIndex * scopes = headers.index ? &headers.index->Scopes() : nullptr;
        std::vector<heimdall::NavTarget> targets = implementation
        ? heimdall::Navigation::Implementation(*tree, offset, scopes)
        : heimdall::Navigation::Definition(*tree, offset, scopes);

        // File table: this document, header-index files (read on demand) and the
        // source files searched below, each loaded once per request.
        std::unordered_map<std::string, LoadedFile> loaded; // keyed by uri
        // `disk` reads the file from disk even when it is open: header-index
        // offsets were computed on the disk content, so positions must be too.
        auto load =[&](const std::filesystem::path & path, std::string uri_for,
            bool disk = false) -> LoadedFile *
        {
            if (uri_for.empty())
            {
                uri_for = UriFromPath(path);
            }

            const std::string cache_key = disk ? uri_for + "#disk" : uri_for;
            if (const auto found = loaded.find(cache_key); found != loaded.end())
            {
                return &found->second;
            }

            std::shared_ptr<const std::string> content;
            if (!disk)
            {
                const std::shared_lock<std::shared_mutex> lock(m_docs_mu);
                for (const auto &[open_uri, document]: m_documents)
                {
                    if (open_uri == uri_for || PathFromUri(open_uri) == path)
                    {
                        content = document.text;
                        break;
                    }
                }
            }

            if (!content)
            {
                auto read = ReadWholeFile(path);
                if (!read)
                {
                    return nullptr;
                }

                content = std::make_shared<const std::string>(std::move(*read));
            }

            LoadedFile file;
            file.uri = uri_for;
            file.text = std::move(content);
            file.lines = std::make_unique<LineIndex>();
            file.lines->Build(*file.text);
            return &loaded.emplace(cache_key, std::move(file)).first->second;
        };

        LoadedFile *self = nullptr;
        {
            LoadedFile file;
            file.uri = uri_string;
            file.text = text;
            file.lines = std::make_unique<LineIndex>();
            file.lines->Build(*text);
            self = &loaded.emplace(uri_string, std::move(file)).first->second;
        }

        struct Resolved
        {
            LoadedFile *file = nullptr;
            std::size_t offset = 0;
            std::size_t length = 0;
        };

        std::vector<Resolved> resolved;
        auto add_resolved =[&](LoadedFile *file, std::size_t at, std::size_t length)
        {
            for (const Resolved & existing: resolved)
            {
                if (existing.file == file && existing.offset == at)
                {
                    return;
                }
            }

            resolved.push_back({file, at, length});
        };

        // Candidate files for bodies of things declared in `owner`.
        auto source_candidates =[&](const std::filesystem::path & owner)
        {
            std::vector<std::filesystem::path> candidates;
            auto push =[&](const std::filesystem::path & path)
            {
                if (path != owner && std::find(candidates.begin(), candidates.end(), path) == candidates.end())
                {
                    candidates.push_back(path);
                }
            };
            const std::string stem = owner.stem().string();
            std::error_code error;
            const std::filesystem::path directory = owner.parent_path();
            for (const auto extension: kSourceExtensions)
            {
                const std::filesystem::path sibling = directory /(stem + std::string(extension));
                if (std::filesystem::is_regular_file(sibling, error))
                {
                    push(sibling);
                }
            }

            {
                const std::shared_lock<std::shared_mutex> lock(m_docs_mu);
                if (m_compile_database != std::nullopt)
                {
                    for (const auto & entry: m_compile_database->Commands())
                    {
                        if (entry.file.stem() == owner.stem() && IsSourceExtension(entry.file))
                        {
                            push(entry.file);
                        }
                    }
                }

                for (const auto &[open_uri, document]: m_documents)
                {
                    const std::filesystem::path open_path = PathFromUri(open_uri);
                    if (IsSourceExtension(open_path))
                    {
                        push(open_path);
                    }
                }
            }

            return candidates;
        };

        const heimdall::ParserOptions parser_options = ParserOptionsFor(command);
        const std::filesystem::path self_path = PathFromUri(uri_string);
        for (const heimdall::NavTarget & target: targets)
        {
            LoadedFile *file = self;
            std::filesystem::path owner = self_path;
            if (target.file >= 0)
            {
                const auto &files = headers.index->Files();
                if (static_cast<std::size_t>(target.file) >= files.size())
                {
                    continue;
                }

                owner = files[static_cast<std::size_t>(target.file)];
                file = load(owner, {}, true);
                if (file == nullptr)
                {
                    continue;
                }
            }

            // A function declared without a body: follow it into its source files.
            const bool function = target.kind == heimdall::CompletionKind::Function;
            bool followed = false;
            if (function && !target.is_definition && !target.back_reference)
            {
                for (const auto & candidate: source_candidates(owner))
                {
                    LoadedFile *source = load(candidate, {});
                    if (source == nullptr)
                    {
                        continue;
                    }

                    const heimdall::ParseTree source_tree = heimdall::ParseTree::Parse(*source->text, parser_options,
                        CurrentStop());
                    if (source_tree.Cancelled())
                    {
                        RespondCancelled(id);
                        return;
                    }

                    for (const auto & found: heimdall::Navigation::FindDefinitions(source_tree, target.scope,
                        target.name, true, target.param_count))
                    {
                        add_resolved(source, found.offset, found.length);
                        followed = true;
                    }
                }
            }

            // Implementation of a method also reports overriders in other files.
            if (implementation && function && !target.scope.empty())
            {
                for (const auto & candidate: source_candidates(owner))
                {
                    LoadedFile *source = load(candidate, {});
                    if (source == nullptr)
                    {
                        continue;
                    }

                    const heimdall::ParseTree source_tree = heimdall::ParseTree::Parse(*source->text, parser_options,
                        CurrentStop());
                    if (source_tree.Cancelled())
                    {
                        RespondCancelled(id);
                        return;
                    }

                    for (const auto & found: heimdall::Navigation::FindOverriders(source_tree, target.scope.back(),
                        target.name, target.param_count))
                    {
                        add_resolved(source, found.offset, found.length);
                        followed = true;
                    }
                }
            }

            if (!followed)
            {
                add_resolved(file, target.offset, target.length);
            }
        }

        if (resolved.empty())
        {
            Respond(id, "[]");
            return;
        }

        std::string response = "[";
        for (const Resolved & item: resolved)
        {
            AppendLocation(*item.file, item.offset, item.length, response);
        }

        response += ']';
        Respond(id, response);
    }

} // namespace heimdall::lsp
