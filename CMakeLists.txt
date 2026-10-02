cmake_minimum_required(VERSION 3.29)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (${CMAKE_CURRENT_SOURCE_DIR} STREQUAL ${CMAKE_SOURCE_DIR})
  option(HEIMDALL_BUILD_TESTS "Build Heimdall tests" ON)
  option(HEIMDALL_BUILD_BENCHMARKS "Build Heimdall benchmarks" ON)

  find_program(HEIMDALL_CCACHE_PROGRAM ccache)
  if (HEIMDALL_CCACHE_PROGRAM)
    message(STATUS "Using ccache: ${HEIMDALL_CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${HEIMDALL_CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${HEIMDALL_CCACHE_PROGRAM}")
  else ()
    message(STATUS "ccache not found; compiling without cache")
  endif ()
else ()
  option(HEIMDALL_BUILD_TESTS "Build Heimdall tests" OFF)
  option(HEIMDALL_BUILD_BENCHMARKS "Build Heimdall benchmarks" OFF)
endif ()

project(Heimdall VERSION 0.1.0 LANGUAGES C CXX)

include(FetchContent)

set(SKIRNIR_USE_FMT OFF CACHE BOOL "Disable optional Skirnir fmt integration" FORCE)
set(BUILD_SKIRNIR_EXAMPLES OFF CACHE BOOL "Disable Skirnir examples" FORCE)
set(BUILD_SKIRNIR_TESTS OFF CACHE BOOL "Disable Skirnir tests" FORCE)
set(BUILD_SKIRNIR_BENCH OFF CACHE BOOL "Disable Skirnir benchmarks" FORCE)
set(SIMDJSON_STATIC_REFLECTION ON CACHE BOOL "Enable simdjson C++26 reflection support" FORCE)
set(SIMDJSON_DEVELOPER_MODE OFF CACHE BOOL "Disable simdjson developer targets" FORCE)
set(SIMDJSON_BUILD_STATIC_LIB OFF CACHE BOOL "Build simdjson as a target dependency" FORCE)
set(SIMDJSON_BUILD_TESTS OFF CACHE BOOL "Disable simdjson tests" FORCE)
set(SIMDJSON_BUILD_BENCHMARKS OFF CACHE BOOL "Disable simdjson benchmarks" FORCE)
set(SIMDJSON_BUILD_EXAMPLES OFF CACHE BOOL "Disable simdjson examples" FORCE)

FetchContent_Declare(
  simdjson
  GIT_REPOSITORY "https://github.com/simdjson/simdjson.git"
  GIT_TAG        "610f14d2140d5e702e2dee27545e6f4bd28fd7e2"
)
FetchContent_MakeAvailable(simdjson)

FetchContent_Declare(
  skirnir
  GIT_REPOSITORY "https://github.com/gilmar-sales/Skirnir.git"
  GIT_TAG        "v0.24.0"
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(skirnir)

# Dependency policy: heimdall_core is standard-library-only (see core/CMakeLists.txt
# and test/src/CorePolicySpec.cpp). Skirnir/simdjson, gtest and Google Benchmark
# are used only outside core: semantic module, LSP, tests and benches.
add_subdirectory(core)

add_library(heimdall_semantic STATIC
  semantic/CompileCommands.cpp
  semantic/Semantic.cpp
)
add_library(heimdall::semantic ALIAS heimdall_semantic)
target_include_directories(heimdall_semantic PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(heimdall_semantic PUBLIC cxx_std_26)
target_link_libraries(heimdall_semantic PUBLIC heimdall::core PRIVATE skirnir::skirnir simdjson::simdjson)

add_executable(heimdall src/cli.cpp)
target_link_libraries(heimdall PRIVATE heimdall::core heimdall::semantic)
target_compile_features(heimdall PRIVATE cxx_std_26)

add_executable(heimdall-lsp lsp/main.cpp)
target_link_libraries(heimdall-lsp PRIVATE heimdall::core heimdall::semantic simdjson::simdjson)
target_compile_features(heimdall-lsp PRIVATE cxx_std_26)

if (HEIMDALL_BUILD_TESTS)
  enable_testing()
  include(CTest)
  add_subdirectory(test)
endif ()

if (HEIMDALL_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif ()
