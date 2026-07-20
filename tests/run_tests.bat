@echo off
setlocal
:: ponytail: banner first so even a flashing window shows we started.
echo [run_tests] compiling parser core + running regression harness...
set GPP=
for %%P in ("C:\Qt\Tools\mingw1310_64\bin\g++.exe" "C:\Qt\Tools\mingw_64\bin\g++.exe") do (
  if not defined GPP if exist %%P set "GPP=%%P"
)
if not defined GPP ( echo ERROR: g++.exe not found in known Qt MinGW dirs & pause & exit /b 1 )
set INC=-I.. -I..\src -I..\vendor
cd /d %~dp0
%GPP% -std=c++17 -O0 -g -static ..\src\core\vtk_parser.cpp ..\src\core\mesh_utils.cpp ^
  ..\src\core\stl_parser.cpp ..\src\core\mesh_loader.cpp parse_regression.cpp ^
  -o parse_regression.exe %INC%
if errorlevel 1 ( echo [run_tests] COMPILE FAILED & pause & exit /b 1 )
parse_regression.exe > last_run.log 2>&1
type last_run.log
:: ponytail: tee-by-hand — save to log (stdout+stderr), echo it back so screen still shows results.
if not defined CI pause
