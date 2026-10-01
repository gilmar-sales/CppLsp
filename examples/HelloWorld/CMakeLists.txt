add_executable(HelloWorld src/main.cpp)

target_link_libraries(HelloWorld PRIVATE baldr)
target_compile_features(HelloWorld PRIVATE cxx_std_26)
