#include <CppLsp/Parser.hpp>

#include <string_view>
#include <vector>

namespace cpplsp
{

class GrammarParser
{
  public:
    explicit GrammarParser(ParseTree& tree) : m_tree(tree)
    {
        for (std::size_t i = 0; i < tree.m_tokens.size(); ++i)
        {
            const auto& token = tree.m_tokens[i];
            if (token.kind != TokenKind::Whitespace && token.kind != TokenKind::LineComment &&
                token.kind != TokenKind::BlockComment)
                m_sig.push_back(i);
        }
        m_match.resize(m_sig.size(), Invalid);
        std::vector<std::size_t> stack;
        for (std::size_t i = 0; i < m_sig.size(); ++i)
        {
            const auto t = Text(i);
            if (t == "(" || t == "[" || t == "{") stack.push_back(i);
            else if (t == ")" || t == "]" || t == "}")
            {
                if (!stack.empty() && Closes(Text(stack.back()), t))
                {
                    m_match[i] = stack.back();
                    m_match[stack.back()] = i;
                    stack.pop_back();
                }
            }
        }
    }

    void Run()
    {
        m_tree.m_nodes.push_back({ GrammarKind::TranslationUnit, 0, m_tree.m_tokens.size(), ParseTree::RootNode });
        ParseScope(0, m_sig.size(), ParseTree::RootNode, false);
    }

  private:
    static constexpr std::size_t Invalid = static_cast<std::size_t>(-1);
    ParseTree& m_tree;
    std::vector<std::size_t> m_sig;
    std::vector<std::size_t> m_match;
    std::size_t m_last_expression_node = Invalid;

    std::string_view Text(std::size_t sig) const
    {
        if (sig >= m_sig.size()) return {};
        const auto& token = m_tree.m_tokens[m_sig[sig]];
        return m_tree.m_source.substr(token.offset, token.length);
    }

    static bool Closes(std::string_view open, std::string_view close)
    {
        return (open == "(" && close == ")") || (open == "[" && close == "]") ||
               (open == "{" && close == "}");
    }

    std::size_t Add(GrammarKind kind, std::size_t begin, std::size_t end, std::size_t parent)
    {
        const std::size_t first = begin < m_sig.size() ? m_sig[begin] : m_tree.m_tokens.size();
        const std::size_t past = end > begin && end - 1 < m_sig.size() ? m_sig[end - 1] + 1 : first;
        m_tree.m_nodes.push_back({ kind, first, past - first, parent });
        return m_tree.m_nodes.size() - 1;
    }

    bool Is(std::size_t i, std::string_view text) const { return Text(i) == text; }

    void SetNodeRange(std::size_t node, std::size_t begin, std::size_t end)
    {
        auto& record = m_tree.m_nodes[node];
        record.first_token = begin < m_sig.size() ? m_sig[begin] : m_tree.m_tokens.size();
        const auto past = end > begin && end - 1 < m_sig.size() ? m_sig[end - 1] + 1 : record.first_token;
        record.token_count = past - record.first_token;
    }

    bool IsDirective(std::size_t sig) const
    {
        if (sig >= m_sig.size() || (!Is(sig, "#") && !Is(sig, "%:"))) return false;
        const auto offset = m_tree.m_tokens[m_sig[sig]].offset;
        const auto line_start = offset == 0 ? 0 : m_tree.m_source.rfind('\n', offset - 1) == std::string_view::npos
                                                     ? 0
                                                     : m_tree.m_source.rfind('\n', offset - 1) + 1;
        for (auto i = line_start; i < offset; ++i)
            if (m_tree.m_source[i] != ' ' && m_tree.m_source[i] != '\t' && m_tree.m_source[i] != '\r') return false;
        return true;
    }

    std::size_t SkipDirective(std::size_t sig, std::size_t end, std::size_t parent)
    {
        const auto start = sig;
        const auto offset = m_tree.m_tokens[m_sig[sig]].offset;
        auto line_end = m_tree.m_source.find('\n', offset);
        if (line_end == std::string_view::npos) line_end = m_tree.m_source.size();
        else ++line_end;
        while (sig < end && m_tree.m_tokens[m_sig[sig]].offset < line_end) ++sig;
        Add(GrammarKind::PreprocessorDirective, start, sig, parent);
        return sig;
    }

