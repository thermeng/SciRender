cmake_minimum_required(VERSION 3.16)
project(vtk_xml_test_build C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find zlib
find_path(ZLIB_INCLUDE_DIR zlib.h
  PATHS "C:/Qt/Tools/mingw1310_64/x86_64-w64-mingw32/include"
        "C:/Qt/Tools/mingw1310_64/x86_64-w64-mingw32/sys-root/mingw/include"
        "C:/Program Files/mingw-w64/mingw64/include")
find_library(ZLIB_LIBRARY z
  PATHS "C:/Qt/Tools/mingw1310_64/x86_64-w64-mingw32/lib"
        "C:/Qt/Tools/mingw1310_64/x86_64-w64-mingw32/sys-root/mingw/lib"
        "C:/Program Files/mingw-w64/mingw64/lib")

add_library(pugixml STATIC ${CMAKE_SOURCE_DIR}/../vendor/pugixml/pugixml.cpp)
target_include_directories(pugixml PUBLIC ${CMAKE_SOURCE_DIR}/../vendor/pugixml)

add_library(lz4 STATIC ${CMAKE_SOURCE_DIR}/../vendor/lz4/lz4.c)
target_include_directories(lz4 PUBLIC ${CMAKE_SOURCE_DIR}/../vendor/lz4)

add_library(lzma STATIC
    ${CMAKE_SOURCE_DIR}/../vendor/lzma/LzmaDec.c
    ${CMAKE_SOURCE_DIR}/../vendor/lzma/7zAlloc.c
)
target_include_directories(lzma PUBLIC ${CMAKE_SOURCE_DIR}/../vendor/lzma)

add_executable(parse_regression
    parse_regression.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/vtk_xml_parser.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/vtk_parser.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/vtk_common.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/mesh_utils.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/stl_parser.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/obj_parser.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/mesh_loader.cpp
)
target_include_directories(parse_regression PRIVATE
    ${CMAKE_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}/../vendor
    ${ZLIB_INCLUDE_DIR}
)
target_link_libraries(parse_regression PRIVATE pugixml ${ZLIB_LIBRARY} lz4 lzma)

# Standalone regression check for the marching-cubes isosurface extractor.
# Compiles the REAL CoreLib sources (no Qt/GL). Run via tests/run_tests.{bat,sh}
add_executable(isosurface_test
    isosurface_test.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/isosurface.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/vtk_xml_parser.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/vtk_parser.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/vtk_common.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/mesh_utils.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/stl_parser.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/obj_parser.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/mesh_loader.cpp
)
target_include_directories(isosurface_test PRIVATE
    ${CMAKE_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}/../vendor
    ${ZLIB_INCLUDE_DIR}
)
target_link_libraries(isosurface_test PRIVATE pugixml ${ZLIB_LIBRARY} lz4 lzma)

# PVD (.pvd collection index) parser check — no Qt/GL, mirrors the other
# standalone tests. Run from tests/ (fixtures are written relative to ../samples).
add_executable(pvd_test
    pvd_test.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/pvd_parser.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/vtk_xml_parser.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/vtk_parser.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/vtk_common.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/mesh_utils.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/stl_parser.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/obj_parser.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/mesh_loader.cpp
)
target_include_directories(pvd_test PRIVATE
    ${CMAKE_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}/../vendor
    ${ZLIB_INCLUDE_DIR}
)
target_link_libraries(pvd_test PRIVATE pugixml ${ZLIB_LIBRARY} lz4 lzma)

# Vector-field name listing rules (FieldResolver): point + cell lists must
# merge deduplicated. Header-inline accessors — links FieldResolver.cpp only.
add_executable(field_names_test
    field_names_test.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/FieldResolver.cpp
)
target_include_directories(field_names_test PRIVATE
    ${CMAKE_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}/../vendor
)

# Animation colormap-range accumulator rules (FieldResolver::AnimRangeState).
# Header-only logic — compiles the real header, no Qt/GL.
add_executable(anim_range_test
    anim_range_test.cpp
)
target_include_directories(anim_range_test PRIVATE
    ${CMAKE_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}/../vendor
)

# Streamline direction / integration check — needs glad+GL stubs + StreamlineSet
add_executable(streamline_direction_test
    streamline_direction_test.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/FieldResolver.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/vtk_xml_parser.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/vtk_parser.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/vtk_common.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/mesh_utils.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/stl_parser.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/obj_parser.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/mesh_loader.cpp
    ${CMAKE_SOURCE_DIR}/../src/render/foundation/gl_raii.cpp
    ${CMAKE_SOURCE_DIR}/../src/render/streamlines/StreamlineSet.cpp
    ${CMAKE_SOURCE_DIR}/../vendor/glad/src/gl.c
)
target_include_directories(streamline_direction_test PRIVATE
    ${CMAKE_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}/../vendor
    ${CMAKE_SOURCE_DIR}/../vendor/glad/include
    ${ZLIB_INCLUDE_DIR}
)
target_link_libraries(streamline_direction_test PRIVATE pugixml ${ZLIB_LIBRARY} lz4 lzma)

# CTest wiring — single source of truth for CI; run_tests.{bat,sh} should delegate here
enable_testing()
add_test(NAME parse_regression COMMAND parse_regression WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME isosurface_test COMMAND isosurface_test WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME pvd_test COMMAND pvd_test WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME field_names_test COMMAND field_names_test WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME anim_range_test COMMAND anim_range_test WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME streamline_direction_test COMMAND streamline_direction_test WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# Fly-to-face camera interpolation (camera_math) + triad footprint rules.
# Uses the real Camera class; pure GLM, no Qt/GL.
add_executable(gizmo_math_test
    gizmo_math_test.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/camera_math.cpp
    ${CMAKE_SOURCE_DIR}/../src/core/Camera.cpp
)
target_include_directories(gizmo_math_test PRIVATE
    ${CMAKE_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}/../vendor
)
add_test(NAME gizmo_math_test COMMAND gizmo_math_test WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
