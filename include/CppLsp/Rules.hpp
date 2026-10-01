#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cpplsp
{

enum class Severity : std::uint8_t
{
    Warning,
    Error
};

enum class RuleId : std::uint8_t
{
    NullMacro,
    TrailingWhitespace
};

struct TextEdit
{
    std::size_t offset;
    std::size_t length;
    std::string replacement;
};

struct Diagnostic
{
    RuleId rule;
    Severity severity;
    std::string code;
    std::string message;
    std::size_t offset;
    std::size_t length;
    std::uint32_t line;
    std::uint32_t column;
    bool has_fix;
    TextEdit fix;
};

struct RuleOptions
{
    bool null_macro = true;
    bool trailing_whitespace = true;
};

class RuleEngine
{
  public:
    explicit RuleEngine(RuleOptions options = {}) : m_options(options) {}

    std::vector<Diagnostic> Analyze(std::string_view source) const;
    static std::string ApplyFixes(std::string_view source, const std::vector<Diagnostic>& diagnostics);

  private:
    RuleOptions m_options;
};

} // namespace cpplsp
