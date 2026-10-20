# ---------------------------------------------------------------------------
# Text+ : FFGL text source
# ---------------------------------------------------------------------------
set(TEXTPLUS_VERSION ${PROJECT_VERSION})

# Core: platform-neutral text engine (no FFGL, no OpenGL)
add_library(tp_core STATIC
    src/core/FontDatabase.cpp
    src/core/FontFace.cpp
    src/core/FontFallback.cpp
    src/core/TextLayout.cpp
    src/core/GlyphCache.cpp
    src/core/Animator.cpp
    src/core/Utf8.cpp
    src/core/Csv.cpp
    src/core/Clock.cpp
    src/core/FileUtil.cpp
    src/core/Path.cpp
    src/core/Osc.cpp
    src/core/Placeholders.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/EmbeddedFonts.cpp
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
elseif(WIN32)
    target_link_libraries(tp_core PUBLIC dwrite ws2_32)
endif()

# Monochrome emoji font compiled into the plugin (SIL Open Font Licence)
include(${PROJECT_SOURCE_DIR}/cmake/EmbedFile.cmake)
embed_files(${CMAKE_CURRENT_BINARY_DIR}/EmbeddedFonts.cpp textplus
    NotoEmoji ${CMAKE_CURRENT_SOURCE_DIR}/resources/NotoEmoji-Regular.ttf)

# FFGL plugins: the source "Text+" and the effect "Text+ Fill" from the same code
set(TEXTPLUS_PLUGIN_SOURCES src/plugin/TextSource.cpp src/plugin/Params.cpp src/plugin/Presets.cpp)
function(textplus_plugin target bundleName bundleId)
    add_library(${target} MODULE ${TEXTPLUS_PLUGIN_SOURCES})
    target_link_libraries(${target} PRIVATE tp_core sdfkit ffglkit)
    target_compile_definitions(${target} PRIVATE TEXTPLUS_VERSION="${TEXTPLUS_VERSION}")
    if(APPLE)
        set_target_properties(${target} PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION bundle
            MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in
            MACOSX_BUNDLE_BUNDLE_NAME "${bundleName}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${bundleId}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${TEXTPLUS_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${TEXTPLUS_VERSION}"
        )
        target_link_libraries(${target} PRIVATE "-Wl,-exported_symbol,_plugMain")
    elseif(WIN32)
        set_target_properties(${target} PROPERTIES PREFIX "" SUFFIX ".dll")
        target_sources(${target} PRIVATE cmake/exports.def)
    else()
        target_link_libraries(${target} PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/exports.map")
        set_target_properties(${target} PROPERTIES PREFIX "")
    endif()
    # Put the plugin where the scripts expect it (build/TextPlus.so|.bundle|.dll)
    set_target_properties(${target} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}
        LIBRARY_OUTPUT_DIRECTORY_RELEASE ${PROJECT_BINARY_DIR}
        RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${PROJECT_BINARY_DIR})
endfunction()

textplus_plugin(TextPlus "TextPlus" "de.textplus.ffgl")
textplus_plugin(TextPlusFill "TextPlusFill" "de.textplus.fill.ffgl")
target_compile_definitions(TextPlusFill PRIVATE TEXTPLUS_EFFECT=1)

if(PLUS_BUILD_TESTS)
    add_executable(layout_test tests/layout_test.cpp)
    target_link_libraries(layout_test PRIVATE tp_core)
    set_target_properties(layout_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR} RUNTIME_OUTPUT_DIRECTORY_RELEASE ${PROJECT_BINARY_DIR})
endif()
