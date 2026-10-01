FetchContent_Declare(
  gtest
  GIT_REPOSITORY "https://github.com/google/googletest"
  GIT_TAG        "v1.17.0"
)
FetchContent_MakeAvailable(gtest)

include(GoogleTest)

include_directories(${gtest_SOURCE_DIR}/include ${gtest_SOURCE_DIR})

file(GLOB_RECURSE TESTS_SRC
  "src/**.cpp"
)

add_executable(Tests_run ${TESTS_SRC})

target_link_libraries(Tests_run gtest gtest_main cpplsp::cpplsp)
target_include_directories(Tests_run PUBLIC ${gtest_SOURCE_DIR}/include ../include)

gtest_discover_tests(Tests_run)