    std::size_t SkipGroup(std::size_t i, std::size_t end) const
    {
        if (i < end && m_match[i] != Invalid && m_match[i] > i) return m_match[i] + 1;
        return i + 1;
    }

    std::size_t FindComma(std::size_t begin, std::size_t end) const
    {
        std::size_t angle_depth = 0;
        for (auto i = begin; i < end;)
        {
            if ((Is(i, "(") || Is(i, "[") || Is(i, "{")) && m_match[i] != Invalid && m_match[i] > i)
            {
                i = m_match[i] + 1;
                continue;
            }
            if (Is(i, "<") && i > begin &&
                (m_tree.m_tokens[m_sig[i - 1]].kind == TokenKind::Identifier || Is(i - 1, ">") || Is(i - 1, ">>")))
                ++angle_depth;
            else if (Is(i, ">") && angle_depth != 0) --angle_depth;
            else if (Is(i, ">>") && angle_depth != 0) angle_depth = angle_depth > 1 ? angle_depth - 2 : 0;
            else if (Is(i, ",") && angle_depth == 0) return i;
            ++i;
        }
        return end;
    }

    std::size_t FindTemplateClose(std::size_t open, std::size_t end) const
    {
        if (!Is(open, "<")) return Invalid;
        std::size_t depth = 1;
        for (auto i = open + 1; i < end;)
        {
            if ((Is(i, "(") || Is(i, "[") || Is(i, "{")) && m_match[i] != Invalid && m_match[i] > i)
            {
                i = m_match[i] + 1;
                continue;
            }
            if (Is(i, "<")) ++depth;
            else if (Is(i, ">"))
            {
                if (--depth == 0) return i;
            }
            else if (Is(i, ">>"))
            {
                if (depth <= 2) return i;
                depth -= 2;
            }
            else if ((Is(i, ";") || Is(i, "=")) && depth == 1) return Invalid;
            ++i;
        }
        return Invalid;
    }

    std::size_t FindSemicolon(std::size_t begin, std::size_t end) const
    {
        for (auto i = begin; i < end;)
        {
            if (Is(i, ";")) return i;
            if ((Is(i, "(") || Is(i, "[") || Is(i, "{")) && m_match[i] != Invalid && m_match[i] > i)
                i = m_match[i] + 1;
            else ++i;
        }
        return end;
    }

    std::size_t TopLevelAssignment(std::size_t begin, std::size_t end) const
    {
        for (auto i = begin; i < end;)
        {
            if (Is(i, "=") || Is(i, "{")) return i;
            if ((Is(i, "(") || Is(i, "[") || Is(i, "{")) && m_match[i] != Invalid && m_match[i] > i)
                i = m_match[i] + 1;
            else ++i;
        }
        return end;
    }

    std::size_t FindDeclaredName(std::size_t begin, std::size_t end) const
    {
        std::size_t candidate = Invalid;
        for (auto i = begin; i < end;)
        {
            if ((Is(i, "[") || Is(i, "{") || Is(i, "(")) && m_match[i] != Invalid && m_match[i] > i)
            {
                i = m_match[i] + 1;
                continue;
            }
            if (m_tree.m_tokens[m_sig[i]].kind == TokenKind::Identifier) candidate = i;
            ++i;
        }
        return candidate;
    }

    void AddTypeAndDeclarator(std::size_t begin, std::size_t end, std::size_t parent, bool include_type = true)
    {
        const auto initializer = TopLevelAssignment(begin, end);
        const auto declarator_end = initializer < end ? initializer : end;
        const auto name = FindDeclaredName(begin, declarator_end);
        if (name == Invalid)
        {
            if (include_type && begin < end) Add(GrammarKind::TypeSpecifier, begin, end, parent);
            return;
        }
        if (include_type && begin < name) Add(GrammarKind::TypeSpecifier, begin, name, parent);
        const auto declarator = Add(GrammarKind::Declarator, name, declarator_end, parent);
        Add(GrammarKind::DeclaredName, name, name + 1, declarator);
        if (initializer < end && Is(initializer, "="))
            ParseExpression(initializer + 1, end, declarator);
        else if (initializer < end && Is(initializer, "{"))
        {
            const auto close = m_match[initializer];
            if (close != Invalid && close < end) ParseExpression(initializer + 1, close, declarator);
        }
    }

