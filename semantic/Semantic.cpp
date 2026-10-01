#include <CppLsp/Lexer.hpp>
#include <CppLsp/Semantic.hpp>

#include <array>
#include <string_view>

namespace cpplsp
{

namespace
{

bool IsWord(std::string_view value)
{
    if (value.empty()) return false;
    const char first = value.front();
    return (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '_';
}

std::vector<std::string_view> SignificantTokens(std::string_view source)
{
    std::vector<std::string_view> result;
    for (const auto& token : Lexer(source).Lex())
    {
        if (token.kind != TokenKind::Whitespace && token.kind != TokenKind::LineComment &&
            token.kind != TokenKind::BlockComment)
        {
            result.push_back(source.substr(token.offset, token.length));
        }
    }
    return result;
}

} // namespace

std::unordered_set<std::string> SemanticAnalyzer::CollectTypeNames(std::string_view source,
                                                                   const CompileCommand* command) const
{
    std::unordered_set<std::string> types = {
        "void", "bool", "char", "wchar_t", "char8_t", "char16_t", "char32_t", "short", "int", "long",
        "signed", "unsigned", "float", "double", "auto", "size_t", "std::size_t"
    };
    const auto tokens = SignificantTokens(source);
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i)
    {
        if (tokens[i] == "enum" && i + 2 < tokens.size() &&
            (tokens[i + 1] == "class" || tokens[i + 1] == "struct") && IsWord(tokens[i + 2]))
        {
            types.emplace(tokens[i + 2]);
        }
        else if ((tokens[i] == "class" || tokens[i] == "struct" || tokens[i] == "union" || tokens[i] == "enum") &&
                 IsWord(tokens[i + 1]))
        {
            types.emplace(tokens[i + 1]);
        }
        else if (tokens[i] == "using" && IsWord(tokens[i + 1]))
        {
            types.emplace(tokens[i + 1]);
        }
    }
    if (command != nullptr)
    {
        for (const auto& [name, value] : command->defines)
        {
            if (types.contains(value)) types.insert(name);
        }
    }
    return types;
}

AsteriskMeaning SemanticAnalyzer::ClassifyAsteriskStatement(
    std::string_view statement, const std::unordered_set<std::string>& known_types,
    const std::unordered_set<std::string>& known_values) const
{
    const auto tokens = SignificantTokens(statement);
    if (tokens.size() != 4 || !IsWord(tokens[0]) || tokens[1] != "*" || !IsWord(tokens[2]) || tokens[3] != ";")
    {
        return AsteriskMeaning::NotApplicable;
    }
    if (known_types.contains(std::string(tokens[0]))) return AsteriskMeaning::Declaration;
    if (known_values.contains(std::string(tokens[0]))) return AsteriskMeaning::Multiplication;
    return AsteriskMeaning::Ambiguous;
}

} // namespace cpplsp
