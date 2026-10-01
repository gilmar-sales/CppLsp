set(BENCHMARK_ENABLE_TESTING OFF)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF)
set(BENCHMARK_ENABLE_INSTALL OFF)

FetchContent_Declare(
  google-benchmark
  GIT_REPOSITORY "https://github.com/google/benchmark.git"
  GIT_TAG "v1.9.5"
)
FetchContent_MakeAvailable(google-benchmark)

file(GLOB BENCH_SRC CONFIGURE_DEPENDS "src/*.cpp")

add_executable(CoreBench ${BENCH_SRC})

target_link_libraries(CoreBench PRIVATE cpplsp::core benchmark::benchmark benchmark::benchmark_main)
target_compile_definitions(CoreBench PRIVATE CPPLSP_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")
target_compile_features(CoreBench PRIVATE cxx_std_26)
