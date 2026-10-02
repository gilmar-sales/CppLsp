#include <gtest/gtest.h>

#include <CppLsp/Cst.hpp>

#include <string>

TEST(CstSpec, PreservesSourceAndBuildsNestedDelimiterGroups)
{
    constexpr std::string_view source = "int f(int x) { return values[x + (1)]; }\n";
    const auto tree = cpplsp::SyntaxTree::Parse(source);

    std::string reconstructed;
    for (const auto& token : tree.Tokens()) reconstructed.append(tree.Text(token));
    EXPECT_EQ(reconstructed, source);
    ASSERT_EQ(tree.Nodes().front().kind, cpplsp::SyntaxKind::TranslationUnit);
    EXPECT_EQ(tree.Nodes().front().token_count, tree.Tokens().size());
    EXPECT_TRUE(tree.Diagnostics().empty());

    std::size_t parens = 0;
    std::size_t brackets = 0;
    std::size_t braces = 0;
    for (const auto& node : tree.Nodes())
    {
        parens += node.kind == cpplsp::SyntaxKind::ParenthesizedGroup;
        brackets += node.kind == cpplsp::SyntaxKind::BracketedGroup;
        braces += node.kind == cpplsp::SyntaxKind::BracedGroup;
    }
    EXPECT_EQ(parens, 2);
    EXPECT_EQ(brackets, 1);
    EXPECT_EQ(braces, 1);
}

TEST(CstSpec, ReportsMismatchedAndUnclosedDelimitersWithoutAborting)
{
    constexpr std::string_view source = "([)] {";
    const auto tree = cpplsp::SyntaxTree::Parse(source);
    EXPECT_EQ(tree.Tokens().size(), 6);
    ASSERT_EQ(tree.Diagnostics().size(), 3);
    EXPECT_EQ(tree.Diagnostics()[0].message, "unmatched closing delimiter");
    EXPECT_EQ(tree.Diagnostics()[1].message, "unclosed delimiter");
    EXPECT_EQ(tree.Diagnostics()[2].message, "unclosed delimiter");
    EXPECT_EQ(tree.Nodes().front().token_count, tree.Tokens().size());
}

TEST(CstSpec, EnforcesNestingLimitAndStillProducesTree)
{
    const auto tree = cpplsp::SyntaxTree::Parse("((((x))))", 2);
    EXPECT_FALSE(tree.Diagnostics().empty());
    EXPECT_EQ(tree.Nodes().front().token_count, tree.Tokens().size());
}

TEST(CstSpec, CarriesExplicitLanguageDialectWithoutChangingSourceSpans)
{
    constexpr std::string_view source = "int f() { return 0; }";
    for (const auto standard : { cpplsp::CppStandard::Cpp20, cpplsp::CppStandard::Cpp23,
                                 cpplsp::CppStandard::Cpp26 })
    {
        const auto tree = cpplsp::SyntaxTree::Parse(source, standard);
        EXPECT_EQ(tree.Standard(), standard);
        EXPECT_EQ(tree.Source(), source);
        EXPECT_TRUE(tree.Diagnostics().empty());
    }

    // Existing callers remain source-compatible and select the documented
    // default dialect until a compilation database provides one.
    EXPECT_EQ(cpplsp::SyntaxTree::Parse(source).Standard(), cpplsp::CppStandard::Cpp20);
}
