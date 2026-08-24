cmake_minimum_required(VERSION 3.19)
project(SciRender LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# MinGW workaround for Qt exception handling
if(MINGW)
    # Add exception support globally to all C++ targets
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fexceptions>)

    # Disable NDEBUG in Debug mode (NDEBUG should NEVER be in Debug flags)
    string(REPLACE "-DNDEBUG" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
endif()

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

# ---------------------------------------------------------------------------
# 1. Locate Qt 6 and System Graphics Dependencies
# ---------------------------------------------------------------------------
list(APPEND CMAKE_PREFIX_PATH "C:/Qt/6.11.1/mingw_64")
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGLWidgets)
find_package(OpenGL REQUIRED)

# Set ZLIB hints before searching
if(NOT DEFINED ZLIB_ROOT AND EXISTS "C:/Qt/Tools/mingw1310_64/x86_64-w64-mingw32")
    set(ZLIB_ROOT "C:/Qt/Tools/mingw1310_64/x86_64-w64-mingw32" CACHE PATH
        "Path to a zlib installation")
endif()
find_package(ZLIB REQUIRED)

# ---------------------------------------------------------------------------
# 2. Configure Vendor Libraries
# ---------------------------------------------------------------------------
add_library(glad STATIC "vendor/glad/src/gl.c")
target_include_directories(glad PUBLIC "vendor/glad/include")

