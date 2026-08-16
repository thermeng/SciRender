#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"
MINGW=/c/Qt/Tools/mingw1310_64/bin
export PATH="$MINGW:$PATH"
echo "[run_tests] building parse_regression..."
g++ -std=c++20 -O0 -g ../src/core/vtk_parser.cpp ../src/core/mesh_utils.cpp \
  ../src/core/stl_parser.cpp ../src/core/obj_parser.cpp ../src/core/mesh_loader.cpp \
  ../src/core/vtk_xml_parser.cpp ../vendor/pugixml/pugixml.cpp \
  ../vendor/lz4/lz4.c ../vendor/lzma/LzmaDec.c ../vendor/lzma/7zAlloc.c \
  parse_regression.cpp \
  -I.. -I../src -I../vendor -I../vendor/pugixml -I../vendor/lz4 -I../vendor/lzma -lz -o parse_regression.exe
echo "[run_tests] running parse_regression..."
./parse_regression.exe
echo ""
echo "[run_tests] building streamline_direction_test..."
g++ -std=c++20 -O0 -g ../vendor/glad/src/gl.c ../src/core/vtk_parser.cpp ../src/core/mesh_utils.cpp \
  ../src/core/stl_parser.cpp ../src/core/obj_parser.cpp ../src/core/mesh_loader.cpp \
  ../src/core/vtk_xml_parser.cpp   ../src/render/foundation/gl_raii.cpp ../src/render/streamlines/StreamlineSet.cpp streamline_direction_test.cpp \
  ../vendor/pugixml/pugixml.cpp ../vendor/lz4/lz4.c ../vendor/lzma/LzmaDec.c ../vendor/lzma/7zAlloc.c \
  -I.. -I../src -I../vendor -I../vendor/pugixml -I../vendor/lz4 -I../vendor/lzma -lz -o streamline_direction_test.exe
echo "[run_tests] running streamline_direction_test..."
./streamline_direction_test.exe
echo ""
echo "[run_tests] building isosurface_test..."
g++ -std=c++20 -O0 -g ../src/core/vtk_parser.cpp ../src/core/vtk_xml_parser.cpp ../src/core/mesh_utils.cpp \
  ../src/core/stl_parser.cpp ../src/core/obj_parser.cpp ../src/core/mesh_loader.cpp \
  ../src/core/isosurface.cpp ../vendor/pugixml/pugixml.cpp \
  ../vendor/lz4/lz4.c ../vendor/lzma/LzmaDec.c ../vendor/lzma/7zAlloc.c \
  isosurface_test.cpp \
  -I.. -I../src -I../vendor -I../vendor/pugixml -I../vendor/lz4 -I../vendor/lzma -lz -o isosurface_test.exe
echo "[run_tests] running isosurface_test..."
./isosurface_test.exe
