# sdfkit: distance-field shapes (glyph MTSDF from an atlas, analytic boxes),
# groups and a style stack, rendered with depth peeling and a threshold composite.
add_library(sdfkit STATIC
    src/Renderer.cpp
    src/Atlas.cpp
)
target_include_directories(sdfkit PUBLIC include)
target_link_libraries(sdfkit PUBLIC ffglkit)
