<h1 align="center">SciRender</h1>

<p align="center">

  <img src="https://img.shields.io/badge/C%2B%2B-20-blue" />
  <img src="https://img.shields.io/badge/Qt-6-green" />
  <img src="https://img.shields.io/badge/OpenGL-4.6-orange" />
  <img src="https://img.shields.io/badge/CMake-%E2%89%A5_3.19-064F8C" />

  <br/>

  <img src="https://img.shields.io/badge/status-active-success" />

<table>
  <tr>
    <td align="center" width="50%">
      <video src="https://github.com/user-attachments/assets/dd3ff780-3794-4423-9b90-c11270f603b1" style="max-width:100%;" controls autoplay loop muted></video>
    </td>
    <td align="center" width="50%">
      <video src="https://github.com/user-attachments/assets/ada6ad70-fcd5-4333-a003-832757237655" style="max-width:100%;" controls autoplay loop muted></video>
    </td>
  </tr>
</table>

Qt 6 + OpenGL 4.6 scientific rendering application. Loads VTK XML (`.vtu`, `.vts`, `.vti`, `.vtp`, `.vtr`, `.vtm` multi-block), VTK legacy (`.vtk`), STL, and OBJ meshes, plus `.pvd` time-series collections. Maps scalar data to surface colormaps, draws instanced vector-field arrow glyphs, integrates streamlines, renders particle traces, Line Integral Convolution (LIC), volume ray-marching, and isosurface extraction via marching cubes. Streams `.pvd` animations and exports them to MJPEG AVI video or PNG frame sequences.

Includes a CFD module: a steady/transient finite-volume (SIMPLE/PISO) solver with k-omega SST turbulence, 2D/3D unstructured mesh support, boundary conditions (velocity inlet, pressure outlet, walls, symmetry), transient restart, and `.scfd` case save/load with full field and clock preservation.

Features include a 5-direction PBR-calibrated lighting kit, axis triad overlay with pole-view handling, axis-aligned clipping and slice planes, wireframe overlay, GPU-compute level-of-detail (LOD), depth-peel order-independent transparency (1–8 layers), orthographic/perspective projection, screenshot export, and a rich Qt Widgets sidebar with per-page control panels.

## Download (no build needed)

For users who don't want to build from source:

- **Installer (recommended):** `SciRender-Setup.exe` from the latest [GitHub Release](../../releases) → Next → Finish → Start Menu `SciRender`.
- **Portable (no install, no admin):** `SciRender-win64.zip` from the same Release → Extract All → double-click `SciRender.exe`.

Then drag-drop a `.vtu`/`.vts`/`.vti`/`.vtp`/`.vtr`/`.vtm`/`.vtk`/`.stl`/`.obj`/`.pvd`/`.scfd` file onto the window, or use `File > Open Mesh`. No Qt install required; all `Qt6` DLLs and `shaders/` are included via `windeployqt`.

## Build (for developers)

