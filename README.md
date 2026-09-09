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
Line Integral Convolution (LIC), volume ray-marching, and isosurface extraction
via marching cubes. Plays ParaView `.pvd` time-series collections as streamed
animations and exports them to MJPEG AVI videos or PNG frame sequences.

Features include a 5-direction PBR-calibrated lighting kit, axis triad overlay
with pole-view handling, axis-aligned clipping and slice planes, wireframe
overlay, GPU-compute level-of-detail (LOD), depth-peel order-independent
transparency (1–8 layers), orthographic/perspective projection, screenshot
export, and a rich Qt Widgets sidebar with per-page control panels.

## Download (no build needed)

For users who don't want to build from source:

- **Installer (recommended):** `SciRender-Setup.exe` from the latest
  [GitHub Release](../../releases) → Next → Finish → Start Menu `SciRender`.
- **Portable (no install, no admin):** `SciRender-win64.zip` from the same
  Release → Extract All → double-click `SciRender.exe`.

Then drag-drop a `.vtu`/`.vts`/`.vti`/`.vtp`/`.vtr`/`.vtm`/`.vtk`/`.stl`/`.obj`/`.pvd`
file onto the window, or use `File > Open Mesh`. No Qt install required; all
`Qt6` DLLs and `shaders/` are included via `windeployqt`.

## Build (for developers)

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

- Loads VTK XML (`.vtu`/`.vts`/`.vti`/`.vtp`/`.vtr`/`.vtm` multiblock),
  VTK legacy (`.vtk`), STL, and OBJ formats with auto-detected compression
  (ZLIB/LZ4/LZMA, `UInt32`/`UInt64` headers, `ASCII`/`BINARY`/`APPENDED`
  encodings, byte-order swap). `.vtm` merges `<DataSet file="..."/>` entries;
  `.pvd` time-series are streamed via the animation controller, not as a single
  mesh
- Scalar surface coloring with GPU colormap textures; per-dataset **surface**
  tessellation for curvilinear grids (boundary shell, not the full volume).
  Palettes: Turbo, Viridis, Inferno, CoolWarm, MutedCoolWarm, BWR, Cividis,
  Grayscale — each with reverse toggle and discrete bands (`0–32`)
- Vector field arrow glyphs with user-controllable density (stride), scaling,
  magnitude transform (linear/sqrt/log), and placement (vertex or cell-center).
  Supports per-vertex and per-cell vector fields; glyph colormap with independent
  magnitude and per-component (X/Y/Z) fixed-range windows and band counts
- **Line Integral Convolution (LIC):** dense vector-field texture (`VectorTextureCache`,
  3D `GL_RGB32F`, up to 4 entries LRU) convolved along streamlines in
  `surface_lic.frag`. Modes: `vectorVisMode` off/glyphs/LIC, steps `4–128`,
  step size `0.001–2.0`× diagonal, noise frequency `0.5–64`, grain
  `64/128/256/512`, enhanced 2-pass, LIC-only (unshaded), integrators
  Euler/Midpoint/RK4 (default RK4). `licNoiseTex` 3D, `GL_REPEAT`
- **Streamline integration** with configurable step size, max steps, direction
  (forward/backward/both), seeding modes (volume/surface/plane), plane counts
  U/V, plane position, jitter, ribbon width, taper factor, opacity, PBR shading
  (ambient/diffuse/specular/power), color modes (solid/magnitude/component X/Y/Z)
  and arrowheads (spacing/size)
- **Particle traces** with count, speed, size, and additive blending controls
- **Volume ray-marching** with step size, opacity, and colormap controls; 3D
  `GL_R32F` textures via `VolumeTextureCache` with PBO double-buffering and
  Beer-Lambert opacity
- **Volume slice overlay:** up to 3 axis-aligned textured quads (X/Y/Z) each with
  independent position (`0–1`), opacity, scalar field, show-colorbar toggle,
  and slice colormap (palette/reverse/bands + fixed-range window)
- **Isosurface extraction** via marching cubes with debounced (150 ms) async
  computation; isosurface surfaces are shaded by the colormap LUT with PBR
  lighting and participate in depth-peel transparency and LOD (own LOD path)
