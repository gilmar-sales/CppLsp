#include <gtest/gtest.h>

#include <CppLsp/Parser.hpp>

#include <string_view>

namespace
{

std::size_t Count(const cpplsp::ParseTree& tree, cpplsp::GrammarKind kind)
{
    std::size_t count = 0;
    for (const auto& node : tree.Nodes()) count += node.kind == kind;
    return count;
}

} // namespace

TEST(ParserSpec, ParsesTranslationUnitDefinitionsAndCompoundStatements)
{
    constexpr std::string_view source =
        "namespace demo {\n"
        "struct Item { int value; };\n"
        "int run(int x) { int y = x; if (y) return y; else return 0; }\n"
        "}\n";
    const auto tree = cpplsp::ParseTree::Parse(source, cpplsp::CppStandard::Cpp23);
    EXPECT_EQ(tree.Standard(), cpplsp::CppStandard::Cpp23);
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::NamespaceDefinition), 1);
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::RecordDefinition), 1);
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::FunctionDefinition), 1);
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::CompoundStatement), 1);
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::IfStatement), 1);
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::ReturnStatement), 2);
}

TEST(ParserSpec, RecoversAtSemicolonAndContinuesParsing)
{
    constexpr std::string_view source = "int f() { int broken return 1; return 2; } int g;";
    const auto tree = cpplsp::ParseTree::Parse(source);
    EXPECT_FALSE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::Error), 1);
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::ReturnStatement), 2);
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::Declaration), 1);
}

TEST(ParserSpec, ReportsUnclosedFunctionBodyAndKeepsPartialTree)
{
    const auto tree = cpplsp::ParseTree::Parse("int f() { return 1;");
    EXPECT_FALSE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::FunctionDefinition), 1);
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::CompoundStatement), 1);
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::ReturnStatement), 1);
}

TEST(ParserSpec, BuildsExpressionNodesForPrecedenceAndPostfixForms)
{
    const auto tree = cpplsp::ParseTree::Parse("int f() { return call(value + 2 * other[0]); }");
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::ReturnStatement), 1);
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::CallExpression), 1);
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::SubscriptExpression), 1);
    EXPECT_GE(Count(tree, cpplsp::GrammarKind::BinaryExpression), 2);
}

TEST(ParserSpec, ParsesParametersAndDeclaratorsWithInitializers)
{
    const auto tree = cpplsp::ParseTree::Parse(
        "int sum(int left, int right = 2) { int first = left, second{right}; return first + second; }");
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::ParameterDeclaration), 2);
    EXPECT_GE(Count(tree, cpplsp::GrammarKind::InitDeclarator), 2);
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::DeclaredName), 4);
    EXPECT_GE(Count(tree, cpplsp::GrammarKind::BinaryExpression), 1);
}

TEST(ParserSpec, KeepsTemplateArgumentCommasInsideParameterAndDeclaratorTypes)
{
    const auto tree = cpplsp::ParseTree::Parse(
        "template<class T, class U> struct Pair {};\n"
        "Pair<int, long> combine(Pair<int, long> left, Pair<char, short> right) {\n"
        "  Pair<int, long> first{}, second{}; return first;\n"
        "}");
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::ParameterDeclaration), 2);
    EXPECT_GE(Count(tree, cpplsp::GrammarKind::InitDeclarator), 2);
}

TEST(ParserSpec, ParsesLambdasAndCommonCompleteStatementForms)
{
    const auto tree = cpplsp::ParseTree::Parse(
        "int f(int value) {\n"
        "  auto fn = [value](int x) { return value + x; };\n"
        "  for (int i = 0; i < value; ++i) { if (i) continue; }\n"
        "  do { --value; } while (value);\n"
        "  switch (value) { case 0: break; default: return fn(1); }\n"
        "  try { return fn(value); } catch (...) { return 0; }\n"
        "}");
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::LambdaExpression), 1);
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::DoStatement), 1);
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::CaseLabel), 2);
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::TryStatement), 1);
}

TEST(ParserSpec, KeepsPreprocessorDirectiveBodiesOpaque)
{
    constexpr std::string_view source =
        "#include <vector>\n"
        "#define DECLARE(name) int name() { return 1; }\n"
        "int value() {\n"
        "#if FEATURE\n"
        "  return 1;\n"
        "#else\n"
        "  return 2;\n"
        "#endif\n"
        "}\n";
    const auto tree = cpplsp::ParseTree::Parse(source);
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::PreprocessorDirective), 5);
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::FunctionDefinition), 1);
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::ReturnStatement), 2);
}

TEST(ParserSpec, MaintainsValidParentLinksAndContainedTokenRanges)
{
    constexpr std::string_view source =
        "template<class T, class U> struct Pair { T first; U second; };\n"
        "int f(Pair<int, long> p) { return p.first + p.second * 2; }\n";
    const auto tree = cpplsp::ParseTree::Parse(source);
    ASSERT_FALSE(tree.Nodes().empty());
    EXPECT_EQ(tree.Nodes()[cpplsp::ParseTree::RootNode].kind, cpplsp::GrammarKind::TranslationUnit);
    for (std::size_t i = 1; i < tree.Nodes().size(); ++i)
    {
        const auto& node = tree.Nodes()[i];
        ASSERT_LT(node.parent, tree.Nodes().size());
        const auto& parent = tree.Nodes()[node.parent];
        EXPECT_LE(parent.first_token, node.first_token);
        EXPECT_LE(node.first_token + node.token_count, parent.first_token + parent.token_count);
    }
}

TEST(ParserSpec, ParsesNestedTemplateIdsAsPostfixExpressionsNotComparisons)
{
    const auto tree = cpplsp::ParseTree::Parse(
        "int f() { return choose<std::pair<int, long>, std::vector<char>>(make<int>(), value); }");
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_GE(Count(tree, cpplsp::GrammarKind::TemplateIdExpression), 3);
    EXPECT_GE(Count(tree, cpplsp::GrammarKind::TemplateArgument), 5);
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::CallExpression), 2);
}

TEST(ParserSpec, ParsesFunctionDeclarationsAndClassMemberPrototypes)
{
    const auto tree = cpplsp::ParseTree::Parse(
        "int transform(const Widget& input, int scale = 1);\n"
        "struct Widget {\n"
        "  Widget();\n"
        "  int value() const;\n"
        "  int data;\n"
        "};\n");
    EXPECT_TRUE(tree.Diagnostics().empty());
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::FunctionDeclaration), 3);
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::ParameterDeclaration), 2);
    EXPECT_EQ(Count(tree, cpplsp::GrammarKind::DeclaredName), 3);
}
