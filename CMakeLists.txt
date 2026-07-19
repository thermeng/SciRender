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
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 OpenGLWidgets)
find_package(OpenGL REQUIRED)

# FIX: Set Qt policy QTP0004 to NEW to support your custom sub-directory layout and silence the macro warning
qt_policy(SET QTP0004 NEW)

# ---------------------------------------------------------------------------
# 2. Configure Vendor Libraries
# ---------------------------------------------------------------------------

# GLAD
add_library(glad STATIC "vendor/glad/glad.c")
target_include_directories(glad PUBLIC "vendor/glad/include")

# GLM (Header-Only Math)
add_library(glm INTERFACE)
target_include_directories(glm INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/vendor")

# Note: Screenshots capture the live GL viewport FBO directly via QImage
# native writers (no STB, no ScreenshotExporter class). Format is inferred
# from the file extension; PNG keeps alpha when Transparent is enabled.

# ---------------------------------------------------------------------------
# 3. Main Application Target Definition
# ---------------------------------------------------------------------------

# Explicitly list source files to avoid capturing deleted/legacy main.cpp files
add_executable(SciRender
    src/app/main.cpp                 # Qt entry point
    src/core/Camera.cpp
    src/core/Camera.h
    src/core/Colormaps.h
    src/core/mesh_loader.cpp
    src/core/mesh_loader.h
    src/core/mesh_utils.cpp
    src/core/vtk_parser.cpp
    src/core/stl_parser.cpp
    src/render/renderer.cpp
    src/render/renderer.h
    src/render/render_settings.cpp
    src/render/render_settings.h
    src/render/LightingModel.cpp
    src/render/LightingModel.h
    src/render/ColormapManager.cpp
    src/render/ColormapManager.h
    src/render/VectorGlyphSet.cpp
    src/render/VectorGlyphSet.h
    src/render/MeshGLManager.cpp
    src/render/MeshGLManager.h
    src/render/gizmo.cpp
    src/render/gizmo.h
    src/render/colorbar_overlay.cpp
    src/render/colorbar_overlay.h
    src/ui/custom_viewport_item.cpp
    src/ui/custom_viewport_item.h
)

# Inform your C++ files where GLSL shaders are housed
target_compile_definitions(SciRender PRIVATE
    SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/shaders"
)

# Set base root compilation includes relative to 'src'
target_include_directories(SciRender PRIVATE "src")

# Link everything together
# FIX: Linked Qt6::QuickControls2 so the linker can map QQuickStyle symbols
target_link_libraries(SciRender PRIVATE
    glad
    glm
    OpenGL::GL
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::OpenGLWidgets
)

# ---------------------------------------------------------------------------
# 4. Declarative UI Assets Module
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# 4. Declarative UI Assets Module + Shaders
# ---------------------------------------------------------------------------
qt_add_qml_module(SciRender
    URI SciRenderUI
    VERSION 1.0
    QML_FILES
        ui/Main.qml
    RESOURCES
        src/shaders/mesh.vert
        src/shaders/mesh.frag
        src/shaders/grid.vert
        src/shaders/grid.frag
        src/shaders/glyph.vert
        src/shaders/glyph.frag
)

# ---------------------------------------------------------------------------
# 5. Post-Build Pipeline Automation (Shader Dynamic Sync)
# ---------------------------------------------------------------------------
add_custom_command(TARGET SciRender POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SciRender>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders"
            "$<TARGET_FILE_DIR:SciRender>/shaders"
    COMMENT "--- Synchronizing runtime GLSL shader files ---"
)

