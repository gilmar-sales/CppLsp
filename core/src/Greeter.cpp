#include <CppLsp/Greeter.hpp>

namespace cpplsp
{

std::string Greet(std::string_view name)
{
    return std::string("Hello, ") + std::string(name) + "!";
}

} // namespace cpplsp
