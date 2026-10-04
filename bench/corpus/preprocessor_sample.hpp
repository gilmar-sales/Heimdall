// Corpus sample: heavy preprocessor usage.
#ifndef HEIMDALL_SAMPLE_CONFIG_HPP
#define HEIMDALL_SAMPLE_CONFIG_HPP

#include "Heimdall/MappedBuffer.hpp"
#include <cstddef>
#include <cstdint>
#include <string>

#define HEIMDALL_VERSION_MAJOR 0
#define HEIMDALL_VERSION_MINOR 1
#define HEIMDALL_VERSION_PATCH 0

#define HEIMDALL_CONCAT_IMPL(a, b) a##b
#define HEIMDALL_CONCAT(a, b) HEIMDALL_CONCAT_IMPL(a, b)
#define HEIMDALL_UNIQUE_NAME(prefix) HEIMDALL_CONCAT(prefix, __LINE__)

#define HEIMDALL_STRINGIFY_IMPL(x) #x
#define HEIMDALL_STRINGIFY(x) HEIMDALL_STRINGIFY_IMPL(x)

#if HEIMDALL_VERSION_MAJOR == 0
#define HEIMDALL_API_HINT "unstable"
#elif HEIMDALL_VERSION_MAJOR >= 1
#define HEIMDALL_API_HINT "stable"
#else
#error "unreachable version"
#endif

#ifdef __clang__
#define HEIMDALL_COMPILER "clang"
#elif defined(__GNUC__)
#define HEIMDALL_COMPILER "gcc"
#elif defined(_MSC_VER)
#define HEIMDALL_COMPILER "msvc"
#else
#define HEIMDALL_COMPILER "unknown"
#endif

#ifndef HEIMDALL_MAX_PATH
#define HEIMDALL_MAX_PATH 4096
#endif

#if defined(HEIMDALL_ENABLE_IO) && !defined(HEIMDALL_DISABLE_MMAP)
#define HEIMDALL_USE_MMAP 1
#endif

#include <vector>
#include "Heimdall/Arena.hpp"
#include <algorithm>

#endif // HEIMDALL_SAMPLE_CONFIG_HPP