- **Colorbar legend:** GPU-composited colorbar overlay — sharp, borderless
  gradient bar (optional rounded background panel) + tick labels + title
  rendered into the viewport FBO so it is captured in screenshots.
  User-controllable tick count (`colorbarTicks`, 2–20) across the live data range.
  Independent colorbars for scalar, vector-magnitude/component, streamline
  magnitude/component, volume, and volume-slice data. Palette, reverse,
  fixed-range window and discrete bands are configured per colorbar via
  right-click → **Style…** dialog. Bar position/orientation/visibility are
  draggable and persisted per field; font family/bold/italic, scale, tick scale,
  length/thickness, and panel opacity are global style controls
- **Lighting system:** 5-direction light kit (key + fill + 2×back + head) that
  tracks the camera; key intensity, 3 K-ratios (`KF`/`KB`/`KH` relative to key),
  per-light azimuth/elevation, kit-wide warm tint (0 cold blue → 0.5 white →
  1 warm amber), 3 presets (Studio/CADFlat/Soft) and PBR material parameters
  (roughness, metallic, ambient/diffuse/specular)
- **Clipping:** axis-aligned clip planes with master `clipEnabled`, per-axis
  `clipEnabledX/Y/Z`, height sliders, per-axis invert toggles, and
  `crinkleClipMode` (geometry-shader clip vs fragment discard)
- **Slicing:** 3 independent slice planes (X/Y/Z) with bounds-aware sliders
  (see above) — distinct from clipping which discards geometry, slicing draws
  a textured quad
- **Scalar filter:** threshold filter (`filterMin`/`filterMax` + `filterEnabled`)
  via fragment discard, separate from clip planes
- **Axis triad overlay:** X/Y/Z coordinate triad in a corner viewport tracking
  camera rotation. Anti-aliased clip-space axis lines, solid-color conical tips,
  solid origin disc, and texture-atlas text labels. Handles pole views (±X/±Y/±Z)
  with end-on disc markers and label offset to avoid overlap. Configurable
  corner (4 positions) and size (96/128/160 px footprints), show/hide toggle
- **Viewport navigation:** left-drag orbit, right-drag pan (middle-drag alias
  retained), inverted wheel zoom, triad click-to-snap, fly-to-face animated
  transitions (400 ms ease-in-out), fit-all isometric framing
- **Light-direction markers:** visual markers in the gizmo corner showing the
  5 kit directions, tinted by the warm setting (toggle `showLightMarkers`)
- **Bounding box overlay:** axis-aligned bounding box (AABB) wireframe toggle
- **Quality overlays:** degenerate triangle, open-edge, and non-manifold edge
  visualization with exact vertex welding at 1e-8 tolerance and counts/watertight
  indicator
- **Wireframe / display:** wireframe overlay (`WireframePass`), flat/smooth shading
  toggle, cull modes (off/back/front), surface/point toggles, point size/opacity,
  line width, surface opacity, depth-peel layer count (1–8), orthographic vs
  perspective projection, auto-rotate
- **Screenshot export** (PNG/JPEG/BMP) with optional transparency, 5 resolution
  levels (Current/HD/FHD/2K/4K) and 3 AA presets (0/2/4 samples), colorbar and
  gizmo are captured
- **Recent files & Clear Mesh:** MRU list capped at 8 (persisted via `QSettings`);
  `File → Clear` frees GPU meshes, isosurface, volume/vector caches, LIC noise,
  streamline pending results, and `FieldResolver` derived-field cache, and resets
  scalar range (`0–1`), world bounds (`-10–10`, radius `1`), filter windows,
  and per-pass fixed-range overrides
- **PVD animation playback:** streams `.pvd` collections through an async,
  prefetching loader (3 concurrent parses, prefetch 8 ahead / keep 2 behind,
  LRU capped at 14 frames / 512 MB); transport controls (play/pause, step,
  timeline scrubbing with drag debouncing), FPS rate, loop toggle, speed
  multiplier; fixed-topology sequences take a scalar-only fast upload path
- **Animation colormap scaling:** *Whole sequence* holds one grow-only range
  across frames so colors and the colorbar never flicker; *Per frame*
  rescales to each frame's own extent. Ranges always describe the active
  field resolved per frame (point/cell/legacy/derived), reseeding on field
  switches mid-sequence
- **Animation export:** renders the loaded sequence offscreen to an MJPEG
  `.avi` video and/or numbered PNG frames with configurable resolution,
  JPEG quality, fps, and frame range (first/last), 30 s timeout; encoding
  overlaps capture on worker threads so exports stay fast regardless of viewport
  size (playback pauses for the duration)
- **Depth-peel transparency** for correct rendering of translucent surfaces
  (1–8 layers, default 4, per-layer `GL_DEPTH32F_STENCIL8`/`GL_RGBA8` FBOs,
  resolution-adaptive capping, UBO-driven composite)
