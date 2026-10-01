cmake_minimum_required(VERSION 3.29)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (${CMAKE_CURRENT_SOURCE_DIR} STREQUAL ${CMAKE_SOURCE_DIR})
  option(CPPLSP_BUILD_TESTS "Build CppLsp tests" ON)
  option(CPPLSP_BUILD_EXAMPLES "Build CppLsp examples" ON)

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

file(GLOB_RECURSE CPPLSP_SOURCES
  src/**.cpp
  include/**.hpp
)

list(FILTER CPPLSP_SOURCES EXCLUDE REGEX "src/main\\.cpp$")

add_library(cpplsp ${CPPLSP_SOURCES})
add_library(cpplsp::cpplsp ALIAS cpplsp)

target_link_libraries(cpplsp PUBLIC baldr)

target_include_directories(cpplsp PUBLIC
  $<BUILD_INTERFACE:${cpplsp_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(cpplsp PUBLIC cxx_std_26)

add_executable(cpplsp-server src/main.cpp)
target_link_libraries(cpplsp-server PRIVATE cpplsp::cpplsp baldr)
target_compile_features(cpplsp-server PRIVATE cxx_std_26)

if (CPPLSP_BUILD_TESTS)
  enable_testing()
  include(CTest)
  add_subdirectory(test)
endif ()

if (CPPLSP_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif ()
