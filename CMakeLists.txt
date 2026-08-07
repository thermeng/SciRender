cmake_minimum_required(VERSION 3.16)
project(SciRender LANGUAGES C CXX)

# Force C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable Qt's automated compilation tools
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Qt 6.11 + MinGW 8.1 workaround: ensure exceptions are enabled so MOC-generated
# *_NON_NOEXCEPT comparators compile cleanly.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions")
set(CMAKE_CXX_FLAGS_DEBUG   "${CMAKE_CXX_FLAGS_DEBUG}   -O0 -g -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -s -DNDEBUG")

# Faster Release builds: LTO
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

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
    src/render/MeshPass.cpp
    src/render/MeshPass.h
    src/render/GlyphPass.cpp
    src/render/GlyphPass.h
    src/render/ParticlePass.cpp
    src/render/ParticlePass.h
    src/render/VolumePass.cpp
    src/render/VolumePass.h
    src/render/ColormapSync.cpp
    src/render/ColormapSync.h
    src/render/LodScheduler.cpp
    src/render/LodScheduler.h
    src/render/gl_raii.cpp
    src/render/gl_raii.h
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
    src/render/screenshot_capture.cpp
    src/render/screenshot_capture.h
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
    src/ui/main_window.ui
    src/ui/scalar_page.ui
    src/ui/lighting_page.ui
    src/ui/slicing_page.ui
    src/ui/view_display_page.ui
    src/ui/vectors_page.ui
    src/ui/volume_page.ui
    src/ui/streamlines_page.ui
    src/ui/screenshot_page.ui
    src/ui/mesh_info_page.ui
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
        src/shaders/volume.vert
        src/shaders/volume.frag
        src/shaders/lod.comp
        src/shaders/lod_output.comp
        src/shaders/lod_tris.comp
        src/shaders/quality_overlay.vert
        src/shaders/quality_overlay.frag
        src/shaders/depth_peel.vert
        src/shaders/depth_peel.frag
        src/shaders/composite.vert
        src/shaders/composite.frag
)

# ---------------------------------------------------------------------------
# 6b. Icon Resources
# ---------------------------------------------------------------------------
qt_add_resources(SciRender "icons"
    PREFIX "/"
    FILES
        src/resources/icons/checkmark.svg
        src/resources/icons/downarrow.svg
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
# NOTE: Tests removed to speed up regular builds.
# To re-enable, restore the section from git history and run:
#   cmake -B build -DSciRender_BUILD_TESTS=ON
# ---------------------------------------------------------------------------
