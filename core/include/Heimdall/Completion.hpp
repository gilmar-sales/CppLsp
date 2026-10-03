#pragma once

#include <Heimdall/ParseTree.hpp>

#include <cstddef>
#include <cstdint>
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
    std::string detail;
};

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
};

} // namespace heimdall