Requires **Qt 6** (Core, Gui, Widgets, OpenGLWidgets), **OpenGL 4.6**, a C++20 compiler, CMake ≥ 3.19, and **ZLIB** (system-installed or auto-detected via `ZLIB_ROOT`). GLAD, GLM, pugixml, LZ4, and LZMA are vendored under `vendor/`, no install needed.

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="<path-to-Qt6>"
cmake --build build -j4
```

The shaders in `src/shaders/` are copied next to the binary at build time, so the program can run from the build directory.

To reproduce the Release package locally:

```bash
cmake -G "MinGW Makefiles" -S . -B build-mingw -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw -j4
cmake --install build-mingw --prefix package
C:/Qt/6.11.1/mingw_64/bin/windeployqt.exe --release --compiler-runtime package/SciRender.exe
# ZIP: Compress-Archive package/* SciRender-win64.zip
# Setup: iscc installer/SciRender.iss /DAppVersion=0.1.0  # requires Inno Setup 6
```

## Features

**Mesh & visualization**
- Loads VTK XML (`.vtu`/`.vts`/`.vti`/`.vtp`/`.vtr`/`.vtm` multiblock), VTK legacy (`.vtk`), STL, and OBJ with auto-detected compression (ZLIB/LZ4/LZMA) and encodings (ASCII/BINARY/APPENDED).
- Scalar surface coloring with GPU colormap textures; palettes Turbo, Viridis, Inferno, CoolWarm, MutedCoolWarm, BWR, Cividis, Grayscale with reverse toggle and discrete bands.
- Vector-field arrow glyphs with stride, scaling, magnitude transform (linear/sqrt/log), placement (vertex/cell-center), and per-component fixed-range windows.
- Line Integral Convolution (LIC) dense vector-field texture with Euler/Midpoint/RK4 integration, step size, noise frequency, grain, and enhanced 2-pass modes.
- Streamline integration with configurable step size, max steps, direction, seeding modes, ribbon width, taper, opacity, PBR shading, color modes, and arrowheads.
- Particle traces with count, speed, size, and additive blending.
- Volume ray-marching with step size, opacity, and colormap; 3D `GL_R32F` textures with PBO double-buffering and Beer-Lambert opacity.
- Volume slice overlay: up to 3 axis-aligned textured quads (X/Y/Z) each with independent position, opacity, scalar field, and slice colormap.
- Isosurface extraction via marching cubes (debounced async computation) shaded by the colormap LUT with PBR lighting; participates in depth-peel transparency and LOD.
- Colorbar legend overlay (GPU-composited) with tick count, fixed-range window, palette/reverse/bands; independent colorbars per field.
- Clipping (axis-aligned planes), slicing (textured quads), scalar threshold filter, wireframe overlay, flat/smooth shading, cull modes, surface/point toggles, point/line width, surface opacity, orthographic/perspective projection, auto-rotate.
- Axis triad overlay with pole-view handling; bounding-box wireframe; quality overlays (degenerate triangle, open-edge, non-manifold edge).
- Screenshot export (PNG/JPEG/BMP, 5 resolution levels, 3 AA presets) capturing colorbar and gizmo.
- Recent files (MRU capped at 8) and Clear Mesh.

**Animation**
- `.pvd` time-series playback: async prefetching loader, transport controls (play/pause/step/scrub), FPS rate, loop, speed multiplier, fixed-topology fast upload path.
- Colormap scaling: whole-sequence grow-only range (no flicker) or per-frame rescale.
- Animation export: offscreen render to MJPEG `.avi` and/or numbered PNG frames with resolution, JPEG quality, fps, and frame range; overlaps capture on worker threads.

**Transparency & performance**
- Depth-peel order-independent transparency (1–8 layers) for translucent surfaces.
- GPU-compute LOD vertex clustering decimating the mesh while the camera moves.
- FPS head-up display with smoothed frame-rate counter.

## CFD module

SciRender includes a finite-volume CFD solver for steady and transient incompressible flow, exposed through the `File > Open Case` workflow (`.scfd` files) and a dedicated CFD page in the sidebar.

**Solver**
- SIMPLE (steady) and PISO (transient) pressure-velocity coupling on collocated unstructured grids, 2D and 3D.
- Momentum convection schemes: upwind, central, second-order upwind.
- Pressure correction with optional AMG or ILU/BiCGSTAB linear solvers; Rhie-Chow collocated interpolation; SIMPLEC option.
- k-omega SST turbulence model with wall functions optional; turbulence intensity and inlet k/omega per velocity-inlet patch.
- Transient: backward-Euler or BDF2 time stepping, adaptive CFL-limited dt, PISO outer correctors, non-orthogonal correctors, `.pvd`/`.vtu` per-step output.

**Boundary conditions**
- Velocity inlet, pressure outlet (with optional backflow suppression), outlet, no-slip wall, slip wall, symmetry, moving wall.
- Per-patch velocity (u/v/w), pressure, backflow values, and turbulence inlet values.

**Case I/O**
- `.scfd` case files store the mesh, all solver settings, boundary conditions, and optional result fields (velocity, pressure, turbulence) together with the transient clock and history so a paused or finished run can be restarted exactly where it stopped. Save while paused captures the live solver state; load restores fields, turbulence, the transient clock (t, dt, dtPrev, step), BDF2 history, and the monitor/residual history. Older `.scfd` files written before 3D support are still read and upgraded on re-save.

**Testing**
- The CFD fundamentals suite (`tests/cfd/`) unit-tests every FVM operator — mesh geometry, gradient, divergence, Poisson, convection schemes, pressure/velocity correction, boundary conditions, and residuals — against hand-derived physics contracts with tight tolerances (`1e-12` relative). It needs neither Qt nor OpenGL.

## Layout

| Path | Purpose |
|------|---------|
| `src/app/` | Application entry point (`main.cpp`, GPU preference) |
| `src/core/` | VTK/STL/OBJ parsers, `.pvd` collection parser, mesh loading, mesh-quality analysis, field-name resolution, camera, colormap definitions, isosurface extraction (marching cubes) |
| `src/cfd/` | CFD module: solver session, case codec, mesh adapter, result builder, settings bridges, solution snapshot |
| `src/fvm/` | Finite-volume solver core (SIMPLE/PISO, momentum, pressure correction, convection, diffusion, gradient, AMG, SST turbulence) |
| `src/field/` | Cell fields (`Field`, `VectorField`) with aligned buffers |
| `src/render/` | OpenGL renderer: lighting (5-dir kit), mesh/LOD upload, vector glyphs, streamlines, particles, volume pass, LIC, colormap manager, colorbar, axis triad, overlays, depth-peel, screenshot, animation playback + AVI/PNG exporter |
| `src/shaders/` | GLSL vertex/fragment/geometry/compute shaders and LOD compute shaders |
| `src/ui/` | Qt Widgets main window, 12 sidebar pages (including CFD case page), animation export dialog, viewport widget, colorbar style dialog |
| `tests/` | Regression suite (doctest + CTest) against `CoreLib`/`FvmCore`/`CfdLib`/`RenderCore`; no Qt or OpenGL needed |
| `tools/` | Developer CLI diagnostics (not part of the test suite) |
| `samples/` | VTK/STL fixtures and `.scfd`/`.msh` CFD samples |
| `assets/` | Application icon |
| `vendor/` | GLAD, GLM, pugixml, LZ4/LZMA, doctest |

## Tests

The regression suite is part of the main CMake project, so tests always run against the same sources as the application. A test-only configure builds neither the app nor Qt/OpenGL.

See **[tests/README.md](tests/README.md)** for the full guide.

```bash
cmake --preset tests
cmake --build --preset tests
ctest --preset tests --output-on-failure
```

Or:

```bash
tests/run_tests.bat        # Windows
tests/run_tests.sh         # Linux / macOS / Git Bash
```

Every doctest test case is its own CTest row, so a failure names the behaviour that broke:

```
12/81 Test #12: core.fields.overlapping_names_deduplicate ...........   Failed    0.01 sec
tests/core/field_names_test.cpp:34: ERROR: CHECK_MESSAGE(...) is NOT correct!
```

Useful filters:

| Command | Effect |
|---------|--------|
| `ctest --test-dir build-tests -L core` | core layer (parsers, fields, isosurface, camera) |
| `ctest --test-dir build-tests -L fvm` | fvm layer (AMG, transient settings, CFD benchmark) |
| `ctest --test-dir build-tests -L cfd` | CFD fundamentals suite |
| `ctest --test-dir build-tests -L render` | render-layer tests |
| `ctest --test-dir build-tests -LE slow` | skip the slow CFD benchmark (`--preset tests-fast`) |
| `ctest --test-dir build-tests -R "core.parse"` | name-matching tests only |
| `ctest --test-dir build-tests --output-on-failure` | print every failing assertion |

Build switches: `SCIRENDER_BUILD_APP` (default `ON`), `SCIRENDER_BUILD_TESTS` (default `ON`), `SCIRENDER_BUILD_TOOLS` (default `OFF`).

## License

MIT license
