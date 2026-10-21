@echo off
REM ---------------------------------------------------------------------------
REM Configure, build and run the SciRender regression suite.
REM
REM The suite is a subdirectory of the main CMake project: it links the real
REM CoreLib / FvmCore / CfdLib / RenderCore targets, so there is no separate
REM test build system. The application is NOT built, which means the suite
REM needs neither Qt nor an OpenGL context.
REM
REM Usage:
REM   run_tests.bat                     run every test
REM   run_tests.bat -LE slow            skip the slow CFD benchmark
REM   run_tests.bat -R core.parse       run one test (or a group of tests)
REM   run_tests.bat --output-on-failure  extra arguments are passed to ctest
REM ---------------------------------------------------------------------------
setlocal

set "ROOT=%~dp0.."
set "BUILD=%ROOT%\build-tests"

where cmake >nul 2>nul
if errorlevel 1 (
    echo [error] cmake was not found on PATH.
    exit /b 1
)

if not exist "%BUILD%\CMakeCache.txt" (
    echo [configure] %BUILD%
    cmake -S "%ROOT%" -B "%BUILD%" -DSCIRENDER_BUILD_APP=OFF -DSCIRENDER_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
    if errorlevel 1 exit /b 1
)

echo [build] test binaries
cmake --build "%BUILD%" --target scirender_core_tests scirender_fvm_tests scirender_render_tests
if errorlevel 1 exit /b 1

echo [test] ctest
ctest --test-dir "%BUILD%" --output-on-failure %*
exit /b %errorlevel%
