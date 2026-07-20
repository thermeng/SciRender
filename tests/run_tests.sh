#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"
MINGW=/c/Qt/Tools/mingw1310_64/bin
export PATH="$MINGW:$PATH"
g++ -std=c++17 -O0 -g ../src/core/vtk_parser.cpp ../src/core/mesh_utils.cpp \
  ../src/core/stl_parser.cpp ../src/core/mesh_loader.cpp parse_regression.cpp \
  -I.. -I../src -I../vendor -o parse_regression.exe
./parse_regression.exe
