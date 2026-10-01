#include <CppLsp/Cst.hpp>

#include <string_view>

namespace cpplsp
{

namespace
{

char ExpectedCloser(char opener)
{
    switch (opener)
    {
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    default: return '\0';
    }
}

SyntaxKind GroupKind(char opener)
{
    switch (opener)
    {
    case '(': return SyntaxKind::ParenthesizedGroup;
    case '[': return SyntaxKind::BracketedGroup;
    default: return SyntaxKind::BracedGroup;
    }
}

} // namespace

SyntaxTree SyntaxTree::Parse(std::string_view source, std::size_t max_nesting_depth)
{
    SyntaxTree tree;
    tree.m_source = source;
    tree.m_tokens = Lexer(source).Lex();
    tree.m_nodes.push_back({ SyntaxKind::TranslationUnit, 0, tree.m_tokens.size(), RootNode });
    tree.m_token_parents.resize(tree.m_tokens.size(), RootNode);

    std::vector<std::size_t> group_stack;
    group_stack.reserve(max_nesting_depth < 4096 ? max_nesting_depth : 4096);
    std::size_t current_parent = RootNode;

    for (std::size_t i = 0; i < tree.m_tokens.size(); ++i)
    {
        const Token& token = tree.m_tokens[i];
        const std::string_view text = tree.Text(token);
        if (token.kind != TokenKind::Punctuation || text.size() != 1)
        {
            tree.m_token_parents[i] = current_parent;
            continue;
        }

        const char c = text.front();
        if (ExpectedCloser(c) != '\0')
        {
            tree.m_token_parents[i] = current_parent;
            if (group_stack.size() >= max_nesting_depth)
            {
                tree.m_diagnostics.push_back({ token.offset, "maximum delimiter nesting depth exceeded" });
                continue;
            }
            const std::size_t node = tree.m_nodes.size();
            tree.m_nodes.push_back({ GroupKind(c), i, 1, current_parent });
            tree.m_token_parents[i] = node;
            group_stack.push_back(node);
            current_parent = node;
            continue;
        }

        if (c == ')' || c == ']' || c == '}')
        {
            if (!group_stack.empty())
            {
                const std::size_t node = group_stack.back();
                const Token& opener = tree.m_tokens[tree.m_nodes[node].first_token];
                const char expected = ExpectedCloser(tree.Text(opener).front());
                if (c == expected)
                {
                    tree.m_token_parents[i] = node;
                    tree.m_nodes[node].token_count = i - tree.m_nodes[node].first_token + 1;
                    group_stack.pop_back();
                    current_parent = tree.m_nodes[node].parent;
                    continue;
                }
            }

            tree.m_token_parents[i] = current_parent;
            tree.m_nodes.push_back({ SyntaxKind::Error, i, 1, current_parent });
            tree.m_diagnostics.push_back({ token.offset, "unmatched closing delimiter" });
            continue;
        }

        tree.m_token_parents[i] = current_parent;
    }

    for (const std::size_t node : group_stack)
    {
        const std::size_t first = tree.m_nodes[node].first_token;
        tree.m_nodes[node].token_count = tree.m_tokens.size() - first;
        tree.m_diagnostics.push_back({ tree.m_tokens[first].offset, "unclosed delimiter" });
    }

    return tree;
}

std::vector<std::size_t> SyntaxTree::Children(std::size_t node_index) const
{
    std::vector<std::size_t> result;
    for (std::size_t i = 1; i < m_nodes.size(); ++i)
    {
        if (m_nodes[i].parent == node_index)
        {
            result.push_back(i);
        }
    }
    return result;
}

} // namespace cpplsp
