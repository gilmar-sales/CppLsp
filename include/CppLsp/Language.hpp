#pragma once

#include <cstdint>

namespace cpplsp
{

// Supported source-language dialects. Separate from the compiler standard
// required to build CppLsp itself.
enum class CppStandard : std::uint8_t
{
    Cpp20,
    Cpp23,
    Cpp26
};

} // namespace cpplsp
