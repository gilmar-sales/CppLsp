#pragma once

#include <Heimdall/ParseTree.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace heimdall
{

enum class CompletionKind : std::uint8_t
{
    Keyword,
    Type,
    Namespace,
    Function,
    Variable,
    Macro,
    Directive
};

struct CompletionItem
{
    std::string label;
    CompletionKind kind = CompletionKind::Keyword;
    // Short signature shown beside the label (`int add(int left, int right)`).
    std::string detail;
    // Doc comment shown in the side popup (sent as LSP markdown).
    std::string documentation;
};

// One named scope and its direct members. `path` is the qualified path from
// the translation unit (`{"std", "chrono"}`); an empty path denotes the
// global scope. `kind` describes the scope itself when listed as a member
// (Namespace for namespaces, Type for records). Backbone for header indexing:
// headers are parsed exactly like sources and their scopes merged by path.
struct IndexedScope
{
    std::vector<std::string> path;
    CompletionKind kind = CompletionKind::Type;
    std::vector<CompletionItem> members;
};

using ScopeIndex = std::vector<IndexedScope>;

// Lexical + local ParseTree completion engine (core, STL only).
// Works on incomplete code: ParseTree diagnostics are ignored, candidates
// are collected from lexer identifiers, DeclaredName nodes and macros,
// then filtered by the identifier prefix immediately before `offset`.
class CompletionEngine
{
  public:
    static std::string PrefixAt(std::string_view source, std::size_t offset);
    static std::vector<CompletionItem> Complete(std::string_view source, std::size_t offset);
    static std::vector<CompletionItem> Complete(std::string_view source, const ParserOptions& options,
                                               std::size_t offset);
    // External scopes (e.g. indexed headers) merged into lookup: qualified
    // `ns::` also matches external scopes, unqualified completion also offers
    // external global-scope members. May be nullptr for single-file lookup.
    static std::vector<CompletionItem> Complete(std::string_view source, const ParserOptions& options,
                                                std::size_t offset, const ScopeIndex* external);
    // ParseTree-backed overloads: the server caches one ParseTree per
    // (uri, version) and reuses it across diagnostics/completion/hover so a
    // keystroke pays for a single lex + preprocess + grammar pass. The tree
    // must have been parsed from a buffer that outlives the call.
    static std::vector<CompletionItem> Complete(const ParseTree& tree, const ParserOptions& options,
                                                std::size_t offset, const ScopeIndex* external = nullptr);
    // Structural index of one file: every namespace/record scope (plus the
    // global scope) with its direct members, merged by qualified path.
    static ScopeIndex IndexScopes(std::string_view source, const ParserOptions& options);
    // Symbol under the cursor for hover: the completion match for the whole
    // identifier around `offset`, with signature and documentation. Empty when
    // the cursor is not on a known name (whitespace, punctuation, keywords).
    static std::optional<CompletionItem> Hover(std::string_view source, const ParserOptions& options,
                                               std::size_t offset, const ScopeIndex* external = nullptr);
    static std::optional<CompletionItem> Hover(const ParseTree& tree, const ParserOptions& options,
                                               std::size_t offset, const ScopeIndex* external = nullptr);
};

} // namespace heimdall
