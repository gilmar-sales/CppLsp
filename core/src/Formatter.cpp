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

bool IsTrivia(TokenKind kind)
{
    return kind == TokenKind::Whitespace || kind == TokenKind::LineComment ||
           kind == TokenKind::BlockComment;
}

bool IsBrace(std::string_view source, const Token& token, char brace)
{
    return token.kind == TokenKind::Punctuation && token.length == 1 &&
           source[token.offset] == brace;
}

bool IsDirectiveLine(std::size_t line_start, const std::vector<PreprocessorDirective>& directives,
                     std::size_t& directive_cursor)
{
    while (directive_cursor < directives.size() &&
           directives[directive_cursor].offset + directives[directive_cursor].length <= line_start)
    {
        ++directive_cursor;
    }
    // The line is a directive line only when it starts inside the directive
    // span (was: `directive.offset < line_start + directive.length`, which
    // also matched plain code lines sitting just before the next directive
    // and copied whole regions verbatim, freezing brace depth).
    return directive_cursor < directives.size() && directives[directive_cursor].offset <= line_start &&
           line_start < directives[directive_cursor].offset + directives[directive_cursor].length;
}

} // namespace

std::string Formatter::Format(std::string_view source) const
{
    const auto tokens = Lexer(source).Lex();
    const auto directives = Preprocessor().Process(source).directives;
    return FormatImpl(source, tokens, directives);
}

std::string Formatter::Format(const ParseTree& tree) const
{
    return FormatImpl(tree.Source(), tree.Tokens(), tree.Directives());
}

