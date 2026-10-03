#include "Server.hpp"

#include "Document.hpp"
#include "JsonRpc.hpp"

#include <Heimdall/Formatter.hpp>
#include <Heimdall/ParseTree.hpp>
#include <Heimdall/RuleEngine.hpp>
#include <Heimdall/SemanticAnalyzer.hpp>
#include <Heimdall/Completion.hpp>

#include <filesystem>

namespace heimdall::lsp
{

bool LanguageServer::Run()
{
    std::string body;
    // Reused across messages: was reconstructed (with its internal buffers)
    // on every request.
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
                    "{\"capabilities\":{\"textDocumentSync\":1,\"documentFormattingProvider\":true,"
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
            Respond(id_json, "null");
        }
        else if (method == "exit")
        {
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
            // No async work to cancel yet (requests run synchronously on the
            // I/O thread); acknowledge by ignoring so it never falls through
            // to the generic response branch.
        }
        else if (has_id)
        {
            Respond(id_json, "null");
        }
    }
    return false;
}

void LanguageServer::LoadInitializationOptions(simdjson::dom::element request)
{
    simdjson::dom::object params;
    simdjson::dom::object options;
    if (!GetObject(request, "params", params) || !GetObject(params, "initializationOptions", options)) return;
    // The compile database feeds both the parser (dialect + predefined macros)
    // and the optional semantic analysis, so load it whenever a path is given.
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
}

void LanguageServer::Respond(std::string_view id, std::string_view result)
{
    Send("{\"jsonrpc\":\"2.0\",\"id\":" + std::string(id) + ",\"result\":" + std::string(result) + "}");
}

