#include "Server.hpp"

#include "Document.hpp"
#include "JsonRpc.hpp"

#include <Heimdall/Formatter.hpp>
#include <Heimdall/ParseTree.hpp>
#include <Heimdall/RuleEngine.hpp>
#include <Heimdall/SemanticAnalyzer.hpp>
#include <Heimdall/Completion.hpp>

#include <chrono>
#include <filesystem>

namespace heimdall::lsp
{

    LanguageServer::LanguageServer() = default;
    LanguageServer::~LanguageServer() = default;

    bool LanguageServer::Run()
    {
        m_index_worker = std::jthread([this](std::stop_token stop) { IndexWorkerMain(stop); });
        m_diag_worker = std::jthread([this](std::stop_token stop) { DiagWorkerMain(stop); });

        std::thread([] { heimdall::IncludeIndex::SystemIncludes("c++"); }).detach();

        auto stop_workers = [&] {
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
            if (parser.parse(body).get(request)) continue;

            simdjson::dom::object object;
            if (request.get_object().get(object)) continue;
            std::string_view method;
            if (!GetString(object, "method", method)) continue;

            simdjson::dom::element id;
            const bool has_id = !object["id"].get(id);
            std::string id_json = has_id ? simdjson::minify(id) : "null";

            if (method == "initialize")
            {
                LoadInitializationOptions(request);
                Respond(id_json,
                "{\"capabilities\":{\"textDocumentSync\":2,\"documentFormattingProvider\":true,"
                "\"codeActionProvider\":true,\"hoverProvider\":true,"
                "\"completionProvider\":{\"triggerCharacters\":[\".\",\">\",\":\",\"#\"],"
                "\"resolveProvider\":false}},"
                "\"serverInfo\":{\"name\":\"Heimdall\",\"version\":\"0.1.0\"}}");
            }
            else if (method == "initialized")
            {
            }
            else if (method == "shutdown")
            {
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
                FormatDocument(request, id_json);
            }
            else if (method == "textDocument/codeAction")
            {
                CodeActions(request, id_json);
            }
            else if (method == "textDocument/completion")
            {
                CompleteDocument(request, id_json);
            }
            else if (method == "textDocument/hover")
            {
                HoverDocument(request, id_json);
            }
            else if (method == "$/cancelRequest")
            {
                // Cooperative cancellation: the id is recorded and consulted
                // before expensive work (header builds, diagnostics publish).
                // Synchronous handlers below already finished by the time this
                // is processed, so the flag mainly guards queued background jobs.
                simdjson::dom::object params;
                if (GetObject(request, "params", params))
                {
                    simdjson::dom::element cancel_id;
                    if (!params["id"].get(cancel_id))
                    {
                        const std::lock_guard<std::mutex> lock(m_mu);
                        // Bound the set: ids for already-answered requests never match.
                        if (m_cancelled.size() > 1024) m_cancelled.clear();
                        m_cancelled.insert(simdjson::minify(cancel_id));
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

    void LanguageServer::LoadInitializationOptions(simdjson::dom::element request)
    {
        simdjson::dom::object params;
        simdjson::dom::object options;
        if (!GetObject(request, "params", params) || !GetObject(params, "initializationOptions", options)) return;


        if (const auto error = options["enableSemantic"].get_bool().get(m_enable_semantic); error)
        {
            m_enable_semantic = false;
        }

        std::string_view path;
        if (!GetString(options, "compileCommands", path) || path.empty()) return;
        auto database = heimdall::CompileDatabase::Load(std::filesystem::path(path));
        if (!database)
        {
            m_initialization_error = database.error();
            return;
        }
        m_compile_database = std::move(*database);


        std::unordered_set<std::string> drivers = { "c++" };
        for (const auto& command : m_compile_database->Commands())
        {
            if (!command.arguments.empty() && !command.arguments.front().empty())
            {
                drivers.insert(command.arguments.front());
            }
        }
        std::thread([drivers = std::move(drivers)] {
            for (const auto& driver : drivers) heimdall::IncludeIndex::SystemIncludes(driver);
        }).detach();
    }

    void LanguageServer::Respond(std::string_view id, std::string_view result)
    {
        Send("{\"jsonrpc\":\"2.0\",\"id\":" + std::string(id) + ",\"result\":" + std::string(result) + "}");
    }

    void LanguageServer::PublishDiagnostics(const std::string& uri, std::shared_ptr<const std::string> text,
    std::int64_t version)
    {
        // Single-pass pipeline: one lex + preprocess + grammar pass per version,
        // shared by the rule engine, the semantic pass and the syntax errors
        // (was: 4 lexes + 2 preprocesses of the same buffer per keystroke).
        const heimdall::CompileCommand* command = nullptr;
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            if (m_compile_database != std::nullopt) command = m_compile_database->Find(PathFromUri(uri));
        }
        const auto tree = CachedParse(uri, text, version, command);
        const auto diagnostics = heimdall::RuleEngine().Analyze(*tree);
        std::vector<heimdall::SemanticDiagnostic> semantic_diagnostics;
        if (m_enable_semantic && command != nullptr)
        {
            semantic_diagnostics = heimdall::SemanticAnalyzer().AnalyzeUnusedLocals(*tree, command);
        }
        // One line-index build per publish; every diagnostic position below is
        // O(log L) (was: an O(file size) scan per diagnostic).
        LineIndex lines;
        lines.Build(*text);
        std::string message = "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":";
        QuoteJson(uri, message);
        message += ",\"version\":" + std::to_string(version) + ",\"diagnostics\":[";
        bool first = true;
        for (const auto& diagnostic : diagnostics)
        {
            if (!first) message += ',';
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
        for (const auto& diagnostic : semantic_diagnostics)
        {
            if (!first) message += ',';
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
        for (const auto& diagnostic : tree->Diagnostics())
        {
            if (!first) message += ',';
            first = false;
            const Position start = lines.ToPosition(diagnostic.offset);
            message += "{\"range\":{\"start\":";
            AppendPosition(start, message);
            message += ",\"end\":";
            AppendPosition(start, message);
            message += "},\"severity\":1,\"code\":\"HEIMDALL900\",\"source\":\"heimdall\",\"message\":";
            QuoteJson(diagnostic.message, message);
            message += '}';
        }
        message += "]}}";
        if (!m_initialization_error.empty())
        {
            std::string notification = "{\"jsonrpc\":\"2.0\",\"method\":\"window/showMessage\",\"params\":{\"type\":2,\"message\":";
            QuoteJson(m_initialization_error, notification);
            notification += "}}";
            Send(notification);
            m_initialization_error.clear();
        }
        Send(message);
    }

    bool LanguageServer::DocumentParams(simdjson::dom::element request, std::string_view& uri,
    simdjson::dom::object& document)
    {
        simdjson::dom::object params;
        if (!GetObject(request, "params", params)) return false;
        if (!GetObject(params, "textDocument", document)) return false;
        return GetString(document, "uri", uri);
    }

    void LanguageServer::OpenDocument(simdjson::dom::element request)
    {
        std::string_view uri;
        simdjson::dom::object text_document;
        if (!DocumentParams(request, uri, text_document)) return;
        std::string_view text;
        if (!GetString(text_document, "text", text)) return;
        std::int64_t version = 0;
        if (const auto error = text_document["version"].get_int64().get(version); error)
        {
            version = 0;
        }
        const std::string uri_string(uri);
        DocumentSnapshot snapshot;
        snapshot.text = std::make_shared<const std::string>(text);
        snapshot.version = version;
        snapshot.lines.Build(*snapshot.text);
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            m_documents[uri_string] = std::move(snapshot);
            m_parse_cache.erase(uri_string);
        }
        EnqueueDiagnostics(uri_string, version);
    }

    bool LanguageServer::ApplyContentChange(std::string& current, simdjson::dom::object change)
    {
        std::string_view text;
        if (!GetString(change, "text", text)) return false;
        simdjson::dom::object range;
        if (!GetObject(change, "range", range))
        {
            // Full-document sync (textDocumentSync = 1 fallback).
            current.assign(text);
            return true;
        }
        // Incremental sync (textDocumentSync = 2): splice the range.
        simdjson::dom::object start_object;
        simdjson::dom::object end_object;
        if (!GetObject(range, "start", start_object) || !GetObject(range, "end", end_object)) return false;
        auto number = [](simdjson::dom::object object, const char* key) -> std::size_t {
            std::uint64_t unsigned_value = 0;
            if (!object[key].get_uint64().get(unsigned_value)) return static_cast<std::size_t>(unsigned_value);
            std::int64_t signed_value = 0;
            if (!object[key].get_int64().get(signed_value) && signed_value > 0)
            {
                return static_cast<std::size_t>(signed_value);
            }
            return 0;
        };
        const Position start = { number(start_object, "line"), number(start_object, "character") };
        const Position end = { number(end_object, "line"), number(end_object, "character") };
        LineIndex index;
        index.Build(current);
        std::size_t start_offset = index.OffsetFromPosition(start);
        std::size_t end_offset = index.OffsetFromPosition(end);
        if (start_offset > current.size()) start_offset = current.size();
        if (end_offset > current.size()) end_offset = current.size();
        if (end_offset < start_offset) end_offset = start_offset;
        current.replace(start_offset, end_offset - start_offset, text);
        return true;
    }

    void LanguageServer::ChangeDocument(simdjson::dom::element request)
    {
        std::string_view uri;
        simdjson::dom::object text_document;
        if (!DocumentParams(request, uri, text_document)) return;
        const std::string uri_string(uri);
        simdjson::dom::object params;
        if (!GetObject(request, "params", params)) return;
        simdjson::dom::array changes;
        if (params["contentChanges"].get_array().get(changes)) return;
        std::int64_t version = 0;
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            const auto found = m_documents.find(uri_string);
            if (found == m_documents.end()) return;
            std::string current(*found->second.text);
            for (simdjson::dom::element change : changes)
            {
                simdjson::dom::object change_object;
                if (change.get_object().get(change_object)) continue;
                ApplyContentChange(current, change_object);
            }
            if (const auto error = text_document["version"].get_int64().get(found->second.version); error)
            {
                found->second.version = 0;
            }
            found->second.text = std::make_shared<const std::string>(std::move(current));
            found->second.lines.Build(*found->second.text);
            version = found->second.version;
            m_parse_cache.erase(uri_string);
        }
        EnqueueDiagnostics(uri_string, version);
    }

    void LanguageServer::CloseDocument(simdjson::dom::element request)
    {
        std::string_view uri;
        simdjson::dom::object text_document;
        if (!DocumentParams(request, uri, text_document)) return;
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            m_documents.erase(std::string(uri));
            m_include_cache.erase(std::string(uri));
            m_parse_cache.erase(std::string(uri));
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
        std::shared_ptr<const std::string> text;
        std::int64_t version = 0;
        LineIndex lines;
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            const auto found = m_documents.find(uri_string);
            if (found == m_documents.end())
            {
                Respond(id, "[]");
                return;
            }
            text = found->second.text;
            version = found->second.version;
            lines = found->second.lines;
        }
        const heimdall::CompileCommand* command = nullptr;
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            if (m_compile_database != std::nullopt) command = m_compile_database->Find(PathFromUri(uri_string));
        }
        const auto tree = CachedParse(uri_string, text, version, command);
        const std::string formatted = heimdall::Formatter().Format(*tree);
        if (formatted == *text)
        {
            Respond(id, "[]");
            return;
        }
        std::string response = "[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":";
        AppendPosition(lines.ToPosition(text->size()), response);
        response += "},\"newText\":";
        QuoteJson(formatted, response);
        response += "}]";
        Respond(id, response);
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
        std::shared_ptr<const std::string> text;
        std::int64_t version = 0;
        LineIndex lines;
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            const auto found = m_documents.find(uri_string);
            if (found == m_documents.end())
            {
                Respond(id, "[]");
                return;
            }
            text = found->second.text;
            version = found->second.version;
            lines = found->second.lines;
        }
        const heimdall::CompileCommand* command = nullptr;
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            if (m_compile_database != std::nullopt) command = m_compile_database->Find(PathFromUri(uri_string));
        }
        const auto tree = CachedParse(uri_string, text, version, command);
        const auto diagnostics = heimdall::RuleEngine().Analyze(*tree);
        std::string response = "[";
        bool first = true;
        for (const auto& diagnostic : diagnostics)
        {
            if (!diagnostic.has_fix) continue;
            if (!first) response += ',';
            first = false;
            response += "{\"title\":";
            QuoteJson("Fix " + diagnostic.code + ": " + diagnostic.message, response);
            response += ",\"kind\":\"quickfix\",\"edit\":{\"changes\":{";
            QuoteJson(uri_string, response);
            response += ":[{\"range\":{\"start\":";
            AppendPosition(lines.ToPosition(diagnostic.fix.offset), response);
            response += ",\"end\":";
            AppendPosition(lines.ToPosition(diagnostic.fix.offset + diagnostic.fix.length), response);
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
                case heimdall::CompletionKind::Function: return 3;
                case heimdall::CompletionKind::Variable: return 6;
                case heimdall::CompletionKind::Type: return 7;
                case heimdall::CompletionKind::Namespace: return 9;
                case heimdall::CompletionKind::Macro: return 21;
                case heimdall::CompletionKind::Directive:
                case heimdall::CompletionKind::Keyword: return 14;
            }
            return 14;
        }

