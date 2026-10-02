#pragma once

#include <Heimdall/Lexer.hpp>
#include <Heimdall/Language.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace heimdall
{

enum class SyntaxKind : std::uint8_t
{
    TranslationUnit,
    ParenthesizedGroup,
    BracketedGroup,
    BracedGroup,
    Error
};

struct GreenNode
{
    SyntaxKind kind;
    std::size_t first_token;
    std::size_t token_count;
    std::size_t parent;
};

struct ParseDiagnostic
{
    std::size_t offset;
    std::string message;
};

// Lossless structural parse. The source must outlive this tree; tokens hold
// byte spans into it. Nodes are flat records linked by parent index, making
// construction iterative and safe for deeply malformed input.
class SyntaxTree
{
  public:
    static constexpr std::size_t RootNode = 0;

    static SyntaxTree Parse(std::string_view source, std::size_t max_nesting_depth = 512);
    static SyntaxTree Parse(std::string_view source, CppStandard standard,
                            std::size_t max_nesting_depth = 512);

    std::string_view Source() const noexcept { return m_source; }
    CppStandard Standard() const noexcept { return m_standard; }
    const std::vector<Token>& Tokens() const noexcept { return m_tokens; }
    const std::vector<GreenNode>& Nodes() const noexcept { return m_nodes; }
    const std::vector<std::size_t>& TokenParents() const noexcept { return m_token_parents; }
    const std::vector<ParseDiagnostic>& Diagnostics() const noexcept { return m_diagnostics; }
    std::string_view Text(const Token& token) const noexcept { return m_source.substr(token.offset, token.length); }

    std::vector<std::size_t> Children(std::size_t node_index) const;

  private:
    std::string_view m_source;
    CppStandard m_standard = CppStandard::Cpp20;
    std::vector<Token> m_tokens;
    std::vector<GreenNode> m_nodes;
    std::vector<std::size_t> m_token_parents;
    std::vector<ParseDiagnostic> m_diagnostics;
};

} // namespace heimdall
