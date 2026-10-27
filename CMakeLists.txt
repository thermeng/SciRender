cmake_minimum_required(VERSION 3.19)
project(SciRender LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# 0. Build switches
#
#    SCIRENDER_BUILD_APP   - the Qt/OpenGL application (and the release package)
#    SCIRENDER_BUILD_TESTS - the doctest regression suite (CPU-only, no Qt/GL)
#    SCIRENDER_BUILD_TOOLS - developer CLI diagnostics (not part of the suite)
#
#    A test-only configure (SCIRENDER_BUILD_APP=OFF) needs neither Qt nor
#    OpenGL: the whole suite is built from the Qt-free CoreLib/FvmCore/
#    CfdLib/RenderCore targets.
# ---------------------------------------------------------------------------
option(SCIRENDER_BUILD_APP   "Build the SciRender application"   ON)
option(SCIRENDER_BUILD_TESTS "Build the test suite"               ON)
option(SCIRENDER_BUILD_TOOLS "Build developer CLI diagnostics"    OFF)

# MinGW workaround for Qt exception handling
if(MINGW)
    # Add exception support globally to all C++ targets
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fexceptions>)

    # Disable NDEBUG in Debug mode (NDEBUG should NEVER be in Debug flags)
    string(REPLACE "-DNDEBUG" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
endif()

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

# ---------------------------------------------------------------------------
# 1. Locate System Dependencies
# ---------------------------------------------------------------------------
find_package(OpenMP REQUIRED)

if(SCIRENDER_BUILD_APP)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)
    set(CMAKE_AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/forms")

    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGLWidgets)
    find_package(OpenGL REQUIRED)
endif()

