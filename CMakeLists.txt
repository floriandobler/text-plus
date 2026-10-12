cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Resolume plugins built on shared kits:
#   libs/ffglkit      FFGL plumbing: parameters, host quirks, GL state
#   libs/sdfkit       distance-field shapes (glyphs, boxes) with a style stack
#   plugins/textplus  Text+ (FFGL source)
#   tools/            headless FFGL test host
#
# macOS:   ./scripts/build_mac.sh      (universal arm64 + x86_64 .bundle)
# Windows: cmake -S . -B build -A x64 && cmake --build build --config Release
# Linux:   development and tests only  (./scripts/run_tests_linux.sh)
# ---------------------------------------------------------------------------

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal build for Resolume on Apple Silicon and Intel")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()
if(MSVC)
    # Same C++ runtime as the FFGL SDK and its GLEW library (Resolume installs it)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

project(ResolumePlus VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
if(MSVC)
    add_compile_options(/utf-8 /W3 /wd4244 /wd4267 /wd4305 /wd4996)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

option(PLUS_BUILD_TESTS "Build the unit tests and the headless test host" OFF)
# Backwards compatible switch name
if(TEXTPLUS_BUILD_TESTS)
    set(PLUS_BUILD_TESTS ON)
endif()

include(cmake/Dependencies.cmake)
if(NOT APPLE AND NOT WIN32)
    find_package(GLEW REQUIRED)
    find_package(OpenGL REQUIRED)
endif()

add_subdirectory(libs/ffglkit)
add_subdirectory(libs/sdfkit)
add_subdirectory(plugins/textplus)
if(PLUS_BUILD_TESTS)
    add_subdirectory(tools/ffgl_test_host)
endif()
