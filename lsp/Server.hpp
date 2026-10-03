#pragma once

#include "Document.hpp"

#include <Heimdall/CompileDatabase.hpp>
#include <Heimdall/IncludeIndex.hpp>
#include <Heimdall/ParseTree.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <simdjson.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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
    // One ParseTree per (uri, version), shared by diagnostics/completion/hover
    // so a keystroke pays for a single lex + preprocess + grammar pass.
    const heimdall::ParseTree& CachedParse(const std::string& uri, const Document& document,
                                           const heimdall::CompileCommand* command);
    static heimdall::ParserOptions ParserOptionsFor(const heimdall::CompileCommand* command);

    std::unordered_map<std::string, Document> m_documents;
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
        std::string index_key;
    };
    std::unordered_map<std::string, IncludeCacheEntry> m_include_cache;
    std::unordered_map<std::string, heimdall::IncludeIndex> m_global_indices;
    struct ParseCacheEntry
    {
        std::int64_t version = -1;
        heimdall::ParserOptions options;
        heimdall::ParseTree tree;
    };
    std::unordered_map<std::string, ParseCacheEntry> m_parse_cache;
    bool m_enable_semantic = false;
    std::string m_initialization_error;
};

} // namespace heimdall::lsp
