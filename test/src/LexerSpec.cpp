#include <gtest/gtest.h>

#include <CppLsp/Lexer.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace
{

std::string Reconstruct(std::string_view source, const std::vector<cpplsp::Token>& tokens)
{
    std::string out;
    for (const auto& token : tokens)
    {
        out.append(source.substr(token.offset, token.length));
    }
    return out;
}

} // namespace

TEST(LexerSpec, RoundTripsEmptyAndOrdinarySourceByteExactly)
{
    for (const std::string_view source : { "", "int x = 42;\r\n", "//comment\n/* block */\t" })
    {
        const cpplsp::Lexer lexer(source);
        const auto tokens = lexer.Lex();
        EXPECT_EQ(Reconstruct(source, tokens), source);
        std::size_t next = 0;
        for (const auto& token : tokens)
        {
            EXPECT_EQ(token.offset, next);
            EXPECT_GT(token.length, 0);
            next += token.length;
        }
        EXPECT_EQ(next, source.size());
    }
}

TEST(LexerSpec, RecognizesTriviaCommentsAndLiteralForms)
{
    constexpr std::string_view source = "  name // line\n/* block */ \"text\" 'c' R\"tag(raw)tag\" 123 0xAB";
    const cpplsp::Lexer lexer(source);
    const auto tokens = lexer.Lex();
    EXPECT_EQ(Reconstruct(source, tokens), source);

    bool whitespace = false;
    bool identifier = false;
    bool line_comment = false;
    bool block_comment = false;
    bool string_literal = false;
    bool character_literal = false;
    bool raw_string = false;
    bool number = false;
    for (const auto& token : tokens)
    {
        whitespace |= token.kind == cpplsp::TokenKind::Whitespace;
        identifier |= token.kind == cpplsp::TokenKind::Identifier;
        line_comment |= token.kind == cpplsp::TokenKind::LineComment;
        block_comment |= token.kind == cpplsp::TokenKind::BlockComment;
        string_literal |= token.kind == cpplsp::TokenKind::StringLiteral;
        character_literal |= token.kind == cpplsp::TokenKind::CharacterLiteral;
        raw_string |= token.kind == cpplsp::TokenKind::RawStringLiteral;
        number |= token.kind == cpplsp::TokenKind::Number;
        EXPECT_EQ(lexer.Text(token), source.substr(token.offset, token.length));
    }
    EXPECT_TRUE(whitespace);
    EXPECT_TRUE(identifier);
    EXPECT_TRUE(line_comment);
    EXPECT_TRUE(block_comment);
    EXPECT_TRUE(string_literal);
    EXPECT_TRUE(character_literal);
    EXPECT_TRUE(raw_string);
    EXPECT_TRUE(number);
}

TEST(LexerSpec, UnterminatedConstructsConsumeToEndWithoutLosingBytes)
{
    for (const std::string_view source : { "\"unterminated", "/* unterminated", "R\"x(raw" })
    {
        const cpplsp::Lexer lexer(source);
        const auto tokens = lexer.Lex();
        EXPECT_EQ(Reconstruct(source, tokens), source);
        ASSERT_FALSE(tokens.empty());
        EXPECT_EQ(tokens.back().offset + tokens.back().length, source.size());
    }
}

TEST(LexerSpec, RoundTripsEveryBenchmarkCorpusFile)
{
    const auto corpus = std::filesystem::path(CPPLSP_SOURCE_DIR) / "bench" / "corpus";
    ASSERT_TRUE(std::filesystem::exists(corpus));
    for (const auto& entry : std::filesystem::directory_iterator(corpus))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        std::ifstream file(entry.path(), std::ios::binary);
        const std::string source(std::istreambuf_iterator<char>(file), {});
        const cpplsp::Lexer lexer(source);
        EXPECT_EQ(Reconstruct(source, lexer.Lex()), source) << entry.path().string();
    }
}
