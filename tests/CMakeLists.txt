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