std::string Formatter::FormatImpl(std::string_view source, const std::vector<Token>& tokens,
                                  const std::vector<PreprocessorDirective>& directives) const
{
    std::string output;
    output.reserve(source.size() + source.size() / 8);

    std::size_t token_cursor = 0;
    std::size_t directive_cursor = 0;
    std::size_t brace_depth = 0;
    std::size_t blank_run = 0;
    bool seen_content = false;
    // Depths of `{` braces opened by switch labels on their own line: their
    // matching `}` lines dedent to the case level without popping the depth.
    std::vector<std::size_t> label_brace_depths;
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
            blank_run = 0;
            seen_content = true;
        }
        else if (blank)
        {
            // Collapse runs of blank lines to max_empty_lines (leading blanks
            // are trimmed; trailing blanks are trimmed at the end).
            ++blank_run;
            if (seen_content && blank_run <= m_options.max_empty_lines)
            {
                if (line_end > content_end) output.append(source.substr(content_end, line_end - content_end));
                if (line_end < source.size()) output += '\n';
            }
        }
        else
        {
            // Scope-aware indent: every leading `}` dedents one level (so
            // `}}` closes two scopes), and scope labels (`public:`,
            // `case 1:`/`default:`, goto labels) dedent one level so the
            // scope contents read as nested. Only the line start matters;
            // braces inside trivia/literals never count as structure.
            std::size_t leading_closers = 0;
            bool is_scope_label = false;
            {
                std::size_t probe = first_significant;
                while (probe < tokens.size() && tokens[probe].offset < content_end)
                {
                    if (IsTrivia(tokens[probe].kind))
                    {
                        ++probe;
                        continue;
                    }
                    if (!IsBrace(source, tokens[probe], '}')) break;
                    ++leading_closers;
                    ++probe;
                }
                if (leading_closers == 0)
                {
                    // Significant token texts on this line (bounded lookahead).
                    std::string_view head[4];
                    TokenKind head_kind[4] = {};
                    std::size_t head_count = 0;
                    probe = first_significant;
                    while (probe < tokens.size() && tokens[probe].offset < content_end &&
                           head_count < 4)
                    {
                        if (IsTrivia(tokens[probe].kind))
                        {
                            ++probe;
                            continue;
                        }
                        head[head_count] =
                            source.substr(tokens[probe].offset, tokens[probe].length);
                        head_kind[head_count] = tokens[probe].kind;
                        ++head_count;
                        ++probe;
                    }
                    if (head_count >= 2)
                    {
                        const bool access_label =
                            (head[0] == "public" || head[0] == "private" || head[0] == "protected") &&
                            head[1] == ":";
                        const bool switch_label = head[0] == "case" || head[0] == "default";
                        // `name:` alone on a line is a goto label (a `:`
                        // inside a declaration never starts the line).
                        const bool goto_label = head_count == 2 && head_kind[0] == TokenKind::Identifier &&
                                                head[1] == ":";
                        is_scope_label = access_label || switch_label || goto_label;
                    }
                    else if (head_count == 1)
                    {
                        // `default:` split across lines is still a switch label.
                        is_scope_label = head[0] == "default";
                    }
                }
            }
            const std::size_t dedent = leading_closers + (is_scope_label ? 1 : 0);
            const std::size_t indent_depth = brace_depth > dedent ? brace_depth - dedent : 0;
            // A `{` opened by a switch label on the same line (`case 2: {`)
            // belongs to the case level: it must not push the body one level
            // deeper. The brace is skipped here and its match is skipped when
            // closed (tracked as a depth stack, since labels can nest).
            std::size_t label_open_token = static_cast<std::size_t>(-1);
            if (is_scope_label)
            {
                std::size_t last_colon = static_cast<std::size_t>(-1);
                std::size_t probe = first_significant;
                while (probe < tokens.size() && tokens[probe].offset < content_end)
                {
                    if (!IsTrivia(tokens[probe].kind) && tokens[probe].kind == TokenKind::Punctuation &&
                        tokens[probe].length == 1 && source[tokens[probe].offset] == ':')
                    {
                        last_colon = probe;
                    }
                    ++probe;
                }
                if (last_colon != static_cast<std::size_t>(-1))
                {
                    int balance = 0;
                    for (probe = last_colon + 1;
                         probe < tokens.size() && tokens[probe].offset < content_end; ++probe)
                    {
                        if (IsTrivia(tokens[probe].kind) ||
                            tokens[probe].kind != TokenKind::Punctuation || tokens[probe].length != 1)
                        {
                            continue;
                        }
                        if (source[tokens[probe].offset] == '{')
                        {
                            if (balance == 0 && label_open_token == static_cast<std::size_t>(-1))
                            {
                                label_open_token = probe;
                            }
                            ++balance;
                        }
                        else if (source[tokens[probe].offset] == '}')
                        {
                            --balance;
                        }
                    }
                    // Balanced (`case f({1}):`) or no brace: ordinary label.
                    if (balance == 0) label_open_token = static_cast<std::size_t>(-1);
                }
            }
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
            blank_run = 0;
            seen_content = true;

            while (token_cursor < tokens.size() && tokens[token_cursor].offset < line_end)
            {
                const Token& token = tokens[token_cursor];
                if (token.offset >= line_start && token.kind == TokenKind::Punctuation)
                {
                    if (token_cursor == label_open_token)
                    {
                        label_brace_depths.push_back(brace_depth);
                    }
                    else if (IsBrace(source, token, '{'))
                    {
                        ++brace_depth;
                    }
                    else if (IsBrace(source, token, '}') && brace_depth > 0)
                    {
                        if (!label_brace_depths.empty() && brace_depth == label_brace_depths.back())
                        {
                            label_brace_depths.pop_back();
                        }
                        else
                        {
                            --brace_depth;
                        }
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
    // Trim trailing blank lines: the file ends with at most its final
    // newline (a missing final newline is preserved as-is).
    std::size_t tail = output.size();
    while (tail > 0 && output[tail - 1] == '\n')
    {
        std::size_t blank_start = tail - 1;
        while (blank_start > 0 && output[blank_start - 1] != '\n') --blank_start;
        std::size_t blank_end = tail - 1;
        if (blank_end > blank_start && output[blank_end - 1] == '\r') --blank_end;
        if (blank_end != blank_start) break;
        tail = blank_start;
    }
    output.erase(tail);
    return output;
}

} // namespace heimdall