    void AddDeclarationDetails(std::size_t begin, std::size_t end, std::size_t parent, bool parameter = false)
    {
        // C++ declarations share one specifier sequence across comma-separated
        // declarators; keep that prefix explicit instead of duplicating it.
        const auto first_comma = FindComma(begin, end);
        const auto first_initializer = TopLevelAssignment(begin, first_comma);
        const auto shared_name = FindDeclaredName(begin, first_initializer < first_comma ? first_initializer : first_comma);
        if (shared_name != Invalid && begin < shared_name)
            Add(GrammarKind::TypeSpecifier, begin, shared_name, parent);
        auto part = begin;
        while (part < end)
        {
            const auto comma = FindComma(part, end);
            const auto declarator = Add(GrammarKind::InitDeclarator, part, comma, parent);
            AddTypeAndDeclarator(part, comma, declarator, false);
            if (comma == end) break;
            part = comma + 1;
        }
        (void)parameter;
    }

    void AddFunctionParameters(std::size_t begin, std::size_t brace, std::size_t parent)
    {
        std::size_t open = Invalid;
        for (auto i = begin; i < brace; ++i)
        {
            if (Is(i, "(") && m_match[i] != Invalid && m_match[i] < brace) open = i;
        }
        if (open == Invalid) return;
        const auto close = m_match[open];
        auto part = open + 1;
        while (part < close)
        {
            const auto comma = FindComma(part, close);
            if (comma > part && !(comma == part + 1 && Is(part, "...")))
            {
                const auto parameter = Add(GrammarKind::ParameterDeclaration, part, comma, parent);
                AddTypeAndDeclarator(part, comma, parameter);
            }
            if (comma == close) break;
            part = comma + 1;
        }
    }

    GrammarKind StatementKind(std::size_t begin, std::size_t end) const
    {
        if (begin >= end) return GrammarKind::Error;
        if (Is(begin, "return") || Is(begin, "co_return")) return GrammarKind::ReturnStatement;
        if (Is(begin, "if")) return GrammarKind::IfStatement;
        if (Is(begin, "while") || Is(begin, "for") || Is(begin, "do")) return GrammarKind::LoopStatement;
        if (Is(begin, "switch")) return GrammarKind::SwitchStatement;
        if (Is(begin, "break") || Is(begin, "continue") || Is(begin, "goto") || Is(begin, "throw") ||
            Is(begin, "co_yield")) return GrammarKind::JumpStatement;
        constexpr std::string_view type_words[] = { "auto", "bool", "char", "char8_t", "char16_t", "char32_t",
            "double", "float", "int", "long", "short", "signed", "unsigned", "void", "wchar_t",
            "const", "constexpr", "static", "struct", "class", "enum", "typename", "using" };
        for (const auto word : type_words)
            if (Text(begin) == word) return GrammarKind::DeclarationStatement;
        if (m_tree.m_tokens[m_sig[begin]].kind == TokenKind::Identifier)
        {
            auto i = begin + 1;
            while (i + 1 < end && Is(i, "::") && m_tree.m_tokens[m_sig[i + 1]].kind == TokenKind::Identifier)
                i += 2;
            if (Is(i, "<"))
            {
                std::size_t depth = 0;
                do
                {
                    if (Is(i, "<")) ++depth;
                    else if (Is(i, ">") && depth != 0) --depth;
                    else if (Is(i, ">>") && depth != 0) depth = depth > 1 ? depth - 2 : 0;
                    ++i;
                } while (i < end && depth != 0);
            }
            while (Is(i, "*") || Is(i, "&") || Is(i, "&&") || Is(i, "const")) ++i;
            if (i < end && m_tree.m_tokens[m_sig[i]].kind == TokenKind::Identifier)
                return GrammarKind::DeclarationStatement;
        }
        return GrammarKind::ExpressionStatement;
    }

    static int Precedence(std::string_view op)
    {
        if (op == ",") return 1;
        if (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=" || op == "%=" ||
            op == "&=" || op == "|=" || op == "^=" || op == "<<=" || op == ">>=") return 2;
        if (op == "?") return 3;
        if (op == "||") return 4;
        if (op == "&&") return 5;
        if (op == "|") return 6;
        if (op == "^") return 7;
        if (op == "&") return 8;
        if (op == "==" || op == "!=") return 9;
        if (op == "<" || op == ">" || op == "<=" || op == ">=" || op == "<=>") return 10;
        if (op == "<<" || op == ">>") return 11;
        if (op == "+" || op == "-") return 12;
        if (op == "*" || op == "/" || op == "%") return 13;
        return 0;
    }

