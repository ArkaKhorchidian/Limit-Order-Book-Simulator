cmake_minimum_required(VERSION 3.20)
project(lob_simulator VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)   # no GNU extensions; we want strict C++20

# Export compile commands for clang-tidy / IDE integration.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Werror          # treat warnings as errors — enforces clean code
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-march=native>   # enables RDTSC, AVX, etc.
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
)

add_link_options(
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

# ---------------------------------------------------------------------------
# Core library (header-only interface + 3 translation units)
# ---------------------------------------------------------------------------

add_library(lob_core STATIC
    src/order_book.cpp
    src/matching_engine.cpp
    src/feed_handler.cpp
)

target_include_directories(lob_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# ---------------------------------------------------------------------------
# Tests (Google Test via FetchContent)
# ---------------------------------------------------------------------------

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    FIND_PACKAGE_ARGS NAMES GTest  # use system GTest if available
)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(test_lob test/test_lob.cpp)
target_link_libraries(test_lob PRIVATE lob_core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(test_lob)

# ---------------------------------------------------------------------------
# Benchmarks (standalone binary; no external dependency)
# ---------------------------------------------------------------------------

add_executable(bench_matching bench/bench_matching.cpp)
target_link_libraries(bench_matching PRIVATE lob_core)

# Release build is the only meaningful one for benchmarks.
# Usage: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
#        ./build/bench_matching

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

message(STATUS "")
message(STATUS "LOB Simulator configuration")
message(STATUS "  C++ standard : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Install dir  : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
