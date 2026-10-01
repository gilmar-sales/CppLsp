#include <gtest/gtest.h>

#include <CppLsp/Greeter.hpp>

TEST(GreeterSpec, ReturnsHelloWorldMessage)
{
    EXPECT_EQ(cpplsp::Greet("World"), "Hello, World!");
}

TEST(GreeterSpec, InterpolatesGivenName)
{
    EXPECT_EQ(cpplsp::Greet("CppLsp"), "Hello, CppLsp!");
}
