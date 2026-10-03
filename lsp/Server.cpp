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
    while (ReadMessage(body))
    {
        simdjson::dom::parser parser;
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
                    "\"codeActionProvider\":true,"
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
    heimdall::ParserOptions parser_options;
    if (command != nullptr)
    {
        parser_options.standard = command->standard;
        parser_options.predefined_macros = command->defines;
    }
    const auto parse_tree = heimdall::ParseTree::Parse(document.text, parser_options);
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
    PublishDiagnostics(uri, found->second);
}

void LanguageServer::CloseDocument(simdjson::dom::element request)
{
    std::string_view uri;
    simdjson::dom::object text_document;
    if (!DocumentParams(request, uri, text_document)) return;
    m_documents.erase(std::string(uri));
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
    heimdall::ParserOptions parser_options;
    if (command != nullptr)
    {
        parser_options.standard = command->standard;
        parser_options.predefined_macros = command->defines;
    }
    const auto items = heimdall::CompletionEngine::Complete(text, parser_options, offset);
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

} // namespace heimdall::lsp
