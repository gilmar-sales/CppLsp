cmake_minimum_required(VERSION 3.29)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (${CMAKE_CURRENT_SOURCE_DIR} STREQUAL ${CMAKE_SOURCE_DIR})
  option(CPPLSP_BUILD_TESTS "Build CppLsp tests" ON)
  option(CPPLSP_BUILD_EXAMPLES "Build CppLsp examples" ON)
  option(CPPLSP_BUILD_BENCHMARKS "Build CppLsp benchmarks" ON)

  find_program(CPPLSP_CCACHE_PROGRAM ccache)
  if (CPPLSP_CCACHE_PROGRAM)
    message(STATUS "Using ccache: ${CPPLSP_CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${CPPLSP_CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CPPLSP_CCACHE_PROGRAM}")
  else ()
    message(STATUS "ccache not found; compiling without cache")
  endif ()
else ()
  option(CPPLSP_BUILD_TESTS "Build CppLsp tests" OFF)
  option(CPPLSP_BUILD_EXAMPLES "Build CppLsp examples" OFF)
  option(CPPLSP_BUILD_BENCHMARKS "Build CppLsp benchmarks" OFF)
endif ()

include(FetchContent)

FetchContent_Declare(
  baldr
  GIT_REPOSITORY "https://github.com/gilmar-sales/Baldr.git"
  # Tag v0.1.0 is not published on the remote yet (only local); pin the
  # commit it points to. Switch back to GIT_TAG "v0.1.0" once pushed.
  GIT_TAG        "dd3b404c9759f94f5c02660bb331be1e587194c7" # v0.1.0
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(baldr)

project(CppLsp VERSION 0.1.0 LANGUAGES C CXX)

# Dependency policy: cpplsp_core is standard-library-only (see core/CMakeLists.txt
# and test/src/CorePolicySpec.cpp). Baldr/simdjson, gtest and Google Benchmark
# are used only outside core: semantic module, server, tests and benches.
add_subdirectory(core)

add_library(cpplsp_semantic STATIC
  semantic/CompileCommands.cpp
  semantic/Semantic.cpp
)
add_library(cpplsp::semantic ALIAS cpplsp_semantic)
target_include_directories(cpplsp_semantic PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(cpplsp_semantic PUBLIC cxx_std_26)
target_link_libraries(cpplsp_semantic PUBLIC cpplsp::core PRIVATE simdjson::simdjson)

add_executable(cpplsp-server src/main.cpp)
target_link_libraries(cpplsp-server PRIVATE cpplsp::core baldr)
target_compile_features(cpplsp-server PRIVATE cxx_std_26)

add_executable(cpplsp src/cli.cpp)
target_link_libraries(cpplsp PRIVATE cpplsp::core cpplsp::semantic)
target_compile_features(cpplsp PRIVATE cxx_std_26)

if (CPPLSP_BUILD_TESTS)
  enable_testing()
  include(CTest)
  add_subdirectory(test)
endif ()

if (CPPLSP_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif ()

if (CPPLSP_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif ()
