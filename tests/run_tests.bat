@echo off
set MINGW=C:\Qt\Tools\mingw1310_64\bin
set GPP=%MINGW%\g++.exe
set INC=-I.. -I..\src -I..\vendor
cd /d %~dp0
%GPP% -std=c++17 -O0 -g ..\src\core\vtk_parser.cpp ..\src\core\mesh_utils.cpp ^
  ..\src\core\stl_parser.cpp ..\src\core\mesh_loader.cpp parse_regression.cpp ^
  -o parse_regression.exe %INC%
if errorlevel 1 exit /b 1
parse_regression.exe
