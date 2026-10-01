#include <Baldr/Baldr.hpp>

struct Payload
{
    std::string message;
};

int main()
{
    auto builder = skr::ApplicationBuilder().WithExtension<baldr::BaldrExtension>();

    auto app = builder.Build<baldr::WebApplication>();

    app->MapGet("/json", [&]() { return Payload { .message = "Hello, World!" }; });

    app->Run();

    return 0;
}
