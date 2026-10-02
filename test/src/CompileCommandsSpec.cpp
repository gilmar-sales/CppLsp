#include <gtest/gtest.h>

#include <Heimdall/CompileCommands.hpp>
#include <Heimdall/Semantic.hpp>

#include <filesystem>
#include <fstream>

TEST(CompileCommandsSpec, ReadsArgumentsAndExtractsDefinesUndefinesAndIncludes)
{
    const auto root = std::filesystem::path(HEIMDALL_SOURCE_DIR);
    const auto path = std::filesystem::temp_directory_path() / "heimdall_compile_commands_test.json";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "[{\"directory\":\"" << root.generic_string()
            << "\",\"file\":\"src/main.cpp\",\"arguments\":[\"g++\",\"-std=c++26\","
               "\"-DDEBUG=1\",\"-DVALUE\",\"-UOLD\",\"-I\",\"include\"]}]";
    }

    auto database = heimdall::CompileDatabase::Load(path);
    ASSERT_TRUE(database) << (database ? "" : database.error());
    ASSERT_EQ(database->Commands().size(), 1);
    const auto* command = database->Find(root / "src" / "main.cpp");
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->standard, heimdall::CppStandard::Cpp26);
    EXPECT_EQ(command->defines.at("DEBUG"), "1");
    EXPECT_EQ(command->defines.at("VALUE"), "1");
    EXPECT_EQ(command->undefines, (std::vector<std::string> { "OLD" }));
    ASSERT_EQ(command->include_directories.size(), 1);
    EXPECT_EQ(command->include_directories[0], (root / "include").lexically_normal());
    std::filesystem::remove(path);
}

TEST(CompileCommandsSpec, ParsesCommandStringFallback)
{
    const auto root = std::filesystem::path(HEIMDALL_SOURCE_DIR);
    const auto path = std::filesystem::temp_directory_path() / "heimdall_compile_commands_command_test.json";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "[{\"directory\":\"" << root.generic_string()
            << "\",\"file\":\"src/main.cpp\",\"command\":\"g++ -DVALUE=42 -I include src/main.cpp\"}]";
    }
    auto database = heimdall::CompileDatabase::Load(path);
    ASSERT_TRUE(database);
    const auto* command = database->Find(root / "src" / "main.cpp");
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->defines.at("VALUE"), "42");
    EXPECT_EQ(command->arguments.size(), 5);
    EXPECT_EQ(command->standard, heimdall::CppStandard::Cpp20);
    std::filesystem::remove(path);
}

TEST(CompileCommandsSpec, SelectsLatestRecognizedLanguageStandardOption)
{
    const auto root = std::filesystem::path(HEIMDALL_SOURCE_DIR);
    const auto path = std::filesystem::temp_directory_path() / "heimdall_compile_commands_standard_test.json";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "[{\"directory\":\"" << root.generic_string()
            << "\",\"file\":\"src/main.cpp\",\"arguments\":[\"cl\",\"/std:c++20\",\"/std:c++23\"]}]";
    }
    auto database = heimdall::CompileDatabase::Load(path);
    ASSERT_TRUE(database);
    ASSERT_EQ(database->Commands().size(), 1);
    EXPECT_EQ(database->Commands()[0].standard, heimdall::CppStandard::Cpp23);
    std::filesystem::remove(path);
}

TEST(SemanticSpec, UsesLocalTypeNamesToResolveAsteriskAmbiguity)
{
    constexpr std::string_view source = "struct A {};\nusing Alias = A;\n";
    const heimdall::SemanticAnalyzer analyzer;
    const auto types = analyzer.CollectTypeNames(source);
    EXPECT_EQ(analyzer.ClassifyAsteriskStatement("A * b;", types), heimdall::AsteriskMeaning::Declaration);
    EXPECT_EQ(analyzer.ClassifyAsteriskStatement("unknown * b;", types), heimdall::AsteriskMeaning::Ambiguous);
    EXPECT_EQ(analyzer.ClassifyAsteriskStatement("value * result;", types, { "value" }),
              heimdall::AsteriskMeaning::Multiplication);
    EXPECT_EQ(analyzer.ClassifyAsteriskStatement("int * b;", types), heimdall::AsteriskMeaning::Declaration);
}

TEST(SemanticSpec, FindsUnusedSimpleLocalsOnlyInsideFunctionBodies)
{
    constexpr std::string_view source =
        "int global_value;\n"
        "struct Holder { int field; };\n"
        "void f() {\n"
        "  int unused = 1;\n"
        "  int used = 2;\n"
        "  consume(used);\n"
        "}\n";
    const heimdall::SemanticAnalyzer analyzer;
    const auto diagnostics = analyzer.AnalyzeUnusedLocals(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].code, "HEIMDALL101");
    EXPECT_EQ(diagnostics[0].message, "local variable 'unused' is never used");
    EXPECT_EQ(diagnostics[0].line, 4);
}

TEST(SemanticSpec, HonorsConditionalBranchesAndCompileCommandDefines)
{
    constexpr std::string_view source =
        "#ifdef FEATURE\n"
        "void enabled() { int active_unused; }\n"
        "#else\n"
        "void disabled() { int inactive_unused; }\n"
        "#endif\n";
    heimdall::CompileCommand command;
    command.defines.emplace("FEATURE", "1");
    const auto diagnostics = heimdall::SemanticAnalyzer().AnalyzeUnusedLocals(source, &command);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].message, "local variable 'active_unused' is never used");
}
