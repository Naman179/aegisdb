cmake_minimum_required(VERSION 3.20)
project(aegisdb CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Sanitiser options ─────────────────────────────────────────────────────────
option(AEGIS_ASAN "Build with AddressSanitizer"  OFF)
option(AEGIS_TSAN "Build with ThreadSanitizer"   OFF)

# ── Base compile flags ────────────────────────────────────────────────────────
add_compile_options(-Wall -Wextra -Wpedantic -pthread)
add_link_options(-pthread)

if(AEGIS_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

if(AEGIS_TSAN)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

# ── Testing ───────────────────────────────────────────────────────────────────
include(CTest)
enable_testing()
include(cmake/FetchGTest.cmake)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ── Subdirectories ────────────────────────────────────────────────────────────
add_subdirectory(src)
add_subdirectory(tools)
add_subdirectory(tests)