add_library(glm INTERFACE)
target_include_directories(glm INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/vendor")

add_library(pugixml STATIC "vendor/pugixml/pugixml.cpp")
target_include_directories(pugixml PUBLIC "vendor/pugixml")

add_library(lz4 STATIC "vendor/lz4/lz4.c")
target_include_directories(lz4 PUBLIC "vendor/lz4")

add_library(lzma STATIC
    "vendor/lzma/LzmaDec.c"
    "vendor/lzma/7zAlloc.c"
)
target_include_directories(lzma PUBLIC "vendor/lzma")

# ---------------------------------------------------------------------------
# 3. CoreLib — parser, mesh, camera, colormaps
# ---------------------------------------------------------------------------
add_library(CoreLib STATIC
    src/core/Camera.cpp
    src/core/Camera.h
    src/core/Colormaps.h
     src/core/mesh_loader.cpp
    src/core/FieldResolver.cpp
    src/core/FieldResolver.h
     src/core/mesh_loader.h
     src/core/isosurface.cpp
     src/core/isosurface.h
     src/core/mesh_utils.cpp
    src/core/vtk_parser.cpp
    src/core/vtk_common.cpp
    src/core/stl_parser.cpp
    src/core/obj_parser.cpp
    src/core/obj_parser.h
     src/core/vtk_xml_parser.cpp
    src/core/pvd_parser.cpp
    src/core/pvd_parser.h
)
target_include_directories(CoreLib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(CoreLib PUBLIC glm pugixml ZLIB::ZLIB lz4 lzma)

# ---------------------------------------------------------------------------
# 4. RenderLib — renderer, GL managers, streamlines, glyphs, overlays
# ---------------------------------------------------------------------------
add_library(RenderLib STATIC
    src/render/foundation/renderer.cpp
    src/render/foundation/renderer.h
    src/render/settings/render_settings.cpp
    src/render/settings/render_settings.h
    src/render/foundation/render_config.h
    src/render/foundation/shader_utils.h
    src/render/settings/isosurface_controller.cpp
    src/render/settings/isosurface_controller.h
    src/render/settings/animation_controller.cpp
    src/render/settings/animation_controller.h
    src/render/settings/AnimationExporter.cpp
    src/render/settings/AnimationExporter.h
    src/render/foundation/LightingModel.cpp
    src/render/foundation/LightingModel.h
    src/render/passes/ColormapManager.cpp
    src/render/passes/ColormapManager.h
    src/render/streamlines/VectorGlyphSet.cpp
    src/render/streamlines/VectorGlyphSet.h
    src/render/streamlines/StreamlineSet.cpp
    src/render/streamlines/StreamlineSet.h
    src/render/passes/MeshGLManager.cpp
    src/render/passes/MeshGLManager.h
    src/render/passes/MeshPass.cpp
    src/render/passes/MeshPass.h
    src/render/passes/GlyphPass.cpp
    src/render/passes/GlyphPass.h
    src/render/passes/ParticlePass.cpp
    src/render/passes/ParticlePass.h
    src/render/passes/VolumePass.cpp
    src/render/passes/VolumePass.h
    src/render/overlays/VolumeSliceOverlay.cpp
    src/render/overlays/VolumeSliceOverlay.h
    src/render/passes/WireframePass.cpp
    src/render/passes/WireframePass.h
    src/render/passes/DepthPeelPass.cpp
    src/render/passes/DepthPeelPass.h
    src/render/passes/LodScheduler.cpp
    src/render/passes/LodScheduler.h
    src/render/foundation/gl_raii.cpp
    src/render/foundation/gl_raii.h
    src/render/overlays/gizmo.cpp
    src/render/overlays/gizmo.h
    src/render/overlays/colorbar_overlay.cpp
    src/render/overlays/colorbar_overlay.h
    src/render/overlays/BBoxOverlay.cpp
    src/render/overlays/BBoxOverlay.h
    src/render/overlays/QualityOverlayRenderer.cpp
    src/render/overlays/QualityOverlayRenderer.h
    src/render/streamlines/StreamlineController.cpp
    src/render/streamlines/StreamlineController.h
    src/render/overlays/screenshot_capture.cpp
    src/render/overlays/screenshot_capture.h
    src/render/overlays/AviMjpegWriter.cpp
    src/render/overlays/AviMjpegWriter.h
)
target_include_directories(RenderLib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(RenderLib PUBLIC CoreLib glad OpenGL::GL
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGLWidgets)

# ---------------------------------------------------------------------------
# 5. Main Application Target
# ---------------------------------------------------------------------------
add_executable(SciRender
    src/app/main.cpp
    src/app/gpu_preference.cpp
    src/ui/viewport_widget.cpp
    src/ui/viewport_widget.h
    src/ui/colorbar_style_dialog.cpp
    src/ui/colorbar_style_dialog.h
    src/ui/animation_export_dialog.cpp
    src/ui/animation_export_dialog.h
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
    src/ui/animation_page.ui
    assets/app_icon.rc
)

# Standardized path normalization for Windows Resource Compiler
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/app_icon.ico" APP_ICON_PATH)
target_compile_definitions(SciRender PRIVATE
    APP_ICON_SRC="${APP_ICON_PATH}"
)

target_link_libraries(SciRender PRIVATE CoreLib RenderLib)

# Standard Qt6 Project Setup (Handles deployment dynamic copying)
qt_standard_project_setup()

# ---------------------------------------------------------------------------
# 6. Resources
# ---------------------------------------------------------------------------
qt_add_resources(SciRender "shaders"
    PREFIX "/"
    FILES
        src/shaders/mesh.vert
        src/shaders/mesh.frag
        src/shaders/mesh_clip.geo
        src/shaders/wire.vert
        src/shaders/wire.geo
        src/shaders/wire.frag
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
        src/shaders/volume_slice.vert
        src/shaders/volume_slice.frag
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

qt_add_resources(SciRender "icons"
    PREFIX "/"
    FILES
        src/resources/icons/checkmark.svg
        src/resources/icons/downarrow.svg
        src/resources/icons/wireframe.svg
        src/resources/icons/surface.svg
        src/resources/icons/volume.svg
        src/resources/icons/slice.svg
        src/resources/icons/ortho_px.svg
        src/resources/icons/ortho_nx.svg
        src/resources/icons/ortho_py.svg
        src/resources/icons/ortho_ny.svg
        src/resources/icons/ortho_pz.svg
        src/resources/icons/ortho_nz.svg
        src/resources/icons/reset.svg
        src/resources/icons/collapse.svg
        src/resources/icons/open_mesh.svg
        src/resources/icons/lighting.svg
        src/resources/icons/slicing.svg
        src/resources/icons/view.svg
        src/resources/icons/colormap.svg
        src/resources/icons/vectors.svg
        src/resources/icons/streamlines.svg
        src/resources/icons/mesh_info.svg
        src/resources/icons/screenshot.svg
        src/resources/icons/animation.svg
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