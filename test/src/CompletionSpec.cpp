#include <gtest/gtest.h>

#include <Heimdall/Completion.hpp>

#include <algorithm>
#include <string_view>
#include <vector>

namespace
{

bool Contains(const std::vector<heimdall::CompletionItem>& items, std::string_view label)
{
    for (const auto& item : items)
    {
        if (item.label == label) return true;
    }
    return false;
}

const heimdall::CompletionItem* Find(const std::vector<heimdall::CompletionItem>& items,
                                     std::string_view label)
{
    for (const auto& item : items)
    {
        if (item.label == label) return &item;
    }
    return nullptr;
}

std::size_t OffsetAfter(std::string_view source, std::string_view needle)
{
    const std::size_t pos = source.find(needle);
    EXPECT_NE(pos, std::string_view::npos) << needle;
    return pos + needle.size();
}

} // namespace

TEST(CompletionSpec, ExtractsIdentifierPrefixBeforeCursor)
{
    EXPECT_EQ(heimdall::CompletionEngine::PrefixAt("int myValue", 11), "myValue");
    EXPECT_EQ(heimdall::CompletionEngine::PrefixAt("int myValue", 6), "my");
    EXPECT_EQ(heimdall::CompletionEngine::PrefixAt("int x", 0), "");
    EXPECT_EQ(heimdall::CompletionEngine::PrefixAt("a+b", 3), "b");
    EXPECT_EQ(heimdall::CompletionEngine::PrefixAt("a", 99), "a");
    EXPECT_EQ(heimdall::CompletionEngine::PrefixAt("", 0), "");
}

TEST(CompletionSpec, SuggestsLocalVariablesAndFunctionsByPrefix)
{
    constexpr std::string_view source =
        "int compute(int value) {\n"
        "  int counter = value;\n"
        "  int count_total = counter;\n"
        "  return count_total + cou;\n"
        "}\n";
    // `find("cou")` hits `counter` first, so anchor on the trailing use.
    const std::size_t pos = source.rfind("cou");
    ASSERT_NE(pos, std::string_view::npos);
    const auto items = heimdall::CompletionEngine::Complete(source, pos + 3);
    EXPECT_TRUE(Contains(items, "counter"));
    EXPECT_TRUE(Contains(items, "count_total"));
    EXPECT_FALSE(Contains(items, "value;"));

    // Function names share the same machinery under a different prefix.
    constexpr std::string_view fn_source = "int compute() {}\nint com";
    const auto fns = heimdall::CompletionEngine::Complete(fn_source, fn_source.size());
    EXPECT_TRUE(Contains(fns, "compute"));
}

TEST(CompletionSpec, SuggestsKeywordsByPrefix)
{
    constexpr std::string_view source = "int f() { ret";
    const auto items = heimdall::CompletionEngine::Complete(source, source.size());
    EXPECT_TRUE(Contains(items, "return"));
    const auto* item = Find(items, "return");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->kind, heimdall::CompletionKind::Keyword);
}

TEST(CompletionSpec, FiltersCandidatesByPrefix)
{
    constexpr std::string_view source = "int alpha = 1;\nint beta = 2;\nint al";
    const auto items = heimdall::CompletionEngine::Complete(source, source.size());
    EXPECT_TRUE(Contains(items, "alpha"));
    EXPECT_FALSE(Contains(items, "beta"));
}

TEST(CompletionSpec, EmptyPrefixReturnsKeywords)
{
    constexpr std::string_view source = "int x = 1;\n";
    const auto items = heimdall::CompletionEngine::Complete(source, source.size());
    EXPECT_TRUE(Contains(items, "return"));
    EXPECT_TRUE(Contains(items, "int"));
}

TEST(CompletionSpec, SuppressesCompletionInsideCommentsAndStrings)
{
    {
        constexpr std::string_view source = "// hello wor";
        EXPECT_TRUE(heimdall::CompletionEngine::Complete(source, source.size()).empty());
    }
    {
        constexpr std::string_view source = "/* hello wor";
        EXPECT_TRUE(heimdall::CompletionEngine::Complete(source, source.size()).empty());
    }
    {
        constexpr std::string_view source = "const char* s = \"hello wor";
        EXPECT_TRUE(heimdall::CompletionEngine::Complete(source, source.size()).empty());
    }
}

