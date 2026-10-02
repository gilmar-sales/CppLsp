#include <Heimdall/Formatter.hpp>

#include <Heimdall/Lexer.hpp>
#include <Heimdall/Preprocessor.hpp>

#include <algorithm>
#include <string_view>
#include <vector>

namespace heimdall
{

namespace
{

bool IsIndent(char c) { return c == ' ' || c == '\t'; }

bool IsDirectiveLine(std::size_t line_start, const std::vector<PreprocessorDirective>& directives,
                     std::size_t& directive_cursor)
{
    while (directive_cursor < directives.size() &&
           directives[directive_cursor].offset + directives[directive_cursor].length <= line_start)
    {
        ++directive_cursor;
    }
    return directive_cursor < directives.size() && directives[directive_cursor].offset >= line_start &&
           directives[directive_cursor].offset < line_start + directives[directive_cursor].length;
}

} // namespace

std::string Formatter::Format(std::string_view source) const
{
    const auto tokens = Lexer(source).Lex();
    const auto directives = Preprocessor().Process(source).directives;
    std::string output;
    output.reserve(source.size() + source.size() / 8);

    std::size_t token_cursor = 0;
    std::size_t directive_cursor = 0;
    std::size_t brace_depth = 0;
    std::size_t line_start = 0;
    while (line_start < source.size())
    {
        std::size_t line_end = source.find('\n', line_start);
        if (line_end == std::string_view::npos) line_end = source.size();
        const std::size_t content_end = line_end > line_start && source[line_end - 1] == '\r' ? line_end - 1 : line_end;

        std::size_t content_start = line_start;
        while (content_start < content_end && IsIndent(source[content_start])) ++content_start;
        const bool blank = content_start == content_end;
        const bool directive = IsDirectiveLine(line_start, directives, directive_cursor);

        std::size_t first_significant = token_cursor;
        while (first_significant < tokens.size() && tokens[first_significant].offset < content_start)
        {
            ++first_significant;
        }
        if (directive)
        {
            output.append(source.substr(line_start, line_end - line_start));
            if (line_end < source.size()) output += '\n';
        }
        else if (blank)
        {
            if (line_end > content_end) output.append(source.substr(content_end, line_end - content_end));
            if (line_end < source.size()) output += '\n';
        }
        else
        {
            std::size_t leading_closers = 0;
            std::size_t probe = first_significant;
            while (probe < tokens.size() && tokens[probe].offset < content_end)
            {
                if (tokens[probe].kind == TokenKind::Whitespace)
                {
                    ++probe;
                    continue;
                }
                const auto token_text = source.substr(tokens[probe].offset, tokens[probe].length);
                if (token_text == "}") ++leading_closers;
                break;
            }
            const std::size_t indent_depth = brace_depth > leading_closers ? brace_depth - leading_closers : 0;
            if (m_options.use_tabs)
            {
                output.append(indent_depth, '\t');
            }
            else
            {
                output.append(indent_depth * m_options.indent_width, ' ');
            }
            output.append(source.substr(content_start, line_end - content_start));
            if (line_end < source.size()) output += '\n';

            while (token_cursor < tokens.size() && tokens[token_cursor].offset < line_end)
            {
                const Token& token = tokens[token_cursor];
                if (token.offset >= line_start && token.kind == TokenKind::Punctuation)
                {
                    const auto token_text = source.substr(token.offset, token.length);
                    if (token_text == "{")
                    {
                        ++brace_depth;
                    }
                    else if (token_text == "}" && brace_depth > 0)
                    {
                        --brace_depth;
                    }
                }
                ++token_cursor;
            }
        }

        if (directive)
        {
            while (token_cursor < tokens.size() && tokens[token_cursor].offset < line_end) ++token_cursor;
        }
        line_start = line_end == source.size() ? source.size() : line_end + 1;
    }

    if (source.empty()) return {};
    return output;
}

} // namespace heimdall
