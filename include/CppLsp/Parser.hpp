#pragma once

#include <CppLsp/Cst.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cpplsp
{

enum class GrammarKind : std::uint8_t
{
    TranslationUnit,
    PreprocessorDirective,
    Declaration,
    ParameterDeclaration,
    InitDeclarator,
    FunctionDefinition,
    FunctionDeclaration,
    NamespaceDefinition,
    RecordDefinition,
    CompoundStatement,
    DeclarationStatement,
    ExpressionStatement,
    ReturnStatement,
    IfStatement,
    LoopStatement,
    SwitchStatement,
    JumpStatement,
    EmptyStatement,
    IdentifierExpression,
    LiteralExpression,
    ParenthesizedExpression,
    UnaryExpression,
    BinaryExpression,
    ConditionalExpression,
    CallExpression,
    SubscriptExpression,
    MemberExpression,
    LambdaExpression,
    CaseLabel,
    TryStatement,
    DoStatement,
    TemplateDeclaration,
    TemplateArgument,
    TemplateIdExpression,
    TypeSpecifier,
    Declarator,
    DeclaredName,
    ErrorExpression,
    Error
};

struct GrammarNode
{
    GrammarKind kind;
    std::size_t first_token;
    std::size_t token_count;
    std::size_t parent;
};

struct GrammarDiagnostic
{
    std::size_t offset;
    std::string message;
};

// Initial recursive-descent grammar layer over the lossless lexer. It parses
// translation-unit items and compound statements while retaining token ranges
// for declaration/expression forms not yet covered by dedicated productions.
class ParseTree
{
  public:
    static constexpr std::size_t RootNode = 0;

    static ParseTree Parse(std::string_view source, CppStandard standard = CppStandard::Cpp20);

    std::string_view Source() const noexcept { return m_source; }
    CppStandard Standard() const noexcept { return m_standard; }
    const std::vector<Token>& Tokens() const noexcept { return m_tokens; }
    const std::vector<GrammarNode>& Nodes() const noexcept { return m_nodes; }
    const std::vector<GrammarDiagnostic>& Diagnostics() const noexcept { return m_diagnostics; }
    std::string_view Text(const Token& token) const noexcept { return m_source.substr(token.offset, token.length); }
    std::vector<std::size_t> Children(std::size_t node_index) const;

  private:
    friend class GrammarParser;
    std::string_view m_source;
    CppStandard m_standard = CppStandard::Cpp20;
    std::vector<Token> m_tokens;
    std::vector<GrammarNode> m_nodes;
    std::vector<GrammarDiagnostic> m_diagnostics;
};

} // namespace cpplsp
