cmake_minimum_required(VERSION 3.20)
project(msm-standalone CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output binaries into a single bin/ directory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin")

# ThreadSanitizer (opt-in via -DENABLE_TSAN=ON). GCC/Clang only. Must be applied
# globally — partial instrumentation produces false negatives.
option(ENABLE_TSAN "Build with ThreadSanitizer instrumentation" OFF)

if (ENABLE_TSAN)
    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(STATUS "ThreadSanitizer ENABLED")
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=thread)
        if (NOT CMAKE_BUILD_TYPE STREQUAL "Release"
            AND NOT CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
            add_compile_options(-O1)
        endif ()
    else ()
        message(WARNING "ENABLE_TSAN=ON but compiler is ${CMAKE_CXX_COMPILER_ID} — ignored.")
    endif ()
endif ()

# Sensible warnings.
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    add_compile_options(/W4 /wd4100 /wd4324 /wd4996)
endif ()

add_subdirectory(src/msm)
add_subdirectory(benchmarks/concurrency)

enable_testing()
add_subdirectory(tests)
