@echo off
setlocal
echo [run_tests] compiling parser core + running regression harness...
set GPP=
for %%P in ("C:\Qt\Tools\mingw1310_64\bin\g++.exe" "C:\Qt\Tools\mingw_64\bin\g++.exe") do (
  if not defined GPP if exist %%P set "GPP=%%P"
)
if not defined GPP ( echo ERROR: g++.exe not found in known Qt MinGW dirs & pause & exit /b 1 )
set PATH=C:\Qt\Tools\mingw1310_64\bin;%PATH%
set INC=-I.. -I..\src -I..\vendor -I..\vendor\glad\include -I..\vendor\pugixml -I..\vendor\lz4 -I..\vendor\lzma
cd /d %~dp0
set FAIL=0
echo [run_tests] building parse_regression...
%GPP% -std=c++20 -O2 -g ..\src\core\vtk_parser.cpp ..\src\core\vtk_xml_parser.cpp ..\src\core\vtk_common.cpp ..\src\core\mesh_utils.cpp ^
  ..\src\core\stl_parser.cpp ..\src\core\obj_parser.cpp ..\src\core\mesh_loader.cpp ^
  ..\vendor\pugixml\pugixml.cpp ..\vendor\lz4\lz4.c ..\vendor\lzma\LzmaDec.c ..\vendor\lzma\7zAlloc.c ^
  parse_regression.cpp ^
  -o parse_regression.exe %INC% -lz
if errorlevel 1 ( echo [run_tests] COMPILE FAILED & exit /b 1 )
echo [run_tests] running parse_regression...
parse_regression.exe > last_parse.log 2>&1
type last_parse.log
if errorlevel 1 set FAIL=1
echo.
echo [run_tests] building streamline_direction_test...
%GPP% -std=c++20 -O2 -g ..\vendor\glad\src\gl.c ..\src\core\FieldResolver.cpp ..\src\core\vtk_parser.cpp ..\src\core\vtk_xml_parser.cpp ..\src\core\vtk_common.cpp ..\src\core\mesh_utils.cpp ^
  ..\src\core\stl_parser.cpp ..\src\core\obj_parser.cpp ..\src\core\mesh_loader.cpp ^
  ..\src\render\foundation\gl_raii.cpp ^
  ..\src\render\streamlines\StreamlineSet.cpp streamline_direction_test.cpp ^
  ..\vendor\pugixml\pugixml.cpp ..\vendor\lz4\lz4.c ..\vendor\lzma\LzmaDec.c ..\vendor\lzma\7zAlloc.c ^
  -o streamline_direction_test.exe %INC% -lz
if errorlevel 1 ( echo [run_tests] COMPILE FAILED & exit /b 1 )
echo [run_tests] running streamline_direction_test...
streamline_direction_test.exe > last_direction.log 2>&1
type last_direction.log
if errorlevel 1 set FAIL=1
:: --- isosurface (marching cubes) regression ---
echo [run_tests] building isosurface_test...
%GPP% -std=c++20 -O2 -g ..\src\core\vtk_parser.cpp ..\src\core\vtk_xml_parser.cpp ..\src\core\vtk_common.cpp ..\src\core\mesh_utils.cpp ^
  ..\src\core\stl_parser.cpp ..\src\core\obj_parser.cpp ..\src\core\mesh_loader.cpp ^
  ..\src\core\isosurface.cpp ..\vendor\pugixml\pugixml.cpp ^
  ..\vendor\lz4\lz4.c ..\vendor\lzma\LzmaDec.c ..\vendor\lzma\7zAlloc.c ^
  isosurface_test.cpp ^
  -o isosurface_test.exe %INC% -lz
if errorlevel 1 ( echo [run_tests] COMPILE FAILED & exit /b 1 )
echo [run_tests] running isosurface_test...
isosurface_test.exe > last_isosurface.log 2>&1
type last_isosurface.log
if errorlevel 1 set FAIL=1
:: --- PVD collection parser regression ---
echo [run_tests] building pvd_test...
%GPP% -std=c++20 -O2 -g ..\src\core\pvd_parser.cpp ..\src\core\vtk_xml_parser.cpp ..\src\core\vtk_parser.cpp ..\src\core\vtk_common.cpp ..\src\core\mesh_utils.cpp ^
  ..\src\core\stl_parser.cpp ..\src\core\obj_parser.cpp ..\src\core\mesh_loader.cpp ^
  ..\vendor\pugixml\pugixml.cpp ..\vendor\lz4\lz4.c ..\vendor\lzma\LzmaDec.c ..\vendor\lzma\7zAlloc.c ^
  pvd_test.cpp ^
  -o pvd_test.exe %INC% -lz
if errorlevel 1 ( echo [run_tests] COMPILE FAILED & exit /b 1 )
echo [run_tests] running pvd_test...
pvd_test.exe > last_pvd.log 2>&1
type last_pvd.log
if errorlevel 1 set FAIL=1
:: --- animation colormap-range accumulator rules ---
echo [run_tests] building anim_range_test...
%GPP% -std=c++20 -O2 -g anim_range_test.cpp -o anim_range_test.exe %INC%
if errorlevel 1 ( echo [run_tests] COMPILE FAILED & exit /b 1 )
echo [run_tests] running anim_range_test...
anim_range_test.exe
if errorlevel 1 set FAIL=1
:: --- vector-field name dedup rules ---
echo [run_tests] building field_names_test...
%GPP% -std=c++20 -O2 -g field_names_test.cpp ..\src\core\FieldResolver.cpp ^
  -o field_names_test.exe %INC%
if errorlevel 1 ( echo [run_tests] COMPILE FAILED & exit /b 1 )
echo [run_tests] running field_names_test...
field_names_test.exe
if errorlevel 1 set FAIL=1
if %FAIL% NEQ 0 ( echo [run_tests] SOME TESTS FAILED & exit /b 1 )
echo [run_tests] ALL TESTS PASSED
if not defined CI pause
