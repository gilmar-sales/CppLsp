#include <Baldr/Baldr.hpp>

#include <CppLsp/Greeter.hpp>

struct Payload
{
    std::string message;
};

int main()
{
    auto builder = skr::ApplicationBuilder().WithExtension<baldr::BaldrExtension>();

    auto app = builder.Build<baldr::WebApplication>();

    app->MapGet("/json", [&]() { return Payload { .message = cpplsp::Greet("World") }; });

    app->Run();

    return 0;
}
