#pragma once

#include <CppLsp/CompileCommands.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>

namespace cpplsp
{

enum class AsteriskMeaning
{
    NotApplicable,
    Declaration,
    Multiplication,
    Ambiguous
};

struct SemanticDiagnostic
{
    std::string code;
    std::string message;
    std::size_t offset;
    std::size_t length;
    std::uint32_t line;
    std::uint32_t column;
};

// Deliberately local type oracle: records types declared in this translation
// unit plus built-ins, without opening transitively included headers.
class SemanticAnalyzer
{
  public:
    std::unordered_set<std::string> CollectTypeNames(std::string_view source,
                                                      const CompileCommand* command = nullptr) const;

    AsteriskMeaning ClassifyAsteriskStatement(std::string_view statement,
                                               const std::unordered_set<std::string>& known_types,
                                               const std::unordered_set<std::string>& known_values = {}) const;

    std::vector<SemanticDiagnostic> AnalyzeUnusedLocals(std::string_view source,
                                                        const CompileCommand* command = nullptr) const;
};

} // namespace cpplsp
