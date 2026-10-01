FetchContent_Declare(
  gtest
  GIT_REPOSITORY "https://github.com/google/googletest"
  GIT_TAG        "v1.17.0"
)
FetchContent_MakeAvailable(gtest)

include(GoogleTest)

include_directories(${gtest_SOURCE_DIR}/include ${gtest_SOURCE_DIR})

file(GLOB_RECURSE TESTS_SRC CONFIGURE_DEPENDS
  "src/**.cpp"
)

add_executable(Tests_run ${TESTS_SRC})

target_link_libraries(Tests_run gtest gtest_main cpplsp::core cpplsp::semantic)
target_include_directories(Tests_run PUBLIC ${gtest_SOURCE_DIR}/include ../include)

target_compile_definitions(Tests_run PRIVATE CPPLSP_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

gtest_discover_tests(Tests_run)

add_library(CliSemanticFixture OBJECT EXCLUDE_FROM_ALL fixtures/semantic_unused.cpp)
target_compile_features(CliSemanticFixture PRIVATE cxx_std_26)
add_test(
  NAME CliSemanticDetectsUnusedLocal
  COMMAND ${CMAKE_COMMAND}
          "-DCPPLSP_EXECUTABLE=$<TARGET_FILE:cpplsp>"
          "-DCOMPILE_COMMANDS_FILE=${PROJECT_BINARY_DIR}/compile_commands.json"
          "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/semantic_unused.cpp"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckSemanticCli.cmake"
)
