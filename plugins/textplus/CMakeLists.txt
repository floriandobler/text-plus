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
    src/core/LiveData.cpp
    src/core/Osc.cpp
    src/core/WebFetch.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/EmbeddedFonts.cpp
)
if(APPLE)
    target_sources(tp_core PRIVATE src/core/FontPaths_mac.mm src/core/WebFetch_mac.mm)
    set_source_files_properties(src/core/FontPaths_mac.mm src/core/WebFetch_mac.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
elseif(WIN32)
    target_sources(tp_core PRIVATE src/core/FontPaths_win.cpp src/core/WebFetch_win.cpp)
    target_link_libraries(tp_core PUBLIC winhttp ws2_32)
else()
    target_sources(tp_core PRIVATE src/core/FontPaths_linux.cpp src/core/WebFetch_posix.cpp)
    find_package(CURL QUIET)
    if(CURL_FOUND)
        target_compile_definitions(tp_core PRIVATE TEXTPLUS_HAVE_CURL)
        target_link_libraries(tp_core PUBLIC CURL::libcurl)
    endif()
endif()
target_include_directories(tp_core PUBLIC src)
target_link_libraries(tp_core PUBLIC tp_freetype tp_harfbuzz tp_msdfgen)
find_package(Threads REQUIRED)
target_link_libraries(tp_core PUBLIC Threads::Threads)
if(APPLE)
    target_link_libraries(tp_core PUBLIC "-framework CoreText" "-framework CoreFoundation" "-framework Foundation")
elseif(WIN32)
    target_link_libraries(tp_core PUBLIC dwrite)
endif()

# Monochrome emoji font compiled into the plugin (SIL Open Font Licence)
include(${PROJECT_SOURCE_DIR}/cmake/EmbedFile.cmake)
embed_files(${CMAKE_CURRENT_BINARY_DIR}/EmbeddedFonts.cpp textplus
    NotoEmoji ${CMAKE_CURRENT_SOURCE_DIR}/resources/NotoEmoji-Regular.ttf)

# FFGL plugins: Text+ (source) and Text+ FX (effect) from the same code
function(textplus_plugin target bundleId)
    add_library(${target} MODULE src/plugin/TextSource.cpp)
    target_link_libraries(${target} PRIVATE tp_core sdfkit ffglkit)
    target_compile_definitions(${target} PRIVATE TEXTPLUS_VERSION="${TEXTPLUS_VERSION}")
    if(APPLE)
        set_target_properties(${target} PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION bundle
            MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in
            MACOSX_BUNDLE_BUNDLE_NAME "${target}"
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
textplus_plugin(TextPlus "de.textplus.ffgl")
textplus_plugin(TextPlusFX "de.textplus.ffgl.fx")
target_compile_definitions(TextPlusFX PRIVATE TEXTPLUS_FX)

if(PLUS_BUILD_TESTS)
    add_executable(layout_test tests/layout_test.cpp)
    target_link_libraries(layout_test PRIVATE tp_core)
    set_target_properties(layout_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR} RUNTIME_OUTPUT_DIRECTORY_RELEASE ${PROJECT_BINARY_DIR})
endif()
