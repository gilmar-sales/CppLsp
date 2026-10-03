#include <Heimdall/Lexer.hpp>
#include <Heimdall/ParseTree.hpp>
#include <Heimdall/Preprocessor.hpp>

#include "detail/GrammarParser.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace heimdall
{

ParseTree ParseTree::Parse(std::string_view source, CppStandard standard)
{
    ParserOptions options;
    options.standard = standard;
    return Parse(source, options);
}

ParseTree ParseTree::Parse(std::string_view source, const ParserOptions& options)
{
    ParseTree tree;
    tree.m_source = source;
    tree.m_standard = options.standard;
    tree.m_tokens = Lexer(source).Lex();
    const auto preprocessing = Preprocessor(options.predefined_macros).Process(source);
    detail::ParseWithGrammar(tree, preprocessing);
    return tree;
}

std::vector<std::size_t> ParseTree::Children(std::size_t node_index) const
{
    std::vector<std::size_t> children;
    for (std::size_t i = 1; i < m_nodes.size(); ++i)
        if (m_nodes[i].parent == node_index) children.push_back(i);
    return children;
}

} // namespace heimdall