    std::size_t ParseExpression(std::size_t pos, std::size_t end, std::size_t parent, int minimum = 1)
    {
        if (pos >= end) { m_last_expression_node = Invalid; return pos; }
        const auto begin = pos;
        GrammarKind prefix_kind = GrammarKind::ErrorExpression;
        std::size_t root = Invalid;
        const auto first = Text(pos);
        if (first == "+" || first == "-" || first == "!" || first == "~" || first == "*" || first == "&" ||
            first == "++" || first == "--" || first == "co_await")
        {
            root = Add(GrammarKind::UnaryExpression, begin, begin + 1, parent);
            ++pos;
            pos = ParseExpression(pos, end, root, 14);
            SetNodeRange(root, begin, pos);
            prefix_kind = GrammarKind::UnaryExpression;
        }
        else if (first == "(" && m_match[pos] != Invalid && m_match[pos] < end)
        {
            const auto close = m_match[pos];
            const auto node = Add(GrammarKind::ParenthesizedExpression, pos, close + 1, parent);
            auto inner = pos + 1;
            ParseExpression(inner, close, node);
            pos = close + 1;
            root = node;
            prefix_kind = GrammarKind::ParenthesizedExpression;
            (void)node;
        }
        else if (first == "[" && m_match[pos] != Invalid && m_match[pos] < end)
        {
            const auto capture_close = m_match[pos];
            auto body = capture_close + 1;
            if (Is(body, "(")) body = SkipGroup(body, end);
            while (body < end && !Is(body, "{") && !Is(body, ";")) ++body;
            if (Is(body, "{") && m_match[body] != Invalid)
            {
                const auto lambda = Add(GrammarKind::LambdaExpression, pos, m_match[body] + 1, parent);
                auto body_pos = body;
                ParseCompound(body_pos, m_match[body] + 1, lambda);
                pos = m_match[body] + 1;
                root = lambda;
                prefix_kind = GrammarKind::LambdaExpression;
            }
            else
            {
                root = Add(GrammarKind::ErrorExpression, pos, capture_close + 1, parent);
                pos = capture_close + 1;
                prefix_kind = GrammarKind::ErrorExpression;
            }
        }
        else if (m_tree.m_tokens[m_sig[pos]].kind == TokenKind::Identifier)
        {
            root = Add(GrammarKind::IdentifierExpression, pos, pos + 1, parent);
            ++pos;
            prefix_kind = GrammarKind::IdentifierExpression;
        }
        else if (m_tree.m_tokens[m_sig[pos]].kind == TokenKind::Number ||
                 m_tree.m_tokens[m_sig[pos]].kind == TokenKind::StringLiteral ||
                 m_tree.m_tokens[m_sig[pos]].kind == TokenKind::CharacterLiteral ||
                 m_tree.m_tokens[m_sig[pos]].kind == TokenKind::RawStringLiteral || first == "true" || first == "false" ||
                 first == "nullptr")
        {
            root = Add(GrammarKind::LiteralExpression, pos, pos + 1, parent);
            ++pos;
            prefix_kind = GrammarKind::LiteralExpression;
        }
        else if (first == "{" && m_match[pos] != Invalid && m_match[pos] < end)
        {
            root = Add(GrammarKind::LiteralExpression, pos, m_match[pos] + 1, parent);
            pos = m_match[pos] + 1;
            prefix_kind = GrammarKind::LiteralExpression;
        }
        else
        {
            m_tree.m_diagnostics.push_back({ m_tree.m_tokens[m_sig[pos]].offset, "expected expression" });
            root = Add(GrammarKind::ErrorExpression, pos, pos + 1, parent);
            m_last_expression_node = root;
            return pos + 1;
        }

        while (pos < end)
        {
            const auto op = Text(pos);
            if ((op == "(" || op == "[") && m_match[pos] != Invalid && m_match[pos] < end)
            {
                const auto close = m_match[pos];
                const auto node_kind = op == "(" ? GrammarKind::CallExpression : GrammarKind::SubscriptExpression;
                const auto node = Add(node_kind, begin, close + 1, parent);
                if (root != Invalid) m_tree.m_nodes[root].parent = node;
                if (op == "(")
                {
                    auto arg = pos + 1;
                    while (arg < close)
                    {
                        const auto next = FindComma(arg, close);
                        if (arg < next) ParseExpression(arg, next, node);
                        arg = next + 1;
                    }
                }
                else ParseExpression(pos + 1, close, node);
                pos = close + 1;
                root = node;
                continue;
            }
            if (op == "." || op == "->" || op == "::")
            {
                if (pos + 1 >= end) break;
                const auto node = Add(GrammarKind::MemberExpression, begin, pos + 2, parent);
                if (root != Invalid) m_tree.m_nodes[root].parent = node;
                Add(GrammarKind::IdentifierExpression, pos + 1, pos + 2, node);
                pos += 2;
                root = node;
                continue;
            }
            if (op == "<" && root != Invalid &&
                (m_tree.m_nodes[root].kind == GrammarKind::IdentifierExpression ||
                 m_tree.m_nodes[root].kind == GrammarKind::MemberExpression ||
                 m_tree.m_nodes[root].kind == GrammarKind::TemplateIdExpression))
            {
                const auto close = FindTemplateClose(pos, end);
                const auto after_template = close == Invalid ? end : close + 1;
                if (close != Invalid &&
                    (after_template == end || Is(after_template, "(") || Is(after_template, "{") ||
                     Is(after_template, "::") || Is(after_template, ",") || Is(after_template, ">") ||
                     Is(after_template, ">>") || m_tree.m_tokens[m_sig[after_template]].kind == TokenKind::Identifier))
                {
                    const auto node = Add(GrammarKind::TemplateIdExpression, begin, close + 1, parent);
                    m_tree.m_nodes[root].parent = node;
                    auto arg = pos + 1;
                    while (arg < close)
                    {
                        const auto comma = FindComma(arg, close);
                        if (comma == arg)
                        {
                            m_tree.m_diagnostics.push_back({ m_tree.m_tokens[m_sig[arg]].offset,
                                                             "expected template argument" });
                            ++arg;
                            continue;
                        }
                        const auto argument = Add(GrammarKind::TemplateArgument, arg, comma, node);
                        if (arg < comma) ParseExpression(arg, comma, argument);
                        if (comma == close) break;
                        arg = comma + 1;
                    }
                    pos = close + 1;
                    root = node;
                    continue;
                }
            }
            if (op == "++" || op == "--")
            {
                const auto node = Add(GrammarKind::UnaryExpression, begin, pos + 1, parent);
                if (root != Invalid) m_tree.m_nodes[root].parent = node;
                ++pos;
                root = node;
                continue;
            }
            if (op == "?" && minimum <= 3)
            {
                const auto node = Add(GrammarKind::ConditionalExpression, begin, end, parent);
                if (root != Invalid) m_tree.m_nodes[root].parent = node;
                ++pos;
                pos = ParseExpression(pos, end, node);
                if (Is(pos, ":")) ++pos;
                pos = ParseExpression(pos, end, node, 2);
                SetNodeRange(node, begin, pos);
                root = node;
                continue;
            }
            const int precedence = Precedence(op);
            if (precedence < minimum || precedence == 0) break;
            const auto node = Add(GrammarKind::BinaryExpression, begin, end, parent);
            if (root != Invalid) m_tree.m_nodes[root].parent = node;
            ++pos;
            const bool right_associative = precedence == 2;
            pos = ParseExpression(pos, end, node, precedence + (right_associative ? 0 : 1));
            SetNodeRange(node, begin, pos);
            root = node;
        }
        m_last_expression_node = root;
        return pos;
    }

