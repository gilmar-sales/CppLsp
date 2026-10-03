#pragma once

#include "Document.hpp"

#include <Heimdall/CompileDatabase.hpp>
#include <Heimdall/IncludeIndex.hpp>

#include <optional>
#include <simdjson.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace heimdall::lsp
{

  /* test*/
class LanguageServer
{
  public:
    bool Run();

  private:
    void LoadInitializationOptions(simdjson::dom::element request);
    void Respond(std::string_view id, std::string_view result);
    void PublishDiagnostics(std::string_view uri, const Document& document);
    bool DocumentParams(simdjson::dom::element request, std::string_view& uri,
                        simdjson::dom::object& document);
    void OpenDocument(simdjson::dom::element request);
    void ChangeDocument(simdjson::dom::element request);
    void CloseDocument(simdjson::dom::element request);
    void FormatDocument(simdjson::dom::element request, std::string_view id);
    void CodeActions(simdjson::dom::element request, std::string_view id);
    void CompleteDocument(simdjson::dom::element request, std::string_view id);
    void HoverDocument(simdjson::dom::element request, std::string_view id);
    const heimdall::ScopeIndex* HeaderScopes(std::string_view uri, const std::string& text,
                                            const heimdall::CompileCommand* command);

    std::unordered_map<std::string, Document> m_documents;
    std::optional<heimdall::CompileDatabase> m_compile_database;
    // Header index per open document, keyed by resolved headers + flags so it
    // rebuilds only when the include set changes (not on every keystroke).
    std::unordered_map<std::string, std::pair<std::string, heimdall::IncludeIndex>> m_include_indices;
    bool m_enable_semantic = false;
    std::string m_initialization_error;
};

} // namespace heimdall::lsp