        std::uint64_t PositionNumber(simdjson::dom::object position, const char* key)
        {
            std::uint64_t unsigned_value = 0;
            if (!position[key].get_uint64().get(unsigned_value)) return unsigned_value;
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
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            if (m_cancelled.erase(std::string(id)) > 0)
            {
                Respond(id, "{\"isIncomplete\":false,\"items\":[]}");
                return;
            }
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
        std::shared_ptr<const std::string> text;
        std::int64_t version = 0;
        LineIndex lines;
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            const auto found = m_documents.find(uri_string);
            if (found == m_documents.end())
            {
                Respond(id, "{\"isIncomplete\":false,\"items\":[]}");
                return;
            }
            text = found->second.text;
            version = found->second.version;
            lines = found->second.lines;
        }
        simdjson::dom::object position;
        if (!GetObject(params, "position", position))
        {
            Respond(id, "{\"isIncomplete\":false,\"items\":[]}");
            return;
        }
        const Position cursor = { static_cast<std::size_t>(PositionNumber(position, "line")),
            static_cast<std::size_t>(PositionNumber(position, "character")) };
        const std::size_t offset = lines.OffsetFromPosition(cursor);

        const heimdall::CompileCommand* command = nullptr;
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            if (m_compile_database != std::nullopt) command = m_compile_database->Find(PathFromUri(uri_string));
        }
        const heimdall::ParserOptions parser_options = ParserOptionsFor(command);
        const HeaderView headers = HeaderScopes(uri_string, text, command);
        const auto tree = CachedParse(uri_string, text, version, command);
        const auto items = heimdall::CompletionEngine::Complete(
        *tree, parser_options, offset, headers.index ? &headers.index->Scopes() : nullptr);
        const std::string prefix = heimdall::CompletionEngine::PrefixAt(*text, offset);
        const Position start = lines.ToPosition(offset - prefix.size());
        const Position end = lines.ToPosition(offset);

        std::string response = headers.complete ? "{\"isIncomplete\":false,\"items\":["
        : "{\"isIncomplete\":true,\"items\":[";
        bool first = true;
        for (const auto& item : items)
        {
            if (!first) response += ',';
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

    void LanguageServer::TouchGlobalIndex(const std::string& key)
    {
        auto found = m_global_indices.find(key);
        if (found == m_global_indices.end()) return;
        m_lru.erase(found->second.lru);
        m_lru.push_front(key);
        found->second.lru = m_lru.begin();
    }

    LanguageServer::HeaderView LanguageServer::HeaderScopes(const std::string& uri,
    const std::shared_ptr<const std::string>& text,
    const heimdall::CompileCommand* command)
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
        const std::string fingerprint = heimdall::IncludeIndex::IncludeFingerprint(base_dir, *text, command);

        std::vector<std::filesystem::path> headers;
        std::string requested_key;
        std::string served_key;
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            auto& entry = m_include_cache[uri];
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
                        return { global->second.index, true };
                    }
                    if (m_index_pending.contains(requested_key))
                    {
                        if (!served_key.empty())
                        {
                            if (const auto stale = m_global_indices.find(served_key);
                            stale != m_global_indices.end())
                            {
                                TouchGlobalIndex(served_key);
                                return { stale->second.index, false };
                            }
                        }
                        return { nullptr, false };
                    }
                    // Requested but neither built nor pending (evicted): rebuild.
                    m_index_pending.insert(requested_key);
                    {
                        const std::lock_guard<std::mutex> queue_lock(m_index_mu);
                        m_index_queue.push_back({ requested_key, headers, command });
                    }
                    m_index_cv.notify_one();
                }
                if (!served_key.empty())
                {
                    if (const auto stale = m_global_indices.find(served_key);
                    stale != m_global_indices.end())
                    {
                        TouchGlobalIndex(served_key);
                        return { stale->second.index, false };
                    }
                }
                return { nullptr, requested_key.empty() };
            }
        }
        // Slow path: fingerprint changed. Resolve + stat outside the lock.
        headers = heimdall::IncludeIndex::ResolveHeaders(base_dir, *text, command);
        if (headers.empty())
        {
            // No headers: nothing to build, trivially complete (and shared, since
            // the key would be identical for every header-less file).
            const std::lock_guard<std::mutex> lock(m_mu);
            auto& entry = m_include_cache[uri];
            entry.fingerprint = fingerprint;
            entry.headers = headers;
            entry.requested_key.clear();
            entry.served_key.clear();
            return { nullptr, true };
        }
        requested_key = heimdall::IncludeIndex::CacheKey(headers, command);
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            auto& entry = m_include_cache[uri];
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
                    return { global->second.index, true };
                }
                if (!m_index_pending.contains(requested_key))
                {
                    m_index_pending.insert(requested_key);
                    {
                        const std::lock_guard<std::mutex> queue_lock(m_index_mu);
                        m_index_queue.push_back({ requested_key, headers, command });
                    }
                    m_index_cv.notify_one();
                }
            }
            else
            {
                entry.served_key.clear();
                return { nullptr, true };
            }
            served_key = entry.served_key;
            if (!served_key.empty())
            {
                if (const auto stale = m_global_indices.find(served_key); stale != m_global_indices.end())
                {
                    TouchGlobalIndex(served_key);
                    return { stale->second.index, false };
                }
            }
            return { nullptr, false };
        }
    }

    heimdall::ParserOptions LanguageServer::ParserOptionsFor(const heimdall::CompileCommand* command)
    {
        heimdall::ParserOptions options;
        if (command == nullptr) return options;
        options.standard = command->standard;
        const std::lock_guard<std::mutex> lock(m_mu);
        if (const auto found = m_macro_cache.find(command); found != m_macro_cache.end())
        {
            options.shared_macros = found->second;
            return options;
        }
        auto macros = std::make_shared<heimdall::Preprocessor::MacroMap>(command->defines);
        for (const auto& name : command->undefines) macros->erase(name);
        m_macro_cache.emplace(command, macros);
        options.shared_macros = std::move(macros);
        return options;
    }

    std::shared_ptr<const heimdall::ParseTree> LanguageServer::CachedParse(
    const std::string& uri, const std::shared_ptr<const std::string>& text, std::int64_t version,
    const heimdall::CompileCommand* command)
    {
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            if (const auto found = m_parse_cache.find(uri);
            found != m_parse_cache.end() && found->second.version == version &&
            found->second.text.get() == text.get() && found->second.tree)
            {
                return found->second.tree;
            }
        }
        // Parse outside the lock; the tree takes shared ownership of the buffer
        // so a concurrent didChange cannot leave it dangling (was: string_view
        // into a Document::text that didChange overwrote via assign).
        heimdall::ParserOptions options = ParserOptionsFor(command);
        auto tree = std::make_shared<heimdall::ParseTree>(heimdall::ParseTree::Parse(*text, options));
        tree->HoldSource(text);
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            auto& entry = m_parse_cache[uri];
            entry.version = version;
            entry.text = text;
            entry.options = std::move(options);
            entry.tree = tree;
        }
        return tree;
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
        std::shared_ptr<const std::string> text;
        std::int64_t version = 0;
        LineIndex lines;
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            const auto found = m_documents.find(uri_string);
            if (found == m_documents.end())
            {
                Respond(id, "null");
                return;
            }
            text = found->second.text;
            version = found->second.version;
            lines = found->second.lines;
        }
        simdjson::dom::object position;
        if (!GetObject(params, "position", position))
        {
            Respond(id, "null");
            return;
        }
        const Position cursor = { static_cast<std::size_t>(PositionNumber(position, "line")),
            static_cast<std::size_t>(PositionNumber(position, "character")) };
        const std::size_t offset = lines.OffsetFromPosition(cursor);

        const heimdall::CompileCommand* command = nullptr;
        {
            const std::lock_guard<std::mutex> lock(m_mu);
            if (m_compile_database != std::nullopt) command = m_compile_database->Find(PathFromUri(uri_string));
        }
        const heimdall::ParserOptions parser_options = ParserOptionsFor(command);
        const HeaderView headers = HeaderScopes(uri_string, text, command);
        const auto tree = CachedParse(uri_string, text, version, command);
        const auto hovered = heimdall::CompletionEngine::Hover(
        *tree, parser_options, offset, headers.index ? &headers.index->Scopes() : nullptr);
        if (!hovered.has_value())
        {
            Respond(id, "null");
            return;
        }
        // One `cpp` line (`int add(int left, int right)`, `unused: int`) plus the
        // doc comment below, mirroring the CLion-style side popup.
        std::string line = hovered->label;
        if (!hovered->detail.empty() && hovered->detail != hovered->label)
        {
            line = hovered->detail.find(hovered->label) == std::string::npos
            ? hovered->label + ": " + hovered->detail
            : hovered->detail;
        }
        std::string value = "```cpp\n" + line + "\n```";
        if (!hovered->documentation.empty()) value += "\n\n" + hovered->documentation;
        std::string response = "{\"contents\":{\"kind\":\"markdown\",\"value\":";
        QuoteJson(value, response);
        response += "}}";
        Respond(id, response);
    }

    void LanguageServer::IndexWorkerMain(std::stop_token stop)
    {
        while (!stop.stop_requested())
        {
            IndexJob job;
            {
                std::unique_lock<std::mutex> lock(m_index_mu);
                m_index_cv.wait(lock, stop, [&] { return !m_index_queue.empty(); });
                if (stop.stop_requested()) return;
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
                m_global_indices[job.key] = { std::move(built), m_lru.begin() };
                for (auto& [uri, entry] : m_include_cache)
                {
                    if (entry.requested_key == job.key) entry.served_key = job.key;
                }
            }
        }
    }

    void LanguageServer::EnqueueDiagnostics(const std::string& uri, std::int64_t version)
    {
        {
            const std::lock_guard<std::mutex> lock(m_diag_mu);
            m_diag_queue.push_back({ uri, version });
        }
        m_diag_cv.notify_one();
    }

    void LanguageServer::FlushDiagnostics()
    {
        std::unique_lock<std::mutex> lock(m_diag_mu);
        m_diag_cv.wait(lock, [&] { return m_diag_queue.empty() && !m_diag_busy; });
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
                m_diag_cv.wait(lock, stop, [&] { return !m_diag_queue.empty(); });
                if (stop.stop_requested()) return;
                job = m_diag_queue.front();
                m_diag_queue.pop_front();
                // Coalesce bursts: a newer queued job for the same uri makes
                // this one stale before it even starts.
                for (const auto& queued : m_diag_queue)
                {
                    if (queued.uri == job.uri)
                    {
                        stale = true;
                        break;
                    }
                }
                if (!stale) m_diag_busy = true;
                else m_diag_cv.notify_all();
            }
            if (stale) continue;
            {
                // Trailing-edge debounce: a keystroke arriving within the window
                // supersedes this version instead of paying for a full publish.
                std::unique_lock<std::mutex> lock(m_diag_mu);
                m_diag_cv.wait_for(lock, 50ms, [&] {
                    if (stop.stop_requested()) return true;
                    for (const auto& queued : m_diag_queue)
                    {
                        if (queued.uri == job.uri) return true;
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
                for (const auto& queued : m_diag_queue)
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
            }
            {
                // Drop versions that are already obsolete (closed or re-edited).
                std::shared_ptr<const std::string> text;
                std::int64_t version = -1;
                {
                    const std::lock_guard<std::mutex> lock(m_mu);
                    if (const auto found = m_documents.find(job.uri); found != m_documents.end())
                    {
                        text = found->second.text;
                        version = found->second.version;
                    }
                }
                if (text && version == job.version)
                {
                    PublishDiagnostics(job.uri, std::move(text), version);
                }
                const std::lock_guard<std::mutex> lock(m_diag_mu);
                m_diag_busy = false;
                m_diag_cv.notify_all();
            }
        }
    }

} // namespace heimdall::lsp