if(SCIRENDER_BUILD_APP OR SCIRENDER_BUILD_TESTS)
    # -----------------------------------------------------------------------
    # 2. Vendor Libraries
    # -----------------------------------------------------------------------
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

    add_library(zlib STATIC
        "vendor/zlib/adler32.c"
        "vendor/zlib/compress.c"
        "vendor/zlib/crc32.c"
        "vendor/zlib/deflate.c"
        "vendor/zlib/gzclose.c"
        "vendor/zlib/gzlib.c"
        "vendor/zlib/gzread.c"
        "vendor/zlib/gzwrite.c"
        "vendor/zlib/infback.c"
        "vendor/zlib/inffast.c"
        "vendor/zlib/inflate.c"
        "vendor/zlib/inftrees.c"
        "vendor/zlib/trees.c"
        "vendor/zlib/uncompr.c"
        "vendor/zlib/zutil.c"
        )
    target_include_directories(zlib PUBLIC "vendor/zlib")

    # -----------------------------------------------------------------------
    # 3. CoreLib — parser, mesh, camera, colormaps   (Qt-free)
    # -----------------------------------------------------------------------
    add_library(CoreLib STATIC
        src/core/Camera.cpp
        src/core/camera_math.cpp
        src/core/mesh_loader.cpp
        src/core/mesh_quality.cpp
        src/core/FieldStore.cpp
        src/core/post_fields.cpp
        src/core/gradient_estimator.cpp
        src/core/isosurface.cpp
        src/core/cell_topology.cpp
        src/core/unstructured_volume.cpp
        src/core/mesh_utils.cpp
        src/core/vtk_parser.cpp
        src/core/vtk_common.cpp
        src/core/stl_parser.cpp
        src/core/obj_parser.cpp
        src/core/vtk_xml_parser.cpp
        src/core/pvd_parser.cpp
    )
    target_include_directories(CoreLib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(CoreLib PUBLIC glm pugixml zlib lz4 lzma)

    # -----------------------------------------------------------------------
    # 3b. FvmCore — absorbed FVM solver (fvm_core) + CfdLib glue   (Qt-free)
    # -----------------------------------------------------------------------
    add_library(FvmCore STATIC
        src/mesh/Mesh.cpp
        src/mesh/BoundaryUtils.cpp
        src/field/Field.cpp
        src/field/VectorField.cpp
        src/fvm/Laplacian.cpp
        src/fvm/Poisson.cpp
        src/fvm/Gradient.cpp
        src/fvm/Divergence.cpp
        src/fvm/Convection.cpp
        src/fvm/Momentum.cpp
        src/fvm/BoundaryConditions.cpp
        src/fvm/SSTModel.cpp
        src/fvm/Solver.cpp
        src/fvm/SIMPLE.cpp
        src/fvm/SIMPLEBoundaryConditions.cpp
        src/fvm/PressureCorrection.cpp
        src/fvm/AMG.cpp
        src/io/VTKXMLWriter.cpp
        src/io/ANSYSMeshReader.cpp
        src/io/GmshReader.cpp
        src/io/Checkpoint.cpp
    )
    target_include_directories(FvmCore SYSTEM PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/eigen-3.4.0"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/json"
    )
    target_compile_features(FvmCore PUBLIC cxx_std_20)
    target_link_libraries(FvmCore PUBLIC CoreLib OpenMP::OpenMP_CXX)
    target_compile_definitions(FvmCore PUBLIC
        EIGEN_PARALLELIZE_OPENMP
        FVM_ALIGNMENT=64
        FVM_CACHE_LINE_SIZE=64
    )
    # NOTE: no -ffast-math / /fp:fast here.
    #
    # "Fast math" implies -ffinite-math-only (GCC) / /fp:fast (MSVC), i.e. the
    # compiler is allowed to assume that no value is ever NaN or infinite. Every
    # NaN/Inf guard in the solver (SIMPLE.cpp, Momentum.cpp, PressureCorrection.cpp)
    # is then compiled to `false` and the divergence detection is dead code: a
    # blown-up run silently propagates NaN into the velocity and pressure fields
    # instead of aborting. It also allows the reassociation of floating point
    # expressions, which breaks the exact telescoping of the flux sums the
    # continuity residual is built from.
    #
    # -fno-math-errno keeps the speed of the transcendental functions without
    # any of that.
    target_compile_options(FvmCore PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
            -fno-math-errno
            -funroll-loops
            -fomit-frame-pointer
    >
        $<$<CXX_COMPILER_ID:MSVC>:
            /fp:precise
    >
    )

    add_library(CfdLib STATIC
        src/cfd/cfd_types.h
        src/cfd/amg_settings_bridge.h
        src/cfd/transient_settings_bridge.h
        src/cfd/cfd_mesh_adapter.h
        src/cfd/cfd_mesh_adapter.cpp
        src/cfd/cfd_result_builder.h
        src/cfd/cfd_result_builder.cpp
        src/cfd/solution_state.h
        src/cfd/cfd_session.h
        src/cfd/cfd_session.cpp
    )
    target_include_directories(CfdLib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/src/core")
    target_link_libraries(CfdLib PUBLIC CoreLib FvmCore)

    # -----------------------------------------------------------------------
    # 3c. RenderCore — the CPU-side of the render layer   (Qt-free)
    #
    #     Streamline/vector-glyph set construction and the GL RAII helpers
    #     carry no Qt dependency, so they can be linked (and tested) without
    #     pulling in Qt6 or a live GL context.
    #
    #     The LIC vector texture cache lives here too: the part that decides
    #     the resolution of the sampled velocity field is pure CPU work and is
    #     covered by the regression suite (tests/render/lic_vector_texture_test.cpp).
    # -----------------------------------------------------------------------
    add_library(RenderCore STATIC
        src/render/streamlines/StreamlineSet.cpp
        src/render/streamlines/VectorGlyphSet.cpp
        src/render/foundation/gl_raii.cpp
        src/render/passes/VectorTextureCache.cpp
    )
    target_link_libraries(RenderCore PUBLIC CoreLib glad)
endif()

if(SCIRENDER_BUILD_APP)
    # -----------------------------------------------------------------------
    # 4. RenderLib — renderer, GL managers, streamlines, glyphs, overlays
    # -----------------------------------------------------------------------
    add_library(RenderLib STATIC
        src/render/foundation/renderer.cpp
        src/render/settings/render_settings.cpp
        src/render/settings/render_settings_apply.cpp
        src/render/settings/isosurface_controller.cpp
        src/render/settings/derived_volume_controller.cpp
        src/render/settings/animation_controller.cpp
        src/render/settings/AnimationExporter.cpp
        src/render/foundation/LightingModel.cpp
        src/render/passes/ColormapManager.cpp
        src/render/passes/MeshGLManager.cpp
        src/render/passes/MeshPass.cpp
        src/render/passes/GlyphPass.cpp
        src/render/passes/ParticlePass.cpp
        src/render/passes/VolumePass.cpp
        src/render/passes/VolumeTextureCache.cpp
        src/render/overlays/VolumeSliceOverlay.cpp
        src/render/passes/WireframePass.cpp
        src/render/passes/DepthPeelPass.cpp
        src/render/passes/LodScheduler.cpp
        src/render/passes/PlotKernel.cpp
        src/render/overlays/gizmo.cpp
        src/render/overlays/light_markers.cpp
        src/render/overlays/colorbar_overlay.cpp
        src/render/overlays/BBoxOverlay.cpp
        src/render/overlays/QualityOverlayRenderer.cpp
        src/render/overlays/LineProbeOverlay.cpp
        src/render/overlays/ProbeOverlay.cpp
        src/render/streamlines/StreamlineController.cpp
        src/render/overlays/screenshot_capture.cpp
        src/render/overlays/AviMjpegWriter.cpp
    )
    target_link_libraries(RenderLib PUBLIC CoreLib RenderCore glad OpenGL::GL
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGLWidgets)

    # -----------------------------------------------------------------------
    # 5. Main Application Target
    # -----------------------------------------------------------------------
    add_executable(SciRender
        src/app/main.cpp
        src/app/gpu_preference.cpp
        src/ui/viewport_widget.cpp
        src/ui/viewport_widget.h
        src/ui/colorbar_style_dialog.cpp
        src/ui/colorbar_style_dialog.h
        src/ui/cfd_bc_dialog.cpp
        src/ui/cfd_bc_dialog.h
        src/ui/widgets/colormap_combo.cpp
        src/ui/widgets/colormap_combo.h
        src/ui/widgets/scientific_spin_box.h
        src/ui/widgets/wheel_guard.h
        src/ui/widgets/amg_config_widget.cpp
        src/ui/widgets/amg_config_widget.h
        src/ui/widgets/range_editor.cpp
        src/ui/widgets/range_editor.h
        src/ui/widgets/plot_widget.cpp
        src/ui/widgets/plot_widget.h
         src/ui/widgets/line_plot_widget.cpp
         src/ui/widgets/line_plot_widget.h
         src/ui/widgets/monitor_plot_widget.cpp
         src/ui/widgets/monitor_plot_widget.h
        src/ui/widgets/plot_window.cpp
        src/ui/widgets/plot_window.h
        src/ui/widgets/transfer_function_widget.cpp
        src/ui/widgets/transfer_function_widget.h
        src/ui/animation_export_dialog.cpp
        src/ui/animation_export_dialog.h
        src/ui/main_window.cpp
        src/ui/main_window.h
        src/ui/solver_output_sink.cpp
        src/ui/solver_output_sink.h
        src/ui/forms/main_window.ui
        src/ui/forms/scalar_page.ui
        src/ui/forms/isosurface_page.ui
        src/ui/forms/slice_plane_page.ui
        src/ui/forms/probe_page.ui
        src/ui/forms/lighting_page.ui
        src/ui/forms/clipping_page.ui
        src/ui/forms/view_display_page.ui
        src/ui/forms/vectors_page.ui
        src/ui/forms/volume_page.ui
        src/ui/forms/streamlines_page.ui
        src/ui/forms/screenshot_page.ui
        src/ui/forms/mesh_info_page.ui
        src/ui/forms/animation_page.ui
        src/ui/forms/postproc_page.ui
        src/ui/forms/cfd_case_page.ui
        src/ui/panel_helpers.h
        src/ui/panel_helpers.cpp
        src/ui/pages/sidebar_page.h
        src/ui/pages/sidebar_page.cpp
        src/ui/pages/lighting_page.h
        src/ui/pages/lighting_page.cpp
        src/ui/pages/clipping_page.h
        src/ui/pages/clipping_page.cpp
        src/ui/pages/view_display_page.h
        src/ui/pages/view_display_page.cpp
        src/ui/pages/scalar_page.h
        src/ui/pages/scalar_page.cpp
        src/ui/pages/vectors_page.h
        src/ui/pages/vectors_page.cpp
        src/ui/pages/streamlines_page.h
        src/ui/pages/streamlines_page.cpp
        src/ui/pages/slice_plane_page.h
        src/ui/pages/slice_plane_page.cpp
        src/ui/pages/volume_page.h
        src/ui/pages/volume_page.cpp
        src/ui/pages/isosurface_page.h
        src/ui/pages/isosurface_page.cpp
        src/ui/pages/probe_page.h
        src/ui/pages/probe_page.cpp
        src/ui/pages/mesh_info_page.h
        src/ui/pages/mesh_info_page.cpp
        src/ui/pages/screenshot_page.h
        src/ui/pages/screenshot_page.cpp
        src/ui/pages/animation_page.h
        src/ui/pages/animation_page.cpp
        src/ui/pages/plots_page.h
        src/ui/pages/plots_page.cpp
        src/ui/pages/cfd_case_page.h
        src/ui/pages/cfd_case_page.cpp
        src/ui/controllers/cfd_controller.h
        src/ui/controllers/cfd_controller.cpp
        src/ui/pages/postproc_page.h
        src/ui/pages/postproc_page.cpp
        assets/app_icon.rc
    )

    # Standardized path normalization for Windows Resource Compiler
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/app_icon.ico" APP_ICON_PATH)
    target_compile_definitions(SciRender PRIVATE
        APP_ICON_SRC="${APP_ICON_PATH}"
    )

    target_link_libraries(SciRender PRIVATE CoreLib RenderLib CfdLib)

    # Standard Qt6 Project Setup (Handles deployment dynamic copying)
    qt_standard_project_setup()

    # -----------------------------------------------------------------------
    # 6. Resources
    # -----------------------------------------------------------------------
    set(SHADER_FILES
        src/shaders/mesh.vert
        src/shaders/mesh.frag
        src/shaders/pbr_common.glsl
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
        src/shaders/plot_histogram.comp
        src/shaders/lod.comp
        src/shaders/lod_output.comp
        src/shaders/lod_tris.comp
        src/shaders/quality_overlay.vert
        src/shaders/quality_overlay.frag
        src/shaders/depth_peel.vert
        src/shaders/depth_peel.frag
        src/shaders/composite.vert
        src/shaders/peel_blit.frag
        src/shaders/surface_lic.frag
    )
    qt_add_resources(SciRender "shaders"
        PREFIX "/"
        FILES ${SHADER_FILES}
    )

    qt_add_resources(SciRender "icons"
        PREFIX "/"
        FILES
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
    )

    # -----------------------------------------------------------------------
    # 7. Post-Build Pipeline Automation (Shader Dynamic Sync)
    # -----------------------------------------------------------------------
    add_custom_command(TARGET SciRender POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SciRender>/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders"
                "$<TARGET_FILE_DIR:SciRender>/shaders"
        COMMENT "--- Synchronizing runtime GLSL shader files ---"
    )

    # -----------------------------------------------------------------------
    # 8. Install & Packaging (for windeployqt + Inno Setup portable ZIP)
    #    Used by CI: cmake --install build --prefix package
    # -----------------------------------------------------------------------
    install(TARGETS SciRender DESTINATION .)
    install(DIRECTORY src/shaders DESTINATION shaders)
    install(FILES README.md DESTINATION . OPTIONAL)
endif()

# ---------------------------------------------------------------------------
# 9. Test Suite — one CTest row per doctest test case
# ---------------------------------------------------------------------------
if(SCIRENDER_BUILD_TESTS)
    add_subdirectory(vendor/doctest)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# 10. Developer CLI diagnostics (not part of the test suite)
# ---------------------------------------------------------------------------
if(SCIRENDER_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
