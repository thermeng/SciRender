@echo off
setlocal
echo [run_tests] compiling parser core + running regression harness...
set GPP=
for %%P in ("C:\Qt\Tools\mingw1310_64\bin\g++.exe" "C:\Qt\Tools\mingw_64\bin\g++.exe") do (
  if not defined GPP if exist %%P set "GPP=%%P"
)
if not defined GPP ( echo ERROR: g++.exe not found in known Qt MinGW dirs & pause & exit /b 1 )
set PATH=C:\Qt\Tools\mingw1310_64\bin;%PATH%
set INC=-I.. -I..\src -I..\vendor -I..\vendor\glad\include
cd /d %~dp0
echo [run_tests] building parse_regression...
%GPP% -std=c++20 -O0 -g ..\src\core\vtk_parser.cpp ..\src\core\vtk_xml_parser.cpp ..\src\core\mesh_utils.cpp ^
  ..\src\core\stl_parser.cpp ..\src\core\obj_parser.cpp ..\src\core\mesh_loader.cpp parse_regression.cpp ^
  -o parse_regression.exe %INC%
if errorlevel 1 ( echo [run_tests] COMPILE FAILED & pause & exit /b 1 )
echo [run_tests] running parse_regression...
parse_regression.exe > last_parse.log 2>&1
type last_parse.log
echo.
echo [run_tests] building streamline_direction_test...
%GPP% -std=c++20 -O0 -g ..\vendor\glad\src\gl.c ..\src\core\vtk_parser.cpp ..\src\core\vtk_xml_parser.cpp ..\src\core\mesh_utils.cpp ^
  ..\src\core\stl_parser.cpp ..\src\core\obj_parser.cpp ..\src\core\mesh_loader.cpp ^
  ..\src\render\StreamlineSet.cpp streamline_direction_test.cpp ^
  -o streamline_direction_test.exe %INC%
if errorlevel 1 ( echo [run_tests] COMPILE FAILED & pause & exit /b 1 )
echo [run_tests] running streamline_direction_test...
streamline_direction_test.exe > last_direction.log 2>&1
type last_direction.log
:: ponytail: tee-by-hand — save to log (stdout+stderr), echo it back so screen still shows results.
if not defined CI pause
