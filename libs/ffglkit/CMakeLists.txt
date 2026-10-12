# ffglkit: what every FFGL plugin of this family needs.
# The FFGL SDK sources are compiled into this library once.
add_library(ffglkit STATIC
    src/ParamPlugin.cpp
    src/GLState.cpp
    ${FFGL_SDK_DIR}/source/lib/FFGLSDK.cpp
)
target_include_directories(ffglkit PUBLIC include ${FFGL_SDK_DIR}/source/lib)
# Everything is hidden except the FFGL entry point (plugMain), which lives in the SDK sources.
if(NOT MSVC)
    set_source_files_properties(${FFGL_SDK_DIR}/source/lib/FFGLSDK.cpp PROPERTIES COMPILE_OPTIONS "-fvisibility=default")
endif()
if(APPLE)
    target_compile_definitions(ffglkit PUBLIC GL_SILENCE_DEPRECATION)
    target_link_libraries(ffglkit PUBLIC "-framework OpenGL")
elseif(WIN32)
    # GLEW, as the FFGL SDK ships it (static)
    target_compile_definitions(ffglkit PUBLIC GLEW_STATIC)
    target_include_directories(ffglkit PUBLIC ${FFGL_SDK_DIR}/deps/glew-2.1.0/include)
    target_link_libraries(ffglkit PUBLIC ${FFGL_SDK_DIR}/deps/glew-2.1.0/lib/Release/x64/glew32s.lib opengl32)
else()
    target_link_libraries(ffglkit PUBLIC GLEW::GLEW OpenGL::GL)
endif()
