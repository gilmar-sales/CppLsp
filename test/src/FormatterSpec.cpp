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
    // CRLF is preserved byte-for-byte and formatting is a fixed point.
    const std::string crlf = formatter.Format("{\r\nx\r\n}\r\n");
    EXPECT_EQ(crlf, "{\r\n\tx\r\n}\r\n");
    EXPECT_EQ(formatter.Format(crlf), crlf);
}

TEST(FormatterSpec, IndentsInnerScopesOfNamespacesAndClasses)
{
    constexpr std::string_view source =
        "namespace outer {\n"
        "namespace inner {\n"
        "int x;\n"
        "}\n"
        "}\n"
        "class Widget {\n"
        "public:\n"
        "int value;\n"
        "void run();\n"
        "private:\n"
        "int hidden;\n"
        "};\n";
    const heimdall::Formatter formatter;
    const std::string expected =
        "namespace outer {\n"
        "    namespace inner {\n"
        "        int x;\n"
        "    }\n"
        "}\n"
        "class Widget {\n"
        "public:\n"
        "    int value;\n"
        "    void run();\n"
        "private:\n"
        "    int hidden;\n"
        "};\n";
    const std::string formatted = formatter.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, DedentsCaseLabelsAndGotoLabels)
{
    constexpr std::string_view source =
        "int f(int x) {\n"
        "switch (x) {\n"
        "case 1:\n"
        "return 1;\n"
        "case 2: {\n"
        "return 2;\n"
        "}\n"
        "default:\n"
        "return 0;\n"
        "}\n"
        "goto done;\n"
        "done:\n"
        "return -1;\n"
        "}\n";
    const heimdall::Formatter formatter;
    const std::string expected =
        "int f(int x) {\n"
        "    switch (x) {\n"
        "    case 1:\n"
        "        return 1;\n"
        "    case 2: {\n"
        "        return 2;\n"
        "    }\n"
        "    default:\n"
        "        return 0;\n"
        "    }\n"
        "    goto done;\n"
        "done:\n"
        "    return -1;\n"
        "}\n";
    const std::string formatted = formatter.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, DedentsOneLevelPerLeadingCloser)
{
    constexpr std::string_view source = "namespace a {\nnamespace b {\nint x;\n}}\n";
    const heimdall::Formatter formatter;
    const std::string expected = "namespace a {\n    namespace b {\n        int x;\n}}\n";
    const std::string formatted = formatter.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
}

TEST(FormatterSpec, CollapsesBlankLinesToMaxEmptyLines)
{
    const heimdall::Formatter formatter;
    constexpr std::string_view source =
        "std::string line;\n"
        "std::size_t length = 0;\n"
        "\n"
        "\n"
        "\n"
        "bool got_length = false;\n";
    const std::string expected =
        "std::string line;\n"
        "std::size_t length = 0;\n"
        "\n"
        "bool got_length = false;\n";
    const std::string formatted = formatter.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);

    // Custom budget keeps two; zero strips all; edges are always trimmed.
    const heimdall::Formatter keep_two({ .max_empty_lines = 2 });
    const std::string two = keep_two.Format(source);
    EXPECT_EQ(two,
              "std::string line;\n"
              "std::size_t length = 0;\n"
              "\n"
              "\n"
              "bool got_length = false;\n");
    EXPECT_EQ(keep_two.Format(two), two);

    const heimdall::Formatter keep_none({ .max_empty_lines = 0 });
    EXPECT_EQ(keep_none.Format(source),
              "std::string line;\n"
              "std::size_t length = 0;\n"
              "bool got_length = false;\n");
    EXPECT_EQ(formatter.Format("\n\nint x;\n\n\n"), "int x;\n");
}

TEST(FormatterSpec, DoesNotTreatCodeBeforeDirectivesAsDirectives)
{
    // Regression: lines preceding a directive used to be copied verbatim,
    // freezing brace depth for the rest of the file.
    constexpr std::string_view source =
        "int main()\n"
        "{\n"
        "#if defined(_WIN32)\n"
        "_setmode(1);\n"
        "#endif\n"
        "x();\n"
        "}\n";
    const heimdall::Formatter formatter;
    const std::string expected =
        "int main()\n"
        "{\n"
        "#if defined(_WIN32)\n"
        "    _setmode(1);\n"
        "#endif\n"
        "    x();\n"
        "}\n";
    const std::string formatted = formatter.Format(source);
    EXPECT_EQ(formatted, expected);
    EXPECT_EQ(formatter.Format(formatted), formatted);
}
