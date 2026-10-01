#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace cpplsp
{

enum class TokenKind : std::uint8_t
{
    Whitespace,
    Identifier,
    Number,
    StringLiteral,
    CharacterLiteral,
    RawStringLiteral,
    LineComment,
    BlockComment,
    Punctuation,
    Unknown
};

// Token positions are byte offsets into the original source; no text is copied.
struct Token
{
    TokenKind kind;
    std::size_t offset;
    std::size_t length;
};

// Lossless lexer: every source byte belongs to exactly one token, including
// whitespace and comments. Unterminated literals/comments become tokens up to
// EOF, allowing callers to continue analysis on malformed files.
class Lexer
{
  public:
    explicit Lexer(std::string_view source) : m_source(source) {}

    std::vector<Token> Lex() const;
    std::string_view Text(const Token& token) const noexcept;

  private:
    std::string_view m_source;
};

} // namespace cpplsp
