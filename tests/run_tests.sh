#!/usr/bin/env sh
# ---------------------------------------------------------------------------
# Configure, build and run the SciRender regression suite.
#
# The suite is a subdirectory of the main CMake project: it links the real
# CoreLib / FvmCore / CfdLib / RenderCore targets, so there is no separate
# test build system. The application is NOT built, which means the suite
# needs neither Qt nor an OpenGL context.
#
# Usage:
#   ./run_tests.sh                     run every test
#   ./run_tests.sh -LE slow            skip the slow CFD benchmark
#   ./run_tests.sh -R core.parse       run one test (or a group of tests)
# Extra arguments are passed straight through to ctest.
# ---------------------------------------------------------------------------
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$ROOT/build-tests"

if [ ! -f "$BUILD/CMakeCache.txt" ]; then
    echo "[configure] $BUILD"
    cmake -S "$ROOT" -B "$BUILD" \
        -DSCIRENDER_BUILD_APP=OFF \
        -DSCIRENDER_BUILD_TESTS=ON \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo
fi

echo "[build] test binaries"
cmake --build "$BUILD" --target scirender_core_tests scirender_fvm_tests scirender_render_tests

echo "[test] ctest"
exec ctest --test-dir "$BUILD" --output-on-failure "$@"