- **FPS Head-up Display** (HUD) with smoothed frame-rate counter and particles
  animation time
- **Robust mesh loading:** sort-based vertex deduplication at `1/4096` tolerance,
  point-cloud support (`GL_POINTS` rendering via `renderAsPoints`), per-vertex
  or per-cell vector field support with `cellCenters`, and quality analysis
  (weld at `1e-8`)
- **Level-of-Detail (LOD):** GPU compute-shader vertex clustering decimation
  (3 dispatches: `lod.comp`/`lod_output.comp`/`lod_tris.comp`) that activates
  only while the camera is moving, debounced `0.14 s`, snaps back after
  `160 ms` settle; scalar field preservation on the decimated mesh.
  Eligibility: volumetric grids and surface meshes, ≥4000 vertices,
  `< 0.5×` triangles worthwhile, `kLodMaxCellsPerAxis 128`; multi-shell safety
  (`≤64` components, within-one-cell merge test)

### Level of Detail (LOD)

LOD uses three GPU compute shaders (`lod.comp` / `lod_output.comp` / `lod_tris.comp`,
HL `lod.comp` vertex clustering) to produce a coarser mesh only while the camera
is moving, debounced `0.14 s` and snaps back to full detail `160 ms` after motion
stops. It is used only when all of the following hold:

- **Dataset type:** volumetric grids (`STRUCTURED_GRID`, `RECTILINEAR_GRID`,
  `STRUCTURED_POINTS`) and surface meshes (`STL`, `POLYDATA`,
  `UNSTRUCTURED_GRID`).
- **Size:** at least 4000 vertices (`lodMinVertices`).
- **Worthwhile:** the decimated mesh has less than half the original triangles
  (`lodDecimateRatio` = 0.5).
- **Multi-shell safe:** a single connected solid is always supported. Meshes
  with multiple disconnected parts (`≤64` components) are only decimated when the
  parts are farther than one cluster cell (`kLodMaxCellsPerAxis 128`) apart;
  otherwise full resolution is used. Isosurface meshes have their own LOD path.

## Layout

| Path | Purpose |
|------|---------|
| `src/app/` | Application entry point (`main.cpp`, GPU preference) |
| `src/core/` | VTK/STL/OBJ parsers, `.pvd` collection parser, mesh loading, mesh-quality analysis, field-name resolution (`FieldResolver`), camera, colormap definitions, isosurface extraction (marching cubes) |
| `src/render/` | OpenGL renderer, lighting model (`LightingModel`, 5-dir kit), mesh/LOD upload (`MeshGLManager`/`LodScheduler`), vector glyphs, streamlines + `StreamlineController`, particles, volume pass + `VolumeTextureCache`/`VectorTextureCache`, LIC (`surface_lic.frag`), colormap manager, colorbar overlay, axis triad + light markers, bbox/quality overlays, wireframe pass, depth-peel transparency, screenshot capture, animation playback controller + AVI/PNG exporter |
| `src/shaders/` | GLSL vertex/fragment/geometry/compute shaders — `mesh.{vert,frag,clip.geo}`, `glyph.{vert,frag}`, `wire.{vert,geo,frag}`, `bbox.{vert,frag}`, `streamline.{vert,frag}`, `seed.{vert,frag}`, `particle.{vert,frag}`, `volume.{vert,frag}`, `volume_slice.{vert,frag}`, `quality_overlay.{vert,frag}`, `depth_peel.{vert,frag}`/`composite.{vert,frag}`, `surface_lic.frag`, `pbr_common.glsl`, LOD `lod.comp`/`lod_output.comp`/`lod_tris.comp` |
| `src/ui/` | Qt Widgets main window, 12 sidebar pages (lighting, clipping, slice planes, volume, scalar, vectors, streamlines, view/display, screenshot, mesh info, animation, isosurface), animation export dialog, viewport widget, colorbar style dialog with per-colorbar palette/fixed-range (sharp, borderless bar), recent-files (8) + Clear Mesh |
| `tests/` | Standalone regression harnesses — parsers, PVD collections, marching-cubes isosurface, streamline direction, animation range rules — driven by `run_tests.{bat,sh}` |
| `samples/` | VTK/STL fixture files used by the regression harness |
| `assets/` | Application icon |
| `vendor/` | GLAD (OpenGL loader), GLM (math), pugixml (XML parsing), LZ4 & LZMA (compression) |

## License

MIT license
