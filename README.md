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

Qt 6 + OpenGL 4.6 scientific rendering application supporting the VTK XML formats
(`.vtu`, `.vts`, `.vti`, `.vtp`, `.vtr`, `.vtm` multi-block), VTK legacy
(`.vtk`), STL, and OBJ files. Maps scalar data to surface colormaps, draws
instanced vector-field arrow glyphs, integrated streamlines, particle traces,
volume ray-marching, and isosurface extraction via marching cubes. Plays
ParaView `.pvd` time-series collections as streamed animations and exports
them to MJPEG AVI videos or PNG frame sequences.

Features include a 4-point PBR-calibrated lighting kit, axis triad overlay
with pole-view handling, axis-aligned clipping/slicing planes, GPU-compute
level-of-detail (LOD), depth-peel order-independent transparency, screenshot
export, and a rich Qt Widgets sidebar with per-page
control panels.

## Build

Requires **Qt 6** (Core, Gui, Widgets, OpenGLWidgets), **OpenGL 4.6**,
a C++20 compiler, CMake ≥ 3.19, and **ZLIB** (system-installed or
auto-detected via `ZLIB_ROOT`). GLAD, GLM, pugixml, LZ4, and LZMA are
vendored under `vendor/`, no install needed.

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="<path-to-Qt6>"
cmake --build build -j4
```

The shaders in `src/shaders/` are copied next to the binary at build time, so
the program can run from the build directory.

## Features

- Loads VTK XML (`.vtu`/`.vts`/`.vti`/`.vtp`/`.vtr`/`.vtm` multiblock),
  VTK legacy (`.vtk`), STL, and OBJ formats
- Scalar surface coloring with GPU colormap textures; per-dataset **surface**
  tessellation for curvilinear grids (boundary shell, not the full volume)
- Vector field arrow glyphs with user-controllable density, scaling, magnitude
  transform (linear/sqrt/log), and placement (vertex or cell-center)
- **Streamline integration** with configurable step size, max steps, direction
  (forward/backward/both), seeding modes (volume/surface/plane), jitter, ribbon
  width, taper factor, and arrowheads
- **Particle traces** with count, speed, and size controls
- **Volume ray-marching** with step size, opacity, and colormap controls
- **Isosurface extraction** via marching cubes with debounced async computation;
  isosurface surfaces are shaded by the colormap LUT with PBR lighting and
  participate in depth-peel transparency and LOD
- **Colorbar legend:** GPU-composited colorbar overlay (gradient bar + tick
  labels + title) rendered into the viewport FBO so it is captured in screenshots.
  User-controllable tick count (`colorbarTicks`, 2–20) across the live data range.
  Supports independent colorbars for scalar, vector-magnitude, streamline,
  volume, and volume-slice data
- **Lighting system:** 4-point light kit (key/fill/back/head) that tracks the
  camera, key intensity + K-ratios (key/fill/back/head), kit-wide warm tint,
  PBR material parameters (roughness, metallic, ambient/diffuse/specular)/.
- **Slicing & clipping:** axis-aligned clip planes and slice planes with
  bounds-aware sliders, per-axis invert toggles
- **Axis triad overlay:** X/Y/Z coordinate triad in a corner viewport tracking
  camera rotation. Anti-aliased clip-space axis lines, solid-color conical tips,
  solid origin disc, and texture-atlas text labels. Handles pole views (±X/±Y/±Z)
  with end-on disc markers and label offset to avoid overlap
- **Light-direction markers:** visual markers in the gizmo corner showing the
  key/fill/back/head light directions, tinted by the warm setting
- **Bounding box overlay:** axis-aligned bounding box (AABB) wireframe
- **Quality overlays:** degenerate triangle, open-edge, and non-manifold edge
  visualization with exact vertex welding at 1e-8 tolerance
- **Screenshot export** (PNG/JPEG/BMP) with optional transparency, arbitrary
  resolution override, and AA sample presets
- **PVD animation playback:** streams `.pvd` collections through an async,
  prefetching loader with a bounded LRU frame cache; transport controls
  (play/pause, step), timeline scrubbing with drag debouncing, FPS rate and
  loop toggles; fixed-topology sequences take a scalar-only fast upload path
- **Animation colormap scaling:** *Whole sequence* holds one grow-only range
  across frames so colors and the colorbar never flicker; *Per frame*
  rescales to each frame's own extent. Ranges always describe the active
  field resolved per frame (point/cell/legacy/derived), reseeding on field
  switches mid-sequence
- **Animation export:** renders the loaded sequence offscreen to an MJPEG
  `.avi` video and/or numbered PNG frames with configurable resolution,
  JPEG quality, fps, and frame range; encoding overlaps capture on worker
  threads so exports stay fast regardless of viewport size (playback pauses
  for the duration)
- **Depth-peel transparency** for correct rendering of translucent surfaces
  (two-layer OIT with separate depth textures)
- **FPS Head-up Display** (HUD) with smoothed frame-rate counter
- **Robust mesh loading:** exact vertex deduplication at 1e-8 tolerance,
  point-cloud support (GL_POINTS rendering), per-vertex or per-cell vector
  field support, flat/smooth shading toggle, and configurable cull modes
  (off/back/front)
- **Level-of-Detail (LOD):** GPU compute-shader vertex clustering decimation
  that activates while the camera is moving, with multi-shell safety and
  scalar field preservation on the decimated mesh

### Level of Detail (LOD)

LOD uses a GPU compute shader (vertex clustering) to produce a coarser mesh
while the camera is in motion, then snaps back to full detail when motion stops.
It is used only when all of the following hold:

- **Dataset type:** volumetric grids (`STRUCTURED_GRID`, `RECTILINEAR_GRID`,
  `STRUCTURED_POINTS`) and surface meshes (`STL`, `POLYDATA`,
  `UNSTRUCTURED_GRID`).
- **Size:** at least 4000 vertices (`lodMinVertices`).
- **Worthwhile:** the decimated mesh has less than half the original triangles
  (`lodDecimateRatio` = 0.5).
- **Multi-shell safe:** a single connected solid is always supported. Meshes
  with multiple disconnected parts are only decimated when the parts are far
  enough apart that they cannot be merged; otherwise full resolution is used.

## Layout

| Path | Purpose |
|------|---------|
| `src/app/` | Application entry point (`main.cpp`, GPU preference) |
| `src/core/` | VTK/STL/OBJ parsers, `.pvd` collection parser, mesh loading, mesh-quality analysis, field-name resolution (`FieldResolver`), camera, colormap definitions, isosurface extraction (marching cubes) |
| `src/render/` | OpenGL renderer, lighting model, mesh/LOD upload, vector glyphs, streamlines, particles, volume pass, colormap manager, colorbar overlay, axis triad, bbox overlay, quality overlay, screenshot capture, depth-peel transparency, animation playback controller + AVI/PNG exporter |
| `src/shaders/` | GLSL vertex/fragment/compute shaders (mesh, glyph, bbox, streamline, seed, particle, volume, volume slice, quality overlay, LOD compute, depth peel, composite) |
| `src/ui/` | Qt Widgets main window, sidebar pages (lighting, slicing, view/display, scalar, vectors, streamlines, screenshot, mesh info, volume, animation), animation export dialog, viewport widget |
| `tests/` | Standalone regression harnesses — parsers, PVD collections, marching-cubes isosurface, streamline direction, animation range rules — driven by `run_tests.{bat,sh}` |
| `samples/` | VTK/STL fixture files used by the regression harness |
| `assets/` | Application icon |
| `vendor/` | GLAD (OpenGL loader), GLM (math), pugixml (XML parsing), LZ4 & LZMA (compression) |

## License

MIT license
