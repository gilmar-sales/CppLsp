#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cpplsp
{

enum class DirectiveKind
{
    Include,
    Define,
    Undef,
    If,
    Ifdef,
    Ifndef,
    Elif,
    Else,
    Endif,
    Pragma,
    Other
};

struct PreprocessorDirective
{
    DirectiveKind kind;
    std::size_t offset;
    std::size_t length;
};

struct PreprocessorDiagnostic
{
    std::size_t offset;
    std::string message;
};

struct PreprocessorResult
{
    // Active non-directive source, with object-like macro substitutions.
    // Includes/directives are intentionally not opened or expanded.
    std::string active_source;
    std::vector<PreprocessorDirective> directives;
    std::vector<PreprocessorDiagnostic> diagnostics;
};

class Preprocessor
{
  public:
    using MacroMap = std::unordered_map<std::string, std::string>;

    explicit Preprocessor(MacroMap predefined = {}) : m_predefined(std::move(predefined)) {}

    PreprocessorResult Process(std::string_view source) const;

  private:
    MacroMap m_predefined;
};

} // namespace cpplsp
