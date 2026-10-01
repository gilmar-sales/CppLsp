#include <gtest/gtest.h>

#include <CppLsp/Rules.hpp>

TEST(RulesSpec, FindsNullMacroOutsideCommentsLiteralsAndDirectives)
{
    constexpr std::string_view source =
        "#define TEXT NULL\n"
        "void f() { auto p = NULL; } // NULL\n"
        "const char* s = \"NULL\";\n";
    const auto diagnostics = cpplsp::RuleEngine().Analyze(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].code, "CPPLSP001");
    EXPECT_EQ(diagnostics[0].line, 2);
    EXPECT_EQ(diagnostics[0].column, 21);
    EXPECT_EQ(cpplsp::RuleEngine::ApplyFixes(source, diagnostics),
              "#define TEXT NULL\nvoid f() { auto p = nullptr; } // NULL\nconst char* s = \"NULL\";\n");
}

TEST(RulesSpec, FindsAndFixesTrailingSpacesAndTabsIncludingCrLf)
{
    constexpr std::string_view source = "first  \r\nsecond\t\nclean\n";
    const auto diagnostics = cpplsp::RuleEngine().Analyze(source);
    ASSERT_EQ(diagnostics.size(), 2);
    EXPECT_EQ(diagnostics[0].code, "CPPLSP002");
    EXPECT_EQ(diagnostics[0].line, 1);
    EXPECT_EQ(diagnostics[1].line, 2);
    EXPECT_EQ(cpplsp::RuleEngine::ApplyFixes(source, diagnostics), "first\r\nsecond\nclean\n");
}

TEST(RulesSpec, CanDisableRulesIndependently)
{
    const auto diagnostics = cpplsp::RuleEngine({ .null_macro = false, .trailing_whitespace = true })
                                 .Analyze("auto p = NULL;  \n");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].rule, cpplsp::RuleId::TrailingWhitespace);
}
