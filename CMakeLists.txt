cmake_minimum_required(VERSION 3.16)
project(SciRender LANGUAGES C CXX)

# Force C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable Qt's automated compilation tools
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ---------------------------------------------------------------------------
# 1. Locate Qt 6 and System Graphics Dependencies
# ---------------------------------------------------------------------------
# FIX: Added QuickControls2 to COMPONENTS to fetch C++ headers like <QQuickStyle>
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGLWidgets)
find_package(OpenGL REQUIRED)



# ---------------------------------------------------------------------------
# 2. Configure Vendor Libraries
# ---------------------------------------------------------------------------

# GLAD
add_library(glad STATIC "vendor/glad/src/gl.c")
target_include_directories(glad PUBLIC "vendor/glad/include")

# GLM (Header-Only Math)
add_library(glm INTERFACE)
target_include_directories(glm INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/vendor")

# ---------------------------------------------------------------------------
# 3. CoreLib — parser, mesh, camera, colormaps (no Qt, no GL)
# ---------------------------------------------------------------------------
add_library(CoreLib STATIC
    src/core/Camera.cpp
    src/core/Camera.h
    src/core/Colormaps.h
    src/core/mesh_loader.cpp
    src/core/mesh_loader.h
    src/core/mesh_utils.cpp
    src/core/vtk_parser.cpp
    src/core/stl_parser.cpp
    src/core/obj_parser.cpp
    src/core/obj_parser.h
    src/core/vtk_xml_parser.cpp
)
target_include_directories(CoreLib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(CoreLib PUBLIC glm)

# ---------------------------------------------------------------------------
# 4. RenderLib — renderer, GL managers, streamlines, glyphs, overlays
# ---------------------------------------------------------------------------
add_library(RenderLib STATIC
    src/render/renderer.cpp
    src/render/renderer.h
    src/render/render_settings.cpp
    src/render/render_settings.h
    src/render/render_config.h
    src/render/shader_utils.h
    src/render/LightingModel.cpp
    src/render/LightingModel.h
    src/render/ColormapManager.cpp
    src/render/ColormapManager.h
    src/render/VectorGlyphSet.cpp
    src/render/VectorGlyphSet.h
    src/render/StreamlineSet.cpp
    src/render/StreamlineSet.h
    src/render/MeshGLManager.cpp
    src/render/MeshGLManager.h
    src/render/gizmo.cpp
    src/render/gizmo.h
    src/render/colorbar_overlay.cpp
    src/render/colorbar_overlay.h
    src/render/GridRenderer.cpp
    src/render/GridRenderer.h
    src/render/BBoxOverlay.cpp
    src/render/BBoxOverlay.h
    src/render/QualityOverlayRenderer.cpp
    src/render/QualityOverlayRenderer.h
    src/render/StreamlineController.cpp
    src/render/StreamlineController.h
)
target_include_directories(RenderLib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(RenderLib PUBLIC CoreLib glad OpenGL::GL
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGLWidgets)

# ---------------------------------------------------------------------------
# 5. Main Application Target
# ---------------------------------------------------------------------------
add_executable(SciRender
    src/app/main.cpp
    src/ui/viewport_widget.cpp
    src/ui/viewport_widget.h
    src/ui/main_window.cpp
    src/ui/main_window.h
    assets/app_icon.rc
)

target_compile_definitions(SciRender PRIVATE
    APP_ICON_SRC="${CMAKE_CURRENT_SOURCE_DIR}/assets/app_icon.ico"
)

target_link_libraries(SciRender PRIVATE CoreLib RenderLib)

# ---------------------------------------------------------------------------
# 6. Shader Resources
# ---------------------------------------------------------------------------
qt_add_resources(SciRender "shaders"
    PREFIX "/"
    FILES
        src/shaders/mesh.vert
        src/shaders/mesh.frag
        src/shaders/grid.vert
        src/shaders/grid.frag
        src/shaders/glyph.vert
        src/shaders/glyph.frag
        src/shaders/bbox.vert
        src/shaders/bbox.frag
        src/shaders/streamline.vert
        src/shaders/streamline.frag
        src/shaders/seed.vert
        src/shaders/seed.frag
        src/shaders/particle.vert
        src/shaders/particle.frag
        src/shaders/lod.comp
        src/shaders/lod_output.comp
        src/shaders/lod_tris.comp
)

# ---------------------------------------------------------------------------
# 6b. Icon Resources
# ---------------------------------------------------------------------------
qt_add_resources(SciRender "icons"
    PREFIX "/"
    FILES
        src/resources/icons/checkmark.svg
)

# ---------------------------------------------------------------------------
# 7. Post-Build Pipeline Automation (Shader Dynamic Sync)
# ---------------------------------------------------------------------------
add_custom_command(TARGET SciRender POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SciRender>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders"
            "$<TARGET_FILE_DIR:SciRender>/shaders"
    COMMENT "--- Synchronizing runtime GLSL shader files ---"
)

# ---------------------------------------------------------------------------
# 8. Tests
# ---------------------------------------------------------------------------
enable_testing()

# Parser regression — no Qt/GL, only CoreLib
add_executable(parse_regression
    tests/parse_regression.cpp
)
target_link_libraries(parse_regression PRIVATE CoreLib)
target_compile_definitions(parse_regression PRIVATE GLM_FORCE_SWIZZLE=0)
add_test(NAME parse_regression COMMAND parse_regression)

# Streamline direction — needs CoreLib + RenderLib + GLAD
add_executable(streamline_direction_test
    tests/streamline_direction_test.cpp
)
target_link_libraries(streamline_direction_test PRIVATE CoreLib RenderLib glad)
target_compile_definitions(streamline_direction_test PRIVATE GLM_FORCE_SWIZZLE=0)
add_test(NAME streamline_direction_test COMMAND streamline_direction_test)
