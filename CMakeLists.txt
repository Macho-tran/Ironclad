cmake_minimum_required(VERSION 3.20)
project(ironclad
    VERSION 0.1.0
    DESCRIPTION "Deterministic rollback netcode toolkit"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Top-level options
# ---------------------------------------------------------------------------
option(IRONCLAD_BUILD_TESTS    "Build ironclad unit tests" ON)
option(IRONCLAD_BUILD_DEMO     "Build headless arena_demo" ON)
option(IRONCLAD_BUILD_SDL_DEMO "Build SDL2 arena_view (requires libsdl2-dev)" OFF)
option(IRONCLAD_SANITIZE       "Enable AddressSanitizer + UBSan" OFF)
option(IRONCLAD_LTO            "Enable link-time optimization" OFF)
option(IRONCLAD_WERROR         "Treat warnings as errors" ON)

# ---------------------------------------------------------------------------
# Global C++ settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(cmake/Warnings.cmake)
include(cmake/Sanitizers.cmake)

# Threads — needed by UDP test (and by anything pulling in <thread>).
find_package(Threads REQUIRED)

if(IRONCLAD_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
    if(_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO requested but not supported: ${_ipo_err}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Vendored third-party
# ---------------------------------------------------------------------------
add_library(ironclad_xxhash STATIC
    third_party/xxhash/xxhash.c)
target_include_directories(ironclad_xxhash
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xxhash)
# xxhash is third-party C; do NOT apply our strict warnings to it.
set_target_properties(ironclad_xxhash PROPERTIES
    C_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON)

# doctest is a header-only "interface" library used only by tests.
add_library(ironclad_doctest INTERFACE)
target_include_directories(ironclad_doctest
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/doctest)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_subdirectory(src)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(IRONCLAD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Apps
# ---------------------------------------------------------------------------
if(IRONCLAD_BUILD_DEMO OR IRONCLAD_BUILD_SDL_DEMO)
    add_subdirectory(apps)
endif()

# ---------------------------------------------------------------------------
# Install / summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "===== ironclad configuration =====")
message(STATUS "  build type      : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard    : ${CMAKE_CXX_STANDARD}")
message(STATUS "  tests           : ${IRONCLAD_BUILD_TESTS}")
message(STATUS "  arena_demo      : ${IRONCLAD_BUILD_DEMO}")
message(STATUS "  arena_view (SDL): ${IRONCLAD_BUILD_SDL_DEMO}")
message(STATUS "  sanitizers      : ${IRONCLAD_SANITIZE}")
message(STATUS "  LTO             : ${IRONCLAD_LTO}")
message(STATUS "  -Werror         : ${IRONCLAD_WERROR}")
message(STATUS "===================================")
message(STATUS "")
