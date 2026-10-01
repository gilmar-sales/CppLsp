// Corpus sample: heavy preprocessor usage.
#ifndef CPPLSP_SAMPLE_CONFIG_HPP
#define CPPLSP_SAMPLE_CONFIG_HPP

#include "CppLsp/Buffer.hpp"
#include <cstddef>
#include <cstdint>
#include <string>

#define CPPLSP_VERSION_MAJOR 0
#define CPPLSP_VERSION_MINOR 1
#define CPPLSP_VERSION_PATCH 0

#define CPPLSP_CONCAT_IMPL(a, b) a##b
#define CPPLSP_CONCAT(a, b) CPPLSP_CONCAT_IMPL(a, b)
#define CPPLSP_UNIQUE_NAME(prefix) CPPLSP_CONCAT(prefix, __LINE__)

#define CPPLSP_STRINGIFY_IMPL(x) #x
#define CPPLSP_STRINGIFY(x) CPPLSP_STRINGIFY_IMPL(x)

#if CPPLSP_VERSION_MAJOR == 0
#define CPPLSP_API_HINT "unstable"
#elif CPPLSP_VERSION_MAJOR >= 1
#define CPPLSP_API_HINT "stable"
#else
#error "unreachable version"
#endif

#ifdef __clang__
#define CPPLSP_COMPILER "clang"
#elif defined(__GNUC__)
#define CPPLSP_COMPILER "gcc"
#elif defined(_MSC_VER)
#define CPPLSP_COMPILER "msvc"
#else
#define CPPLSP_COMPILER "unknown"
#endif

#ifndef CPPLSP_MAX_PATH
#define CPPLSP_MAX_PATH 4096
#endif

#if defined(CPPLSP_ENABLE_IO) && !defined(CPPLSP_DISABLE_MMAP)
#define CPPLSP_USE_MMAP 1
#endif

#include <vector>
#include "CppLsp/Arena.hpp"
#include <algorithm>

#endif // CPPLSP_SAMPLE_CONFIG_HPP
