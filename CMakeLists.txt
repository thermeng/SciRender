cmake_minimum_required(VERSION 3.16)
project(RendererQT LANGUAGES C CXX)

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

# Note: STB has been omitted since ScreenshotExporter was refactored to use QImage natively!

# ---------------------------------------------------------------------------
# 3. Main Application Target Definition
# ---------------------------------------------------------------------------

# Explicitly list source files to avoid capturing deleted/legacy main.cpp files
add_executable(RendererQT
    main.cpp   # Active Qt entry point at the project root
    src/renderer/renderer.cpp
    src/renderer/renderer.h
    src/camera/Camera.cpp
    src/camera/Camera.h
    src/colormaps/Colormaps.h
    src/mesh/vtk_parser.cpp
    src/gizmo/gizmo.cpp
    src/gizmo/gizmo.h
    src/mesh/mesh_loader.cpp
    src/mesh/mesh_loader.h
    src/mesh/mesh_utils.cpp
    src/mesh/stl_parser.cpp
    src/export/screenshot.cpp
    src/export/screenshot.h
    src/ui/custom_viewport_item.cpp
    src/ui/custom_viewport_item.h
)

# Inform your C++ files where GLSL shaders are housed
target_compile_definitions(RendererQT PRIVATE
    SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/shaders"
)

# Set base root compilation includes relative to 'src'
target_include_directories(RendererQT PRIVATE "src")

# Link everything together
# FIX: Linked Qt6::QuickControls2 so the linker can map QQuickStyle symbols
target_link_libraries(RendererQT PRIVATE
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
qt_add_qml_module(RendererQT
    URI RendererQTUI
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
add_custom_command(TARGET RendererQT POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:RendererQT>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders"
            "$<TARGET_FILE_DIR:RendererQT>/shaders"
    COMMENT "--- Synchronizing runtime GLSL shader files ---"
)

