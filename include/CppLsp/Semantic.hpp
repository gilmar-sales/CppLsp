#pragma once

#include <CppLsp/CompileCommands.hpp>

#include <string>
#include <string_view>
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
};

} // namespace cpplsp