    void ParseStatement(std::size_t& pos, std::size_t end, std::size_t parent)
    {
        const auto start = pos;
        if (Is(pos, "}")) return;
        if (Is(pos, ";"))
        {
            Add(GrammarKind::EmptyStatement, pos, pos + 1, parent);
            ++pos;
            return;
        }
        if (Is(pos, "{"))
        {
            ParseCompound(pos, end, parent);
            return;
        }
        const auto keyword = Text(pos);
        if (keyword == "case" || keyword == "default")
        {
            auto colon = pos + 1;
            while (colon < end && !Is(colon, ":") && !Is(colon, "}"))
            {
                if ((Is(colon, "(") || Is(colon, "[") || Is(colon, "{")) && m_match[colon] != Invalid)
                    colon = m_match[colon] + 1;
                else ++colon;
            }
            if (Is(colon, ":")) ++colon;
            else m_tree.m_diagnostics.push_back({ m_tree.m_tokens[m_sig[pos]].offset, "expected ':' after case label" });
            Add(GrammarKind::CaseLabel, pos, colon, parent);
            pos = colon;
            return;
        }
        if (keyword == "do")
        {
            const auto node = Add(GrammarKind::DoStatement, pos, pos + 1, parent);
            ++pos;
            if (pos < end) ParseStatement(pos, end, node);
            if (Is(pos, "while"))
            {
                ++pos;
                if (Is(pos, "(") && m_match[pos] != Invalid) pos = SkipGroup(pos, end);
                if (Is(pos, ";")) ++pos;
            }
            else m_tree.m_diagnostics.push_back({ m_tree.m_tokens[m_sig[start]].offset,
                                                  "expected while after do statement" });
            const auto past = pos > start ? m_sig[pos - 1] + 1 : m_tree.m_nodes[node].first_token;
            m_tree.m_nodes[node].token_count = past - m_tree.m_nodes[node].first_token;
            return;
        }
        if (keyword == "try")
        {
            const auto node = Add(GrammarKind::TryStatement, pos, pos + 1, parent);
            ++pos;
            if (pos < end && Is(pos, "{")) ParseStatement(pos, end, node);
            while (Is(pos, "catch"))
            {
                const auto catch_start = pos++;
                if (Is(pos, "(") && m_match[pos] != Invalid) pos = SkipGroup(pos, end);
                if (pos < end && Is(pos, "{")) ParseStatement(pos, end, node);
                else m_tree.m_diagnostics.push_back({ m_tree.m_tokens[m_sig[catch_start]].offset,
                                                      "expected compound statement after catch clause" });
            }
            const auto past = pos > start ? m_sig[pos - 1] + 1 : m_tree.m_nodes[node].first_token;
            m_tree.m_nodes[node].token_count = past - m_tree.m_nodes[node].first_token;
            return;
        }
        const bool control = keyword == "if" || keyword == "while" || keyword == "for" ||
                             keyword == "switch";
        if (control)
        {
            const auto kind = keyword == "if" ? GrammarKind::IfStatement :
                              keyword == "switch" ? GrammarKind::SwitchStatement : GrammarKind::LoopStatement;
            const auto node = Add(kind, start, start + 1, parent);
            ++pos;
            if (Is(pos, "(") && m_match[pos] != Invalid)
            {
                const auto close = m_match[pos];
                if (keyword == "for")
                {
                    const auto first_sep = FindSemicolon(pos + 1, close);
                    if (first_sep < close)
                    {
                        const auto second_sep = FindSemicolon(first_sep + 1, close);
                        if (StatementKind(pos + 1, first_sep) == GrammarKind::DeclarationStatement)
                        {
                            const auto init = Add(GrammarKind::DeclarationStatement, pos + 1, first_sep, node);
                            AddDeclarationDetails(pos + 1, first_sep, init);
                        }
                        else if (first_sep > pos + 1) ParseExpression(pos + 1, first_sep, node);
                        if (second_sep < close && second_sep > first_sep + 1)
                            ParseExpression(first_sep + 1, second_sep, node);
                        if (second_sep < close && second_sep + 1 < close)
                            ParseExpression(second_sep + 1, close, node);
                    }
                    else
                    {
                        auto colon = pos + 1;
                        while (colon < close && !Is(colon, ":"))
                        {
                            if ((Is(colon, "(") || Is(colon, "[") || Is(colon, "{")) && m_match[colon] != Invalid)
                                colon = m_match[colon] + 1;
                            else ++colon;
                        }
                        if (colon < close)
                        {
                            const auto init = Add(GrammarKind::DeclarationStatement, pos + 1, colon, node);
                            AddDeclarationDetails(pos + 1, colon, init);
                            if (colon + 1 < close) ParseExpression(colon + 1, close, node);
                        }
                        else ParseExpression(pos + 1, close, node);
                    }
                }
                else ParseExpression(pos + 1, close, node);
                pos = close + 1;
            }
            if (pos < end && !Is(pos, "}")) ParseStatement(pos, end, node);
            if (keyword == "if" && Is(pos, "else"))
            {
                ++pos;
                if (pos < end && !Is(pos, "}")) ParseStatement(pos, end, node);
            }
            const auto past = pos > start ? m_sig[pos - 1] + 1 : m_tree.m_nodes[node].first_token;
            m_tree.m_nodes[node].token_count = past - m_tree.m_nodes[node].first_token;
            return;
        }
        const auto semicolon = FindSemicolon(pos, end);
        if (semicolon < end)
        {
            if (StatementKind(start, semicolon + 1) == GrammarKind::DeclarationStatement)
            {
                for (auto i = start + 1; i < semicolon; ++i)
                {
                    if ((Is(i, "(") || Is(i, "[") || Is(i, "{")) && m_match[i] != Invalid && m_match[i] > i)
                    {
                        i = m_match[i];
                        continue;
                    }
                    if (Is(i, "return") || Is(i, "co_return"))
                    {
                        m_tree.m_diagnostics.push_back({ m_tree.m_tokens[m_sig[i]].offset,
                                                         "expected ';' before return statement" });
                        Add(GrammarKind::Error, start, i, parent);
                        pos = i;
                        return;
                    }
                }
            }
            pos = semicolon + 1;
            const auto kind = StatementKind(start, pos);
            const auto node = Add(kind, start, pos, parent);
            auto expression_begin = start;
            if (kind == GrammarKind::ReturnStatement) ++expression_begin;
            if (expression_begin < semicolon &&
                (kind == GrammarKind::ReturnStatement || kind == GrammarKind::ExpressionStatement))
                ParseExpression(expression_begin, semicolon, node);
            if (kind == GrammarKind::DeclarationStatement)
                AddDeclarationDetails(start, semicolon, node);
            return;
        }
        if (Is(pos, "}")) return;
        m_tree.m_diagnostics.push_back({ m_tree.m_tokens[m_sig[start]].offset,
                                         "expected ';' before end of compound statement" });
        while (pos < end && !Is(pos, "}") && !Is(pos, ";")) ++pos;
        if (Is(pos, ";")) ++pos;
        if (pos == start) ++pos;
        Add(GrammarKind::Error, start, pos, parent);
    }

