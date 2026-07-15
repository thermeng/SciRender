# RendererQT

Qt 6 + OpenGL viewer for scientific meshes. Loads VTK `STRUCTURED_GRID` /
`POLYDATA` and STL files, maps scalar point data to a surface colormap, and
draws optional vector glyphs. Includes a camera-relative Light Kit, axis triad
gizmo, screenshot export, and a QML control panel.

## Build

Requires **Qt 6** (Core, Gui, Qml, Quick, QuickControls2, OpenGLWidgets),
**OpenGL**, a C++20 compiler, CMake ≥ 3.16, and GLAD/GLM (vendored under
`vendor/`, no install needed).

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="<path-to-Qt6>"
cmake --build build -j4
```

The shaders in `src/shaders/` are copied next to the binary at build time, so
the program can run from the build directory.

## Features

- Loads VTK `STRUCTURED_GRID` (curvilinear), `RECTILINEAR_GRID`,
  `STRUCTURED_POINTS`, `POLYDATA`, `UNSTRUCTURED_GRID` and STL
- Scalar surface coloring with colormaps; per-dataset **surface** tessellation
  for curvilinear grids (boundary shell, not the full volume)
- Vector field arrow glyphs (instanced, uniform-length)
- **Colorbar legend:** clean gradient bar with a user-controllable number of
  tick labels (`colorbarTicks`, 2–20) spread across the live data range;
  applies to both the scalar and vector-magnitude bars
- Clipping/slicing planes; clipping auto-resets to disabled on each new mesh load
- Camera-relative Light Kit: key/fill/back/head lights that track the view,
  key intensity + K-ratios, kit-wide warm tint
- Axis triad + light-direction markers in a corner overlay
- Screenshot export (PNG/JPEG/BMP), optional transparency
- QML side panel: lighting, slicing/clipping, colormap, presets, recent files

## Layout

| Path | Purpose |
|------|---------|
| `main.cpp`, `src/ui/` | Qt/QML entry point and viewport item |
| `src/renderer/` | OpenGL renderer and lighting |
| `src/camera/` | VTK-style camera (position/focal/up) |
| `src/mesh/` | VTK/STL parsers and loaders |
| `src/gizmo/` | Axis triad + light markers overlay |
| `src/shaders/` | GLSL vertex/fragment shaders |
| `vendor/` | GLAD, GLM |

## License

No license file yet — add one before publishing if you want it open source.
