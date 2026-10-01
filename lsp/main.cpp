#include <CppLsp/Formatter.hpp>
#include <CppLsp/Rules.hpp>
#include <CppLsp/CompileCommands.hpp>
#include <CppLsp/Semantic.hpp>

#include <simdjson.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace
{

struct Position
{
    std::size_t line = 0;
    std::size_t character = 0;
};

struct Document
{
    std::string text;
    std::int64_t version = 0;
};

void QuoteJson(std::string_view value, std::string& out)
{
    constexpr char hex[] = "0123456789abcdef";
    out += '"';
    for (const unsigned char c : value)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
            {
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 0x0f];
            }
            else out += static_cast<char>(c);
        }
    }
    out += '"';
}

void Send(std::string_view body)
{
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n";
    std::cout.write(body.data(), static_cast<std::streamsize>(body.size()));
    std::cout.flush();
}

bool ReadMessage(std::string& body)
{
    std::string line;
    std::size_t length = 0;
    bool got_length = false;
    while (std::getline(std::cin, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        constexpr std::string_view prefix = "Content-Length:";
        if (line.starts_with(prefix))
        {
            const auto value = std::string_view(line).substr(prefix.size());
            try
            {
                length = static_cast<std::size_t>(std::stoull(std::string(value)));
                got_length = true;
            }
            catch (...)
            {
                return false;
            }
        }
    }
    if (!std::cin || !got_length) return false;
    body.resize(length);
    std::cin.read(body.data(), static_cast<std::streamsize>(length));
    return static_cast<std::size_t>(std::cin.gcount()) == length;
}

bool GetString(simdjson::dom::object object, const char* key, std::string_view& output)
{
    return !object[key].get_string().get(output);
}

bool GetObject(simdjson::dom::element element, const char* key, simdjson::dom::object& output)
{
    return !element[key].get_object().get(output);
}

Position ToPosition(std::string_view text, std::size_t offset)
{
    Position position;
    const std::size_t stop = offset < text.size() ? offset : text.size();
    for (std::size_t i = 0; i < stop;)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\n')
        {
            ++position.line;
            position.character = 0;
            ++i;
            continue;
        }
        if ((c & 0x80) == 0)
        {
            ++position.character;
            ++i;
        }
        else
        {
            std::size_t width = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
            std::uint32_t codepoint = c & (width == 2 ? 0x1f : width == 3 ? 0x0f : 0x07);
            for (std::size_t j = 1; j < width && i + j < stop; ++j)
            {
                codepoint = (codepoint << 6) | (static_cast<unsigned char>(text[i + j]) & 0x3f);
            }
            position.character += codepoint > 0xffff ? 2 : 1;
            i += width;
        }
    }
    return position;
}

std::filesystem::path PathFromUri(std::string_view uri)
{
    constexpr std::string_view prefix = "file://";
    if (uri.starts_with(prefix)) uri.remove_prefix(prefix.size());
    std::string decoded;
    decoded.reserve(uri.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < uri.size(); ++i)
    {
        if (uri[i] == '%' && i + 2 < uri.size() && hex(uri[i + 1]) >= 0 && hex(uri[i + 2]) >= 0)
        {
            decoded += static_cast<char>(hex(uri[i + 1]) * 16 + hex(uri[i + 2]));
            i += 2;
        }
        else decoded += uri[i];
    }
#if defined(_WIN32)
    if (decoded.size() >= 3 && decoded[0] == '/' && decoded[2] == ':') decoded.erase(decoded.begin());
    std::replace(decoded.begin(), decoded.end(), '/', '\\');
#endif
    return std::filesystem::path(decoded);
}

void AppendPosition(Position position, std::string& out)
{
    out += "{\"line\":" + std::to_string(position.line) + ",\"character\":" +
           std::to_string(position.character) + "}";
}

class LanguageServer
{
  public:
    bool Run()
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
                        "\"codeActionProvider\":true},\"serverInfo\":{\"name\":\"CppLsp\",\"version\":\"0.1.0\"}}");
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
            else if (has_id)
            {
                Respond(id_json, "null");
            }
        }
        return false;
    }

  private:
    void LoadInitializationOptions(simdjson::dom::element request)
    {
        simdjson::dom::object params;
        simdjson::dom::object options;
        if (!GetObject(request, "params", params) || !GetObject(params, "initializationOptions", options)) return;
        bool enabled = false;
        if (const auto error = options["enableSemantic"].get_bool().get(enabled); error) enabled = false;
        if (!enabled) return;

        std::string_view path;
        if (!GetString(options, "compileCommands", path) || path.empty()) return;
        auto database = cpplsp::CompileDatabase::Load(std::filesystem::path(path));
        if (!database)
        {
            m_initialization_error = database.error();
            return;
        }
        m_compile_database = std::move(*database);
    }

    void Respond(std::string_view id, std::string_view result)
    {
        Send("{\"jsonrpc\":\"2.0\",\"id\":" + std::string(id) + ",\"result\":" + std::string(result) + "}");
    }

    void PublishDiagnostics(std::string_view uri, const Document& document)
    {
        const auto diagnostics = cpplsp::RuleEngine().Analyze(document.text);
        std::vector<cpplsp::SemanticDiagnostic> semantic_diagnostics;
        if (m_compile_database)
        {
            const auto* command = m_compile_database->Find(PathFromUri(uri));
            if (command != nullptr)
            {
                semantic_diagnostics = cpplsp::SemanticAnalyzer().AnalyzeUnusedLocals(document.text, command);
            }
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
            message += ",\"source\":\"cpplsp\",\"message\":";
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
            message += ",\"source\":\"cpplsp\",\"message\":";
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

    bool DocumentParams(simdjson::dom::element request, std::string_view& uri, simdjson::dom::object& document)
    {
        simdjson::dom::object params;
        if (!GetObject(request, "params", params)) return false;
        if (!GetObject(params, "textDocument", document)) return false;
        return GetString(document, "uri", uri);
    }

    void OpenDocument(simdjson::dom::element request)
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

    void ChangeDocument(simdjson::dom::element request)
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

    void CloseDocument(simdjson::dom::element request)
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

    void FormatDocument(simdjson::dom::element request, std::string_view id)
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
        const std::string formatted = cpplsp::Formatter().Format(found->second.text);
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

    void CodeActions(simdjson::dom::element request, std::string_view id)
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
        const auto diagnostics = cpplsp::RuleEngine().Analyze(found->second.text);
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

    std::unordered_map<std::string, Document> m_documents;
    std::optional<cpplsp::CompileDatabase> m_compile_database;
    std::string m_initialization_error;
};

} // namespace

int main()
{
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    LanguageServer server;
    return server.Run() ? 0 : 1;
}