    std::size_t ParseCompound(std::size_t& pos, std::size_t end, std::size_t parent)
    {
        const auto start = pos++;
        const auto node = Add(GrammarKind::CompoundStatement, start, start + 1, parent);
        while (pos < end && !Is(pos, "}"))
        {
            const auto before = pos;
            if (IsDirective(pos)) pos = SkipDirective(pos, end, node);
            else ParseStatement(pos, end, node);
            if (pos == before) ++pos;
        }
        if (pos < end && Is(pos, "}")) ++pos;
        else m_tree.m_diagnostics.push_back({ m_tree.m_tokens[m_sig[start]].offset, "expected '}' to close compound statement" });
        auto& record = m_tree.m_nodes[node];
        const auto past = pos > start && pos - 1 < m_sig.size() ? m_sig[pos - 1] + 1 : record.first_token;
        record.token_count = past - record.first_token;
        return node;
    }

    void ParseScope(std::size_t begin, std::size_t end, std::size_t parent, bool member_scope)
    {
        auto pos = begin;
        while (pos < end)
        {
            const auto start = pos;
            if (IsDirective(pos))
            {
                pos = SkipDirective(pos, end, parent);
                continue;
            }
            auto declaration_start = pos;
            const bool is_template = Is(pos, "template");
            if (is_template)
            {
                auto angle = pos + 1;
                while (angle < end && !Is(angle, "<")) ++angle;
                if (angle == end)
                {
                    m_tree.m_diagnostics.push_back({ m_tree.m_tokens[m_sig[pos]].offset,
                                                     "expected template parameter list" });
                    Add(GrammarKind::Error, pos, end, parent);
                    break;
                }
                std::size_t depth = 0;
                for (; angle < end; ++angle)
                {
                    if (Is(angle, "<")) ++depth;
                    else if (Is(angle, ">") && depth != 0)
                    {
                        if (--depth == 0) { ++angle; break; }
                    }
                    else if (Is(angle, ">>") && depth != 0)
                    {
                        depth = depth > 1 ? depth - 2 : 0;
                        if (depth == 0) { ++angle; break; }
                    }
                }
                declaration_start = angle;
                if (declaration_start >= end)
                {
                    m_tree.m_diagnostics.push_back({ m_tree.m_tokens[m_sig[start]].offset,
                                                     "expected declaration after template parameter list" });
                    Add(GrammarKind::Error, start, end, parent);
                    break;
                }
            }
            const bool namespace_decl = Is(declaration_start, "namespace");
            const bool record_decl = Is(declaration_start, "class") || Is(declaration_start, "struct") ||
                                     Is(declaration_start, "union") || Is(declaration_start, "enum");
            std::size_t brace = end;
            for (auto i = declaration_start; i < end;)
            {
                if (Is(i, ";")) break;
                if (Is(i, "{")) { brace = i; break; }
                if ((Is(i, "(") || Is(i, "[") ) && m_match[i] != Invalid && m_match[i] > i)
                    i = m_match[i] + 1;
                else ++i;
            }
            if (brace < end)
            {
                const bool closed = m_match[brace] != Invalid;
                const auto close = closed ? m_match[brace] : end;
                bool has_function_parens = false;
                for (auto i = declaration_start; i < brace; ++i)
                    if (Is(i, "(") && m_match[i] != Invalid && m_match[i] < brace) has_function_parens = true;
                const bool function_body = has_function_parens && !namespace_decl && !record_decl;
                if (!namespace_decl && !record_decl && !function_body)
                {
                    const auto semi = FindSemicolon(brace + (closed ? 1 : 0), end);
                    if (semi < end)
                    {
                        const auto wrapper = is_template ? Add(GrammarKind::TemplateDeclaration, start, semi + 1, parent)
                                                         : parent;
                        const auto declaration = Add(GrammarKind::Declaration, declaration_start, semi + 1, wrapper);
                        AddDeclarationDetails(declaration_start, semi, declaration);
                        pos = semi + 1;
                        continue;
                    }
                }
                const auto node_kind = namespace_decl ? GrammarKind::NamespaceDefinition :
                                       record_decl ? GrammarKind::RecordDefinition :
                                       function_body ? GrammarKind::FunctionDefinition : GrammarKind::Error;
                const auto item_end = closed ? close + 1 : close;
                const auto item_parent = is_template ? Add(GrammarKind::TemplateDeclaration, start, item_end, parent) : parent;
                const auto node = Add(node_kind, declaration_start, item_end, item_parent);
                if (node_kind == GrammarKind::FunctionDefinition) AddFunctionParameters(declaration_start, brace, node);
                if (node_kind == GrammarKind::NamespaceDefinition || node_kind == GrammarKind::RecordDefinition)
                {
                    ParseScope(brace + 1, close, node, node_kind == GrammarKind::RecordDefinition);
                    if (!closed)
                        m_tree.m_diagnostics.push_back({ m_tree.m_tokens[m_sig[brace]].offset,
                                                         "expected '}' to close definition" });
                }
                else if (node_kind == GrammarKind::FunctionDefinition)
                {
                    auto body = brace;
                    ParseCompound(body, closed ? close + 1 : close, node);
                }
                pos = closed ? close + 1 : end;
                if (Is(pos, ";")) ++pos;
                continue;
            }
            const auto semi = FindSemicolon(pos, end);
            if (semi < end)
            {
                const auto item_parent = is_template ? Add(GrammarKind::TemplateDeclaration, start, semi + 1, parent) : parent;
                bool function_declaration = false;
                for (auto i = declaration_start; i < semi; ++i)
                    if (Is(i, "(") && m_match[i] != Invalid && m_match[i] < semi) function_declaration = true;
                if (function_declaration)
                {
                    const auto function = Add(GrammarKind::FunctionDeclaration, declaration_start, semi + 1, item_parent);
                    AddFunctionParameters(declaration_start, semi, function);
                }
                else
                {
                    const auto declaration = Add(GrammarKind::Declaration, declaration_start, semi + 1, item_parent);
                    AddDeclarationDetails(declaration_start, semi, declaration);
                }
                pos = semi + 1;
                continue;
            }
            m_tree.m_diagnostics.push_back({ m_tree.m_tokens[m_sig[start]].offset,
                                             "expected ';' or definition body before end of scope" });
            Add(GrammarKind::Error, start, end, parent);
            break;
        }
    }
};

ParseTree ParseTree::Parse(std::string_view source, CppStandard standard)
{
    ParseTree tree;
    tree.m_source = source;
    tree.m_standard = standard;
    tree.m_tokens = Lexer(source).Lex();
    GrammarParser parser(tree);
    parser.Run();
    return tree;
}

std::vector<std::size_t> ParseTree::Children(std::size_t node_index) const
{
    std::vector<std::size_t> children;
    for (std::size_t i = 1; i < m_nodes.size(); ++i)
        if (m_nodes[i].parent == node_index) children.push_back(i);
    return children;
}

} // namespace cpplsp
