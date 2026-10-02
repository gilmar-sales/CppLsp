#pragma once

#include <expected>
#include <CppLsp/Language.hpp>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cpplsp
{

struct CompileCommand
{
    std::filesystem::path directory;
    std::filesystem::path file;
    std::vector<std::string> arguments;
    std::unordered_map<std::string, std::string> defines;
    std::vector<std::string> undefines;
    std::vector<std::filesystem::path> include_directories;
    CppStandard standard = CppStandard::Cpp20;
};

class CompileDatabase
{
  public:
    static std::expected<CompileDatabase, std::string> Load(const std::filesystem::path& path);

    const std::vector<CompileCommand>& Commands() const noexcept { return m_commands; }
    const CompileCommand* Find(std::filesystem::path file) const;

  private:
    std::vector<CompileCommand> m_commands;
};

} // namespace cpplsp
