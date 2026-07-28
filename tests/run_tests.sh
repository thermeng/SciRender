#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"
MINGW=/c/Qt/Tools/mingw1310_64/bin
export PATH="$MINGW:$PATH"
echo "[run_tests] building parse_regression..."
g++ -std=c++20 -O0 -g ../src/core/vtk_parser.cpp ../src/core/mesh_utils.cpp \
  ../src/core/stl_parser.cpp ../src/core/obj_parser.cpp ../src/core/mesh_loader.cpp parse_regression.cpp \
  -I.. -I../src -I../vendor -o parse_regression.exe
echo "[run_tests] running parse_regression..."
./parse_regression.exe
echo ""
echo "[run_tests] building streamline_direction_test..."
g++ -std=c++20 -O0 -g ../vendor/glad/src/gl.c ../src/core/vtk_parser.cpp ../src/core/mesh_utils.cpp \
  ../src/core/stl_parser.cpp ../src/core/obj_parser.cpp ../src/core/mesh_loader.cpp \
  ../src/render/StreamlineSet.cpp streamline_direction_test.cpp \
  -I.. -I../src -I../vendor -o streamline_direction_test.exe
echo "[run_tests] running streamline_direction_test..."
./streamline_direction_test.exe
