cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Text+ - FFGL text source for Resolume
#
# macOS:  ./scripts/build_mac.sh   (universal arm64 + x86_64 .bundle)
# Linux:  only used for development/testing (headless test host, see tests/)
# ---------------------------------------------------------------------------

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal build for Resolume on Apple Silicon and Intel")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

project(TextPlus VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(TEXTPLUS_BUILD_TESTS "Build the unit tests and the headless test host (Linux: EGL, macOS: CGL)" OFF)

include(cmake/Dependencies.cmake)

# ---------------------------------------------------------------------------
# Core: platform-neutral text engine (no FFGL, no OpenGL)
# ---------------------------------------------------------------------------
add_library(tp_core STATIC
    src/core/FontDatabase.cpp
    src/core/FontFace.cpp
    src/core/TextLayout.cpp
    src/core/GlyphCache.cpp
    src/core/Animator.cpp
    src/core/Utf8.cpp
    src/core/Csv.cpp
)
if(APPLE)
    target_sources(tp_core PRIVATE src/core/FontPaths_mac.mm)
    set_source_files_properties(src/core/FontPaths_mac.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
elseif(WIN32)
    target_sources(tp_core PRIVATE src/core/FontPaths_win.cpp)
else()
    target_sources(tp_core PRIVATE src/core/FontPaths_linux.cpp)
endif()
target_include_directories(tp_core PUBLIC src)
target_link_libraries(tp_core PUBLIC tp_freetype tp_harfbuzz tp_msdfgen)
find_package(Threads REQUIRED)
target_link_libraries(tp_core PUBLIC Threads::Threads)
if(APPLE)
    target_link_libraries(tp_core PUBLIC "-framework CoreText" "-framework CoreFoundation" "-framework Foundation")
endif()

# ---------------------------------------------------------------------------
# FFGL plugin
# ---------------------------------------------------------------------------
set(PLUGIN_SOURCES
    src/render/GlyphAtlas.cpp
    src/render/TextRenderer.cpp
    src/plugin/TextSource.cpp
    ${FFGL_SDK_DIR}/source/lib/FFGLSDK.cpp
)

add_library(TextPlus MODULE ${PLUGIN_SOURCES})
# Everything is hidden except the FFGL entry point (plugMain), which lives in the SDK sources.
if(NOT MSVC)
    set_source_files_properties(${FFGL_SDK_DIR}/source/lib/FFGLSDK.cpp PROPERTIES COMPILE_OPTIONS "-fvisibility=default")
endif()
target_include_directories(TextPlus PRIVATE src ${FFGL_SDK_DIR}/source/lib)
target_link_libraries(TextPlus PRIVATE tp_core)

if(APPLE)
    target_compile_definitions(TextPlus PRIVATE GL_SILENCE_DEPRECATION)
    set_target_properties(TextPlus PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION bundle
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME "TextPlus"
        MACOSX_BUNDLE_GUI_IDENTIFIER "de.textplus.ffgl"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    )
    target_link_libraries(TextPlus PRIVATE "-framework OpenGL" "-Wl,-exported_symbol,_plugMain")
elseif(WIN32)
    target_link_libraries(TextPlus PRIVATE opengl32)
else()
    find_package(GLEW REQUIRED)
    find_package(OpenGL REQUIRED)
    target_link_libraries(TextPlus PRIVATE GLEW::GLEW OpenGL::GL "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/exports.map")
    set_target_properties(TextPlus PROPERTIES PREFIX "")
endif()

# ---------------------------------------------------------------------------
# Tests (Linux only): layout unit test + headless FFGL host rendering PNGs
# ---------------------------------------------------------------------------
if(TEXTPLUS_BUILD_TESTS AND NOT WIN32)
    # Unit tests expect the Linux test fonts (DejaVu, Cantarell, Inter)
    add_executable(layout_test tests/layout_test.cpp)
    target_link_libraries(layout_test PRIVATE tp_core)

    add_executable(ffgl_test_host tests/ffgl_test_host.cpp tests/stb_impl.cpp)
    target_include_directories(ffgl_test_host PRIVATE src ${FFGL_SDK_DIR}/source/lib tests)
    if(APPLE)
        target_link_libraries(ffgl_test_host PRIVATE "-framework OpenGL" ${CMAKE_DL_LIBS})
    else()
        target_link_libraries(ffgl_test_host PRIVATE GLEW::GLEW OpenGL::GL EGL ${CMAKE_DL_LIBS})
    endif()
    add_dependencies(ffgl_test_host TextPlus)
endif()