void LanguageServer::PublishDiagnostics(std::string_view uri, const Document& document)
{
    const auto diagnostics = heimdall::RuleEngine().Analyze(document.text);
    const auto* command = m_compile_database ? m_compile_database->Find(PathFromUri(uri)) : nullptr;
    const std::string uri_string(uri);
    const auto& parse_tree = CachedParse(uri_string, document, command);
    std::vector<heimdall::SemanticDiagnostic> semantic_diagnostics;
    if (m_enable_semantic && command != nullptr)
    {
        semantic_diagnostics = heimdall::SemanticAnalyzer().AnalyzeUnusedLocals(document.text, command);
    }
    std::string message = "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":";
    QuoteJson(uri, message);
    message += ",\"version\":" + std::to_string(document.version) + ",\"diagnostics\":[";
    bool first = true;
    for (const auto& diagnostic : diagnostics)
    {
        if (!first) message += ',';
        first = false;
        const Position start = ToPosition(document.text, diagnostic.offset);
        const Position end = ToPosition(document.text, diagnostic.offset + diagnostic.length);
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
        const Position start = ToPosition(document.text, diagnostic.offset);
        const Position end = ToPosition(document.text, diagnostic.offset + diagnostic.length);
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
    for (const auto& diagnostic : parse_tree.Diagnostics())
    {
        if (!first) message += ',';
        first = false;
        const Position start = ToPosition(document.text, diagnostic.offset);
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
    auto& document = m_documents[std::string(uri)];
    document = { std::string(text), version };
    m_parse_cache.erase(std::string(uri));
    PublishDiagnostics(uri, document);
}

void LanguageServer::ChangeDocument(simdjson::dom::element request)
{
    std::string_view uri;
    simdjson::dom::object text_document;
    if (!DocumentParams(request, uri, text_document)) return;
    const auto found = m_documents.find(std::string(uri));
    if (found == m_documents.end()) return;
    simdjson::dom::object params;
    if (!GetObject(request, "params", params)) return;
    simdjson::dom::array changes;
    if (params["contentChanges"].get_array().get(changes)) return;
    std::string_view text;
    for (simdjson::dom::element change : changes)
    {
        if (change["text"].get_string().get(text)) continue;
        found->second.text.assign(text);
    }
    if (const auto error = text_document["version"].get_int64().get(found->second.version); error)
    {
        found->second.version = 0;
    }
    m_parse_cache.erase(found->first);
    PublishDiagnostics(uri, found->second);
}

void LanguageServer::CloseDocument(simdjson::dom::element request)
{
    std::string_view uri;
    simdjson::dom::object text_document;
    if (!DocumentParams(request, uri, text_document)) return;
    m_documents.erase(std::string(uri));
    m_include_cache.erase(std::string(uri));
    m_parse_cache.erase(std::string(uri));
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
    const auto found = m_documents.find(std::string(uri));
    if (found == m_documents.end())
    {
        Respond(id, "[]");
        return;
    }
    const std::string formatted = heimdall::Formatter().Format(found->second.text);
    if (formatted == found->second.text)
    {
        Respond(id, "[]");
        return;
    }
    std::string response = "[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":";
    AppendPosition(ToPosition(found->second.text, found->second.text.size()), response);
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
    const auto found = m_documents.find(std::string(uri));
    if (found == m_documents.end())
    {
        Respond(id, "[]");
        return;
    }
    const auto diagnostics = heimdall::RuleEngine().Analyze(found->second.text);
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
        QuoteJson(uri, response);
        response += ":[{\"range\":{\"start\":";
        AppendPosition(ToPosition(found->second.text, diagnostic.fix.offset), response);
        response += ",\"end\":";
        AppendPosition(ToPosition(found->second.text, diagnostic.fix.offset + diagnostic.fix.length), response);
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
    const auto found = m_documents.find(std::string(uri));
    if (found == m_documents.end())
    {
        Respond(id, "{\"isIncomplete\":false,\"items\":[]}");
        return;
    }
    simdjson::dom::object position;
    if (!GetObject(params, "position", position))
    {
        Respond(id, "{\"isIncomplete\":false,\"items\":[]}");
        return;
    }
    const Position cursor = { static_cast<std::size_t>(PositionNumber(position, "line")),
                              static_cast<std::size_t>(PositionNumber(position, "character")) };
    const std::string& text = found->second.text;
    const std::size_t offset = OffsetFromPosition(text, cursor);

    const auto* command = m_compile_database ? m_compile_database->Find(PathFromUri(uri)) : nullptr;
    const heimdall::ParserOptions parser_options = ParserOptionsFor(command);
    const std::string uri_string(uri);
    const heimdall::ScopeIndex* external = HeaderScopes(uri, text, command);
    const auto& tree = CachedParse(uri_string, found->second, command);
    const auto items = heimdall::CompletionEngine::Complete(tree, parser_options, offset, external);
    const std::string prefix = heimdall::CompletionEngine::PrefixAt(text, offset);
    const Position start = ToPosition(text, offset - prefix.size());
    const Position end = ToPosition(text, offset);

    std::string response = "{\"isIncomplete\":false,\"items\":[";
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

const heimdall::ScopeIndex* LanguageServer::HeaderScopes(std::string_view uri, const std::string& text,
                                                        const heimdall::CompileCommand* command)
{
    // Two-level header cache. The fingerprint covers only the file's own
    // `#include` block plus search flags: repeat keystrokes hit it with zero
    // disk I/O (previously every completion/hover re-lexed every transitive
    // header from disk just to compute the cache key). A fingerprint miss
    // re-resolves, then re-stats the resolved set; only a real change rebuilds.
    // Built indexes are shared globally by content key, so the same <vector>
    // is parsed once across all open documents (was: once per document).
    const std::string uri_string(uri);
    const std::filesystem::path file_path = PathFromUri(uri);
    const std::filesystem::path base_dir =
        file_path.has_parent_path() ? file_path.parent_path() : std::filesystem::path();
    const std::string fingerprint = heimdall::IncludeIndex::IncludeFingerprint(base_dir, text, command);

    auto& entry = m_include_cache[uri_string];
    if (entry.fingerprint != fingerprint)
    {
        entry.fingerprint = fingerprint;
        entry.headers = heimdall::IncludeIndex::ResolveHeaders(base_dir, text, command);
        entry.index_key.clear(); // force CacheKey recomputation below
    }
    const std::string index_key = heimdall::IncludeIndex::CacheKey(entry.headers, command);
    if (entry.index_key != index_key)
    {
        entry.index_key = index_key;
        auto global = m_global_indices.find(index_key);
        if (global == m_global_indices.end())
        {
            global = m_global_indices.emplace(index_key, heimdall::IncludeIndex::Build(entry.headers,
                                                                                       command)).first;
        }
        return &global->second.Scopes();
    }
    // Fast path: return the shared index without touching the disk.
    if (const auto global = m_global_indices.find(entry.index_key); global != m_global_indices.end())
    {
        return &global->second.Scopes();
    }
    // The global entry was evicted (never happens today: no eviction) or the
    // cache started empty: (re)build under the content key.
    auto global = m_global_indices.emplace(entry.index_key, heimdall::IncludeIndex::Build(entry.headers,
                                                                                           command)).first;
    return &global->second.Scopes();
}

heimdall::ParserOptions LanguageServer::ParserOptionsFor(const heimdall::CompileCommand* command)
{
    heimdall::ParserOptions options;
    if (command != nullptr)
    {
        options.standard = command->standard;
        options.predefined_macros = command->defines;
    }
    return options;
}

const heimdall::ParseTree& LanguageServer::CachedParse(const std::string& uri, const Document& document,
                                                        const heimdall::CompileCommand* command)
{
    auto& entry = m_parse_cache[uri];
    // The tree borrows the document buffer: reuse only when the version
    // matches and the buffer is still the one the tree was parsed from.
    if (entry.version == document.version && entry.tree.Source().data() == document.text.data() &&
        entry.tree.Source().size() == document.text.size())
    {
        return entry.tree;
    }
    entry.version = document.version;
    entry.options = ParserOptionsFor(command);
    entry.tree = heimdall::ParseTree::Parse(document.text, entry.options);
    return entry.tree;
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
    const auto found = m_documents.find(std::string(uri));
    if (found == m_documents.end())
    {
        Respond(id, "null");
        return;
    }
    simdjson::dom::object position;
    if (!GetObject(params, "position", position))
    {
        Respond(id, "null");
        return;
    }
    const Position cursor = { static_cast<std::size_t>(PositionNumber(position, "line")),
                              static_cast<std::size_t>(PositionNumber(position, "character")) };
    const std::string& text = found->second.text;
    const std::size_t offset = OffsetFromPosition(text, cursor);

    const auto* command = m_compile_database ? m_compile_database->Find(PathFromUri(uri)) : nullptr;
    const heimdall::ParserOptions parser_options = ParserOptionsFor(command);
    const std::string uri_string(uri);
    const auto& tree = CachedParse(uri_string, found->second, command);
    const auto hovered = heimdall::CompletionEngine::Hover(tree, parser_options, offset,
                                                           HeaderScopes(uri, text, command));
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

} // namespace heimdall::lsp
