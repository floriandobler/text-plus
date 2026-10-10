# Third-party dependencies, all fetched and linked statically so the plugin
# bundle has no runtime dependencies besides the OS.
#
#   FFGL SDK   (Resolume fork, master)  - plugin interface        - BSD-style
#   FreeType   2.14.3                   - font loading, outlines  - FTL
#   HarfBuzz   14.6.0                   - text shaping            - MIT
#   msdfgen    1.13                     - (M)SDF generation       - MIT

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# --- FFGL SDK: we only need its sources, not its CMake project --------------
FetchContent_Declare(ffgl_sdk
    GIT_REPOSITORY https://github.com/resolume/ffgl.git
    GIT_TAG        5b6e07840277df9ec35a228b9eb067636316350f
    GIT_SHALLOW    FALSE
    SOURCE_SUBDIR  this-directory-does-not-exist
)
FetchContent_MakeAvailable(ffgl_sdk)
set(FFGL_SDK_DIR ${ffgl_sdk_SOURCE_DIR} CACHE INTERNAL "")

# --- FreeType -----------------------------------------------------------------
set(FT_DISABLE_ZLIB     ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   ON  CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL    ON  CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS   OFF CACHE BOOL "" FORCE)
FetchContent_Declare(freetype
    GIT_REPOSITORY https://github.com/freetype/freetype.git
    GIT_TAG        VER-2-14-3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(freetype)
add_library(kontur_freetype INTERFACE)
target_link_libraries(kontur_freetype INTERFACE freetype)

# --- HarfBuzz: amalgamated single-file build, OpenType functions only --------
FetchContent_Declare(harfbuzz
    GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
    GIT_TAG        14.6.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  this-directory-does-not-exist
)
FetchContent_MakeAvailable(harfbuzz)
add_library(kontur_harfbuzz STATIC ${harfbuzz_SOURCE_DIR}/src/harfbuzz.cc)
target_include_directories(kontur_harfbuzz PUBLIC ${harfbuzz_SOURCE_DIR}/src)
set_target_properties(kontur_harfbuzz PROPERTIES CXX_STANDARD 17)
if(NOT MSVC)
    target_compile_options(kontur_harfbuzz PRIVATE -w)
endif()

# --- msdfgen core -------------------------------------------------------------
FetchContent_Declare(msdfgen
    GIT_REPOSITORY https://github.com/Chlumsky/msdfgen.git
    GIT_TAG        v1.13
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  this-directory-does-not-exist
)
FetchContent_MakeAvailable(msdfgen)
file(GLOB MSDFGEN_CORE_SOURCES ${msdfgen_SOURCE_DIR}/core/*.cpp)
add_library(kontur_msdfgen STATIC ${MSDFGEN_CORE_SOURCES})
target_include_directories(kontur_msdfgen PUBLIC ${msdfgen_SOURCE_DIR})
target_compile_definitions(kontur_msdfgen PUBLIC MSDFGEN_PUBLIC= MSDFGEN_USE_CPP11)
if(NOT MSVC)
    target_compile_options(kontur_msdfgen PRIVATE -w)
endif()
