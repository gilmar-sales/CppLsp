#include <gtest/gtest.h>

#include <CppLsp/CompileCommands.hpp>
#include <CppLsp/Semantic.hpp>

#include <filesystem>
#include <fstream>

TEST(CompileCommandsSpec, ReadsArgumentsAndExtractsDefinesUndefinesAndIncludes)
{
    const auto root = std::filesystem::path(CPPLSP_SOURCE_DIR);
    const auto path = std::filesystem::temp_directory_path() / "cpplsp_compile_commands_test.json";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "[{\"directory\":\"" << root.generic_string()
            << "\",\"file\":\"src/main.cpp\",\"arguments\":[\"g++\",\"-std=c++26\","
               "\"-DDEBUG=1\",\"-DVALUE\",\"-UOLD\",\"-I\",\"include\"]}]";
    }

    auto database = cpplsp::CompileDatabase::Load(path);
    ASSERT_TRUE(database) << (database ? "" : database.error());
    ASSERT_EQ(database->Commands().size(), 1);
    const auto* command = database->Find(root / "src" / "main.cpp");
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->defines.at("DEBUG"), "1");
    EXPECT_EQ(command->defines.at("VALUE"), "1");
    EXPECT_EQ(command->undefines, (std::vector<std::string> { "OLD" }));
    ASSERT_EQ(command->include_directories.size(), 1);
    EXPECT_EQ(command->include_directories[0], (root / "include").lexically_normal());
    std::filesystem::remove(path);
}

TEST(CompileCommandsSpec, ParsesCommandStringFallback)
{
    const auto root = std::filesystem::path(CPPLSP_SOURCE_DIR);
    const auto path = std::filesystem::temp_directory_path() / "cpplsp_compile_commands_command_test.json";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "[{\"directory\":\"" << root.generic_string()
            << "\",\"file\":\"src/main.cpp\",\"command\":\"g++ -DVALUE=42 -I include src/main.cpp\"}]";
    }
    auto database = cpplsp::CompileDatabase::Load(path);
    ASSERT_TRUE(database);
    const auto* command = database->Find(root / "src" / "main.cpp");
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->defines.at("VALUE"), "42");
    EXPECT_EQ(command->arguments.size(), 5);
    std::filesystem::remove(path);
}

TEST(SemanticSpec, UsesLocalTypeNamesToResolveAsteriskAmbiguity)
{
    constexpr std::string_view source = "struct A {};\nusing Alias = A;\n";
    const cpplsp::SemanticAnalyzer analyzer;
    const auto types = analyzer.CollectTypeNames(source);
    EXPECT_EQ(analyzer.ClassifyAsteriskStatement("A * b;", types), cpplsp::AsteriskMeaning::Declaration);
    EXPECT_EQ(analyzer.ClassifyAsteriskStatement("unknown * b;", types), cpplsp::AsteriskMeaning::Ambiguous);
    EXPECT_EQ(analyzer.ClassifyAsteriskStatement("value * result;", types, { "value" }),
              cpplsp::AsteriskMeaning::Multiplication);
    EXPECT_EQ(analyzer.ClassifyAsteriskStatement("int * b;", types), cpplsp::AsteriskMeaning::Declaration);
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
    const cpplsp::SemanticAnalyzer analyzer;
    const auto diagnostics = analyzer.AnalyzeUnusedLocals(source);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].code, "CPPLSP101");
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
    cpplsp::CompileCommand command;
    command.defines.emplace("FEATURE", "1");
    const auto diagnostics = cpplsp::SemanticAnalyzer().AnalyzeUnusedLocals(source, &command);
    ASSERT_EQ(diagnostics.size(), 1);
    EXPECT_EQ(diagnostics[0].message, "local variable 'active_unused' is never used");
}
