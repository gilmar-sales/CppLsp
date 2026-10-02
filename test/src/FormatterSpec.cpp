#include <gtest/gtest.h>

#include <Heimdall/Formatter.hpp>

TEST(FormatterSpec, IndentsByBraceDepthAndIsIdempotent)
{
    constexpr std::string_view source = "int main() {\nint x = 1;\nif (x) {\nreturn x;\n}\n}\n";
    const heimdall::Formatter formatter;
    const std::string expected = "int main() {\n    int x = 1;\n    if (x) {\n        return x;\n    }\n}\n";
    const std::string formatted = formatter.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, IgnoresBracesInCommentsLiteralsAndPreprocessorDirectives)
{
    constexpr std::string_view source =
        "#define BLOCK { ignored }\n"
        "void f() {\n"
        "auto text = \"{ not a block }\"; // } comment\n"
        "/* { comment */\n"
        "}\n";
    const heimdall::Formatter formatter;
    const std::string expected =
        "#define BLOCK { ignored }\n"
        "void f() {\n"
        "    auto text = \"{ not a block }\"; // } comment\n"
        "    /* { comment */\n"
        "}\n";
    const std::string formatted = formatter.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, SupportsTabsBlankLinesCrLfAndMissingFinalNewline)
{
    const heimdall::Formatter formatter({ .indent_width = 2, .use_tabs = true });
    EXPECT_EQ(formatter.Format("{\r\n\r\nx\r\n}\r\n"), "{\r\n\r\n\tx\r\n}\r\n");
    EXPECT_EQ(formatter.Format("{\nx\n}"), "{\n\tx\n}");
}