TEST(CompletionSpec, SuppressesMemberAccessUntilMembersAreModeled)
{
    {
        constexpr std::string_view source = " Khalifa; Khalifa.";
        const auto items = heimdall::CompletionEngine::Complete(source, source.size());
        EXPECT_TRUE(items.empty());
    }
    {
        constexpr std::string_view source = "ptr->";
        const auto items = heimdall::CompletionEngine::Complete(source, source.size());
        EXPECT_TRUE(items.empty());
    }
    {
        // Even with a partial member name, no guesses: `value.mem`.
        constexpr std::string_view source = "value.mem";
        const auto items = heimdall::CompletionEngine::Complete(source, source.size());
        EXPECT_TRUE(items.empty());
    }
}

TEST(CompletionSpec, SuggestsPreprocessorDirectivesAfterHash)
{
    constexpr std::string_view source = "#inc";
    const auto items = heimdall::CompletionEngine::Complete(source, source.size());
    EXPECT_TRUE(Contains(items, "include"));
    const auto* item = Find(items, "include");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->kind, heimdall::CompletionKind::Directive);
}

TEST(CompletionSpec, SuggestsDefinesAndPredefinedMacros)
{
    constexpr std::string_view source = "#define MY_FEATURE 1\nint x = MY_FEA";
    heimdall::ParserOptions options;
    options.predefined_macros.emplace("PLATFORM_FLAG", "1");
    const auto items = heimdall::CompletionEngine::Complete(source, options, source.size());
    EXPECT_TRUE(Contains(items, "MY_FEATURE"));
    // Predefined macro does not match this prefix, but is returned for empty prefixes.
    const auto all = heimdall::CompletionEngine::Complete("#define MY_FEATURE 1\n", options, 0);
    EXPECT_TRUE(Contains(all, "PLATFORM_FLAG"));
    const auto* macro = Find(items, "MY_FEATURE");
    ASSERT_NE(macro, nullptr);
    EXPECT_EQ(macro->kind, heimdall::CompletionKind::Macro);
}

TEST(CompletionSpec, ClassifiesDeclaredFunctionsAndTypes)
{
    constexpr std::string_view source =
        "struct Widget { int value; };\n"
        "int compute(Widget w) { return w.value; }\n"
        "int comp";
    const auto items = heimdall::CompletionEngine::Complete(source, source.size());
    EXPECT_TRUE(Contains(items, "compute"));
    const auto* function = Find(items, "compute");
    ASSERT_NE(function, nullptr);
    EXPECT_EQ(function->kind, heimdall::CompletionKind::Function);

    constexpr std::string_view type_source = "struct Widget { int v; };\nWid";
    const auto types = heimdall::CompletionEngine::Complete(type_source, type_source.size());
    EXPECT_TRUE(Contains(types, "Widget"));
    const auto* type = Find(types, "Widget");
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->kind, heimdall::CompletionKind::Type);
}

TEST(CompletionSpec, CompletesOnBrokenCodeWithoutCrashing)
{
    constexpr std::string_view source = "int f() { int broken return con";
    const auto items = heimdall::CompletionEngine::Complete(source, source.size());
    // `con` matches `continue`/`const`/`constexpr` even though the missing
    // ';' leaves a parse diagnostic behind.
    EXPECT_TRUE(Contains(items, "continue"));
    EXPECT_TRUE(Contains(items, "const"));
}

TEST(CompletionSpec, ClassifiesLocalsAndParametersAsVariables)
{
    constexpr std::string_view source =
        "int compute(int myParam) {\n"
        "  int myLocal = myParam;\n"
        "  return myLocal;\n"
        "}\n";
    const auto items = heimdall::CompletionEngine::Complete(source, 0);
    const auto* local = Find(items, "myLocal");
    ASSERT_NE(local, nullptr) << "expected myLocal among completions";
    EXPECT_EQ(local->kind, heimdall::CompletionKind::Variable);
    const auto* param = Find(items, "myParam");
    ASSERT_NE(param, nullptr) << "expected myParam among completions";
    EXPECT_EQ(param->kind, heimdall::CompletionKind::Variable);
    const auto* function = Find(items, "compute");
    ASSERT_NE(function, nullptr);
    EXPECT_EQ(function->kind, heimdall::CompletionKind::Function);
}

TEST(CompletionSpec, ResultsAreDeduplicatedAndSorted)
{
    constexpr std::string_view source = "int alpha = 1;\nint alpha = 2;\nint alp";
    const auto items = heimdall::CompletionEngine::Complete(source, source.size());
    std::size_t count = 0;
    for (const auto& item : items) count += item.label == "alpha";
    EXPECT_EQ(count, 1);
    EXPECT_TRUE(std::is_sorted(items.begin(), items.end(), [](const auto& left, const auto& right) {
        return left.label < right.label;
    }));
}
