#include <gtest/gtest.h>

#include <Heimdall/Rules.hpp>

TEST(RulesSpec, FindsNullMacroOutsideCommentsLiteralsAndDirectives)
{
    constexpr std::string_view source =
        "#define TEXT NULL\n"
        "void f() { auto p = NULL; } // NULL\n"
        "const char* s = \"NULL\";\n";
    const auto diagnostics = heimdall::RuleEngine().Analyze(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].code, "HEIMDALL001");
    EXPECT_EQ(diagnostics[0].line, 2);
    EXPECT_EQ(diagnostics[0].column, 21);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics),
              "#define TEXT NULL\nvoid f() { auto p = nullptr; } // NULL\nconst char* s = \"NULL\";\n");
}

TEST(RulesSpec, FindsAndFixesTrailingSpacesAndTabsIncludingCrLf)
{
    constexpr std::string_view source = "first  \r\nsecond\t\nclean\n";
    const auto diagnostics = heimdall::RuleEngine().Analyze(source);
    ASSERT_EQ(diagnostics.size(), 2);
    EXPECT_EQ(diagnostics[0].code, "HEIMDALL002");
    EXPECT_EQ(diagnostics[0].line, 1);
    EXPECT_EQ(diagnostics[1].line, 2);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes(source, diagnostics), "first\r\nsecond\nclean\n");
}

TEST(RulesSpec, CanDisableRulesIndependently)
{
    const auto diagnostics = heimdall::RuleEngine({ .null_macro = false, .trailing_whitespace = true })
                                 .Analyze("auto p = NULL;  \n");
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::TrailingWhitespace);
}

TEST(RulesSpec, AddsMissingFinalNewlineAndPreservesLineEndingStyle)
{
    const auto lf = heimdall::RuleEngine().Analyze("int value;");
    ASSERT_EQ(lf.size(), 1);
    EXPECT_EQ(lf[0].code, "HEIMDALL003");
    EXPECT_TRUE(lf[0].has_fix);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes("int value;", lf), "int value;\n");

    const auto crlf = heimdall::RuleEngine().Analyze("int value;\r\nint other;");
    ASSERT_EQ(crlf.size(), 1);
    EXPECT_EQ(heimdall::RuleEngine::ApplyFixes("int value;\r\nint other;", crlf),
              "int value;\r\nint other;\r\n");
}

TEST(RulesSpec, DoesNotFlagEmptyFilesOrFilesAlreadyEndingInNewline)
{
    EXPECT_TRUE(heimdall::RuleEngine().Analyze("").empty());
    EXPECT_TRUE(heimdall::RuleEngine().Analyze("int value;\n").empty());
}
