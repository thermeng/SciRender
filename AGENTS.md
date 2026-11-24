# SciRender — agent notes

Qt 6 + OpenGL 4.6 scientific rendering app with a finite-volume CFD module. C++20, CMake ≥ 3.19, Ninja generator.

## Build the app

Qt 6 is required (Core, Gui, Widgets, OpenGLWidgets). The presets hardcode the local Qt/MinGW layout:

```
cmake -G Ninja -S . -B build -DCMAKE_PREFIX_PATH="<path-to-Qt6>"
cmake --build build -j4
```

Shaders in `src/shaders/` are copied next to the binary at build time (POST_BUILD), so the app runs from the build directory.

Release package (reproduces the installer/ZIP):

```
cmake -G Ninja -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j4
cmake --install build-release --prefix package
C:/Qt/6.11.1/mingw_64/bin/windeployqt.exe --release --compiler-runtime package/SciRender.exe
# then iscc installer/SciRender.iss /DAppVersion=0.1.0  (Inno Setup 6)
```

## Build switches

| Option | Default | Meaning |
|---|---|---|
| `SCIRENDER_BUILD_APP` | ON | Qt/OpenGL application |
| `SCIRENDER_BUILD_TESTS` | ON | doctest + CTest regression suite (CPU-only) |
| `SCIRENDER_BUILD_TOOLS` | OFF | developer CLI diagnostics, not in the suite |

A test-only configure needs neither Qt nor OpenGL (`SCIRENDER_BUILD_APP=OFF`).

## CMake presets

`debug`, `release`, `tests` (visible presets). `vendor/cgns/` and `vendor/hdf5/` are gitignored — fetched on demand via `python tools/build_vendored_deps.py`, never committed.

All builds cap parallelism at `-j4` to limit resources:

- `debug`: `cmake --preset debug && cmake --build --preset debug -- -j4`
- `release`: `cmake --preset release && cmake --build --preset release -- -j4`
- `tests` (no Qt/OpenGL, no CGNS rebuilds; runs against prebuilt vendored libs): `cmake --preset tests && cmake --build --preset tests -- -j4 && ctest --preset tests --output-on-failure`

The vendored deps script must run once before `tests`/`debug-vendored`/`release-vendored` presets that consume `build-vendored-lib/install`.

The AMG solver is self-contained (cached aggregation); no Hypre/BoomerAMG dependency.

## No fast math

`FvmCore` is deliberately NOT built with `-ffast-math` / `/fp:fast`. NaN/Inf guards in the solver would compile to `false` and divergence detection becomes dead code. Do not add `ffinite-math-only` / `/fp:fast`. GCC/Clang get `-fno-math-errno -funroll-loops -fomit-frame-pointer`; MSVC gets `/fp:precise`.

MinGW workaround: `-fexceptions` is added globally, and `NDEBUG` is stripped from Debug flags (never in Debug).

## Run the tests

One CTest row per doctest case; a failing row names the behaviour that broke.

Development is on Windows 11; use PowerShell/CMD commands. Log ctest output to a file so you can grep/pattern-search it afterward without re-running:

```
ctest --test-dir build-tests --output-on-failure | Tee-Object -FilePath tests.log
```

Then search the captured log instead of re-running:

```
Select-String -Path tests.log -Pattern "FAILED"            # find failures
Get-Content tests.log -Tail 100                            # last 100 lines
Select-String -Path tests.log -Pattern "core.parse"        # pattern search
```

Useful ctest filters (append to any ctest command):

```
ctest --test-dir build-tests -N             # list every row without running
ctest --test-dir build-tests -L core        # one layer
ctest --test-dir build-tests -L cfd         # CFD fundamentals
ctest --test-dir build-tests -LE slow       # skip the slow CFD cavity benchmarks (end-to-end runs, not unit tests)
ctest --test-dir build-tests -R "core.parse" # name pattern
```

Or run a single case directly:

```
build-tests/tests/scirender_cfd_tests --test-case="cfd.gradient.*"
build-tests/tests/scirender_cfd_tests --list-test-cases
```

Convenience script: `tests/run_tests.bat` (Windows); extra args pass straight to `ctest`.

Test layout: `tests/core/`, `tests/fvm/`, `tests/render/`, `tests/cfd/` — one file per operator area. Name cases `<layer>.<area>.<behaviour>`; the name is the CTest row name.

## Known-failure registry

`tests/cfd/known_failures.txt` lists cases known to deviate from the contract. Each runs through `cmake/expect_failure.cmake` which inverts the outcome: open defect → row passes (by failing); solver fixed → row turns red → remove the entry. A listed name matching no test case **fails the build**, so the registry cannot rot.

When fixing a solver operator, read `tests/cfd/README.md` (the contracts) first — it states what each operator is supposed to do and which parts are still open.

## CI

`.github/workflows/release.yml`: Windows-only, triggers on `v*` tags and `workflow_dispatch`. Builds the app **only** (`SCIRENDER_BUILD_TESTS=OFF`) — the test binaries crash on the runner (MinGW runtime mismatch, 0xc0000139) because CTest discovery loads them with a different toolchain than Qt was built with. Tests are not wired into CI. The job installs Qt 6.11.1 win64_mingw, Ninja via choco, runs `windeployqt`, copies MinGW runtime DLLs, builds the Inno Setup installer, and uploads the Setup + portable ZIP.

## Directory layout (what changes how you work)

- `src/app/` — app entry (`main.cpp`, GPU preference, CLI)
- `src/core/` — VTK/STL/OBJ/PVD parsers, mesh loading, colormaps, camera, isosurface (Qt-free)
- `src/cfd/` — CFD module (session, case codec, mesh adapter, result builder)
- `src/fvm/` — FVM solver core (SIMPLE/PISO, AMG, SST turbulence)
- `src/field/` — `Field`, `VectorField` with aligned buffers
- `src/render/` — OpenGL renderer (passes, overlays, exporters)
- `src/shaders/` — GLSL shaders, synced to the build dir post-build
- `src/ui/` — Qt Widgets main window, 12 sidebar pages, `.ui` forms in `src/ui/forms/`
- `tests/` — regression suite (doctest + CTest), support fixtures in `tests/support/`
- `tools/` — developer CLI diagnostics (not part of the test suite)
- `samples/` — VTK/STL fixtures and `.scfd`/`.msh` CFD samples (read-only; some tests write scratch under `samples/pvd_*`)
- `vendor/` — GLAD, GLM, zlib, eigen, pugixml, LZ4/LZMA, doctest (tracked); cgns/hdf5 fetched on demand (gitignored)

## Gotchas

- `.gitignore` has a case-insensitive `core` dump rule on Windows with an explicit negation for `vendor/eigen-3.4.0/Eigen/Core` — do not rename or drop that Eigen header.
- `.ui` forms use `CMAKE_AUTOUIC_SEARCH_PATHS` pointing at `src/ui/forms/`.
- The `tests/` tree is part of the main CMake project; tests always run against the same sources as the app.
- `.opencode/`, `.kilo/`, `.superpowers/`, `docs/`, `.vscode/`, `CONTEXT.md` are gitignored local tooling — not part of the repo.
- `.pvd` time-series animation export overlaps capture on worker threads; `.scfd` save-while-paused captures live solver state.