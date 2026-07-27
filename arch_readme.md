# SciRender — Architecture Overview

## Project Summary

SciRender is a desktop scientific mesh visualization application built with **Qt 6 (QML) + OpenGL 3.3 Core**. It loads VTK, STL, and OBJ datasets, renders them with GPU-accelerated shaders, and exposes interactive controls via a declarative QML sidebar. The application enforces a strict **two-thread separation** between the GUI (Qt Quick scene graph) and the raw OpenGL render thread.

---

## Technology Stack

| Component | Technology |
|-----------|-----------|
| Language | C++20 |
| UI Framework | Qt 6 (Qml, Quick, QuickControls2, OpenGLWidgets) |
| Graphics API | OpenGL 3.3 Core Profile |
| OpenGL Loader | GLAD |
| Math | GLM (header-only) |
| Build System | CMake 3.16+ |

---

## High-Level Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     GUI Thread (Qt Quick)                     │
│                                                              │
│  Main.qml ──► ViewportVisualizer (QQuickFBO)               │
│                     │                                       │
│                     │ synchronize()                         │
│                     ▼                                       │
│              RenderSettings (QObject)                        │
│              - Q_PROPERTY facade for ~80 settings           │
│              - buildRenderState() → deep-copy snapshot      │
│              - Async mesh load via QtConcurrent             │
│              - Camera manipulation (GUI-thread copy)        │
└───────────────────────┬─────────────────────────────────────┘
                        │
                        │ RenderRenderState (value copy)
                        │ setState() / setPendingMesh()
                        ▼
┌─────────────────────────────────────────────────────────────┐
│                 Render Thread (QSG)                          │
│                                                              │
│  ViewportFboRenderer::render()                              │
│                     │                                       │
│                     ▼                                       │
│              Renderer (pure C++, no QObject)                 │
│              - owns GL programs, VAOs, VBOs                  │
│              - delegates to 4 helpers:                       │
│                  • MeshGLManager (GPU mesh + LOD)            │
│                  • ColormapManager (LUT textures)            │
│                  • VectorGlyphSet (instanced glyphs)         │
│                  • LightingModel (4-point light kit)        │
│              - draws overlays: Gizmo, ColorbarOverlay        │
│              - renderFrame() reads m_state snapshot only     │
└─────────────────────────────────────────────────────────────┘
```

---

## Directory Structure

```
src/
├── app/
│   └── main.cpp                  # Qt entry point; OpenGL 3.3 context setup
    ├── core/
    │   ├── Camera.h / .cpp           # VTK-style camera (azimuth, elevation, dolly, pan)
    │   ├── mesh_loader.h / .cpp      # Parser dispatcher (loadMeshFile)
    │   ├── mesh_utils.cpp            # Bounds, normals, endianness helpers
    │   ├── vtk_parser.cpp            # Legacy VTK (ASCII/binary, structured/unstructured)
    │   ├── vtk_xml_parser.cpp        # VTK XML (.vtu/.vts/.vti/.vtp/.vtr) — hand-rolled tokenizer, zero external deps
    │   ├── stl_parser.cpp            # ASCII + binary STL
    │   ├── obj_parser.h / .cpp       # Wavefront OBJ
    │   ├── Colormaps.h               # Colormap palette definitions
    │   └── mesh_quality.h            # Mesh quality analysis (degenerate faces, edges)
├── render/
│   ├── renderer.h / .cpp         # Pure-C++ render backend (thread-isolated)
│   ├── render_settings.h / .cpp  # GUI-thread QObject facade (~80 Q_PROPERTYs)
│   ├── render_config.h           # Compile-time render options
│   ├── LightingModel.h / .cpp    # 4-point light kit (key, fill, back, head)
│   ├── ColormapManager.h / .cpp  # Scalar + vector LUT texture management
│   ├── MeshGLManager.h / .cpp    # GPU mesh upload, LOD decimation, scalar re-upload
│   ├── VectorGlyphSet.h / .cpp   # Instanced arrow glyphs for VTK VECTORS
│   ├── gizmo.h / .cpp            # 3D coordinate triad overlay (billboarded text)
│   └── colorbar_overlay.h / .cpp # GPU-composited colorbar legend (QImage → texture)
├── ui/
│   └── custom_viewport_item.h / .cpp  # ViewportFboRenderer + ViewportVisualizer
ui/
└── Main.qml                      # Declarative UI (rail sidebar, panels, viewport)
src/shaders/
├── mesh.vert / mesh.frag         # Main mesh shading (Phong + colormap LUT)
├── grid.vert / grid.frag         # Reference grid (procedural ray-cast ground plane)
└── glyph.vert / glyph.frag       # Instanced vector arrow glyphs
vendor/
├── glad/                         # OpenGL function loader (static lib)
└── glm/                          # GLM header-only math library (INTERFACE target)
tests/
└── parse_regression.cpp          # Parser unit tests (no Qt/GL dependency)
```

---

## Threading Model

SciRender uses **strict thread isolation** to avoid race conditions and deadlocks between the Qt Quick GUI thread and the QSG render thread.

### GUI Thread Owns
- `RenderSettings` (QObject, Q_PROPERTY, signals/slots)
- All QML bindings and UI controls
- `Camera` copy (GUI-side source of truth)
- `RenderMesh` shared_ptr (immutable after parse)
- Async mesh parsing via `QtConcurrent::run`

### Render Thread Owns
- `Renderer` (pure C++, no QObject, no signals)
- All OpenGL resources: shader programs, VAOs, VBOs, EBOs, textures
- `MeshGLManager`, `ColormapManager`, `VectorGlyphSet`, `LightingModel`
- `Gizmo`, `ColorbarOverlay`

### State Transfer Protocol

```
GUI Thread                              Render Thread
     │                                      │
     │  buildRenderState()                  │
     │  → deep-copy RenderRenderState       │
     │  → publishRenderState()              │
     │  → setState(snapshot) ──────────────►│
     │                                      │  renderFrame()
     │                                      │  reads m_state ONLY
     │                                      │
     │  setPendingMesh(shared_ptr) ────────►│  renderFrame()
     │                                      │  uploads to GPU
     │  markScalarDirty(shared_ptr) ───────►│  updateScalarsOnGPU()
     │                                      │
     │  markCameraMoving() ────────────────►│  resets LOD debounce timer
```

**Key invariants:**
- The render thread never reads `RenderSettings` members directly.
- The GUI thread never reads or writes `Renderer` members directly.
- All cross-thread handoffs use **value copies** (snapshots) or **shared_ptr** (zero-copy reference handoff).
- `meshQueueMutex` protects the scalar handoff queue only (the snapshot copy itself is structurally safe).

---

## Core Data Structures

### RenderMesh (`src/core/mesh_loader.h`)
The central GPU-facing data structure. Produced by parsers, consumed by `MeshGLManager`.

```
RenderMesh
├── vertices:        float[]  (x,y,z interleaved)
├── indices:         uint32[] (triangle indices)
├── normals:         float[]  (nx,ny,nz interleaved)
├── scalars:         float[]  (active scalar field, per-vertex)
├── cellEdges:       float[]  (per-cell boundary edges, xyz line verts)
├── pointVectorsData: glm::vec3[] (contiguous vector field runs)
├── pointVectorOffset: unordered_map (field name → vec3 offset)
├── availableScalarNames: string[]
├── availableVectorNames: string[]
├── bounds:           BoundingVolume (double-precision)
├── flatVerts:        float[] (raw per-face corners for quality analysis)
├── sourcePointCount: int (true topological point count)
├── renderAsPoints:   bool (point cloud mode)
└── supportsCellGrid: bool (structured grid cell edges)
```

### RenderRenderState (`src/render/renderer.h`)
The per-frame snapshot that crosses the thread boundary.

```
RenderRenderState
├── camera:               Camera (position, focalPoint, viewUp, distance)
├── display flags:        showWireframe, showSurface, showGrid, showGizmo,
│                         showPoints, autoRotate, orthographic, etc.
├── render params:        pointSize, lineWidth, surfaceOpacity, cullMode
├── colors:               meshColor[3], surfaceColor[3], bgColor[3]
├── world bounds:         worldCenterX/Y/Z, worldRadius, worldMin/MaxX/Y/Z
├── lighting:             LightingModel (4-point kit + material)
├── colormap state:       colormapChoice, colormapReversed, vectorColormapChoice
├── scalar field:         meshHasScalars, scalarMin/Max, filterMin/Max,
│                         activeScalarName, showScalarColorbar
├── slice/clip:           clipEnabled, sliceHeightX/Y/Z, invertX/Y/Z
├── vector glyphs:        showVectors, vectorScale, vectorStride, vectorField
├── quality overlays:     qualityDegenerateTris, qualityOpenEdges, qualityNonManifoldEdges
└── screenshot:           screenshotTransparent
```

---

## Rendering Pipeline (per frame)

```
ViewportFboRenderer::render()
    │
    ├─ 1. synchronize() called by QSG
    │     ├─ consume pending mesh handoff → m_scene->setPendingMesh()
    │     ├─ consume pending scalar handoff → m_scene->markScalarDirty()
    │     ├─ consume pending screenshot path
    │     └─ publishRenderState() → deep-copy snapshot into m_scene
    │
    ├─ 2. renderFrame() (GL context current)
    │     │
    │     ├─ Upload pending mesh (if any) → MeshGLManager::upload()
    │     │     ├─ buildMeshGL() → full-res VAO/VBO/EBO/NBO/SBO
    │     │     └─ optional decimate() → LOD mesh via vertex clustering
    │     │
    │     ├─ Upload pending scalars (if dirty) → MeshGLManager::updateScalars()
    │     │     └─ orphan + refill SBO (no stall)
    │     │
    │     ├─ Update LUT textures (if dirty) → ColormapManager::update()
    │     │     └─ uploadLUT() → GL 1D texture
    │     │
    │     ├─ Rebuild vector glyphs (if dirty) → VectorGlyphSet::rebuild()
    │     │     └─ instanced arrow VAO (unit arrow + per-instance origin/direction)
    │     │
    │     ├─ Clear FBO → glClear(bgColor)
    │     │
    │     ├─ Draw grid (optional) → grid shader (procedural ray-cast ground plane)
    │     │
    │     ├─ Draw mesh (if loaded)
    │     │     ├─ Compute MVP (Camera view × proj × model)
    │     │     ├─ Compute light directions (LightingModel::computeDirections)
    │     │     ├─ Bind mesh shader program
    │     │     ├─ Set uniforms (MVP, lighting, colormap, slice/clip planes)
    │     │     ├─ snapshotDrawList() → select full or LOD mesh
    │     │     ├─ Draw triangles (GL_TRIANGLES, indexed)
    │     │     ├─ Draw points (optional, GL_POINTS)
    │     │     ├─ Draw wireframe (optional, GL_LINES)
    │     │     ├─ Draw cell edges (optional, GL_LINES from cellEdges VBO)
    │     │     └─ Draw bounds box (optional, GL_LINES)
    │     │
    │     ├─ Draw vector glyphs (optional) → glyph shader (instanced)
    │     │
    │     ├─ Draw gizmo (optional) → corner viewport with axis triad + light markers
    │     │
    │     └─ Draw colorbar legends (optional) → ColorbarOverlay::draw()
    │           └─ CPU-rendered QImage → GL texture → textured quad in FBO
    │
    └─ 3. Screenshot capture (if pending)
          └─ captureViewportToFile() → read FBO → QImage → native PNG/JPEG/BMP writer
```

---

## Subsystem Details

### Mesh Loading (Async)
```
User loads file (QML file dialog / drag-and-drop)
    │
    ▼
RenderSettings::loadMesh(path)
    │
    ├─ Save state to QSettings
    ├─ Increment load token (generation counter)
    ├─ QtConcurrent::run(parseVTK/parseSTL/parseOBJ/parseVTKXML)
    │     └─ Parse file → RenderMesh (CPU, no GL)
    │
    ▼ (callback on GUI thread)
RenderSettings::onMeshParsed()
    │
    ├─ Check token matches current (stale loads are discarded)
    ├─ Run mesh quality analysis (MeshQuality)
    ├─ Compute bounds (mesh_utils::computeBounds)
    ├─ Compute normals (mesh_utils::computeNormals)
    ├─ Store m_loadedMesh (shared_ptr, immutable)
    ├─ Extract GUI metadata (m_guiMeta)
    ├─ Update RenderSettings state (triangleCount, pointCount, etc.)
    ├─ setPendingMesh(m_loadedMesh) → handoff to render thread
    └─ emit meshLoadStateChanged()
```

### LOD (Level of Detail)
- `MeshGLManager` runs **vertex-clustering decimation** (coarse voxel grid, one representative per cell).
- Produces a separate `decimatedMeshes_` list alongside the full-resolution `meshes_`.
- While the camera is moving (`cameraMoving == true`), the renderer draws the decimated set.
- When the camera stops (debounce timer expires), the full-resolution set is drawn.
- Scalar fields on the LOD mesh are recomputed by averaging (`decimateScalars()`) using the same clustering.

### Vector Glyphs
- VTK VECTORS are stored per-point as `glm::vec3` runs in `RenderMesh::pointVectorsData`.
- `VectorGlyphSet::rebuild()` samples points at the configured stride, computes magnitude, and builds:
  - A **unit arrow** geometry (cylinder shaft + cone head, height 1, pointing +Y).
  - An **instanced buffer** of `[originX, originY, originZ, dirX, dirY, dirZ]` per instance.
- The glyph shader applies per-instance transforms with magnitude scaling (linear/sqrt/log).
- LUT-based magnitude coloring is optional (`vectorUseColormap`).

### Lighting Model
- **4-point light kit**: Key, Fill, Back, Head lights.
- Directions are specified in **camera-relative azimuth/elevation** (degrees).
- `computeDirections()` rotates kit-local directions into world space using the camera basis.
- Presets: Studio (default), CAD Flat, Soft.
- Material registers: ambient, diffuse, specular, shininess.
- Warm tint control (`lightWarm`) blends between cool and warm key-light color.

### Screenshots
- The viewport FBO is captured directly on the render thread (`captureViewportToFile`).
- Since the colorbar is rendered **inside** the FBO (not as a QML overlay), screenshots include the colorbar exactly as displayed.
- Format is inferred from file extension: PNG (with optional alpha), JPEG, BMP.

---

## Build Targets

| Target | Type | Purpose |
|--------|------|---------|
| `SciRender` | Executable | Main application |
| `parse_regression` | Test executable | Parser unit tests (no Qt/GL dependency) |

### Key CMake Details
- **Qt modules**: Core, Gui, Qml, Quick, QuickControls2, OpenGLWidgets
- **Vendor libs**: GLAD (static), GLM (INTERFACE/header-only)
- **Shader sync**: Post-build copies `src/shaders/` to `<build>/shaders/` for runtime loading
- **QML module**: `SciRenderUI` (URI), exposes `Main.qml` and shader resources
- **Test**: `enable_testing()` + `add_test()` for CTest integration

---

## Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| Raw OpenGL instead of Qt RHI | Need OpenGL 3.3 core for `#version 330` shaders; Qt RHI on Windows defaults to ANGLE/ES3. |
| `QQuickFramebufferObject` over `SGRenderNode` | Allows raw OpenGL with an explicit FBO; simpler lifecycle for screenshot capture. |
| Thread-isolated Renderer (no QObject) | Eliminates race conditions; snapshot copy is the only cross-thread boundary. |
| shared_ptr for mesh/scalar handoff | Zero-copy transfer; the GUI thread keeps the authoritative CPU payload alive. |
| Value-type RenderRenderState | Deep copy is cheap (~75 fields, no heap indirection); avoids mutexes on the snapshot. |
| LOD with camera-motion debounce | Balances interactivity (low poly while orbiting) and fidelity (full poly when静止). |
| Colorbar as FBO quad, not QML overlay | Ensures screenshots capture the colorbar; keeps it in GL coordinate space. |
| VTK legacy parser only | Structured/Unstructured Grid + PolyData (legacy .vtk); VTK XML formats handled separately by vtk_xml_parser.cpp. |
| GLM header-only | No linking overhead; used for all math (matrices, quaternions, vectors). |

---

## Dependencies Between Modules

```
RenderSettings ──owns──► Renderer
     │                       │
     │                       ├──► MeshGLManager ──uses──► RenderMesh (core)
     │                       ├──► ColormapManager
     │                       ├──► VectorGlyphSet ──uses──► RenderMesh
     │                       ├──► LightingModel
     │                       ├──► Gizmo
     │                       └──► ColorbarOverlay
     │
     ├──► Camera (GUI-thread copy)
     │
     └──► ViewportFboRenderer ──owns──► QOpenGLFramebufferObject

ViewportVisualizer (QQuickFBO) ──creates──► ViewportFboRenderer
Main.qml ──binds──► ViewportVisualizer
Main.qml ──binds──► backendSettings (RenderSettings*)
```

---

## Out of Scope

- Volume rendering / ray marching
- Multi-pass transparency (alpha blending is single-pass, order-dependent)
- Animation / time-varying datasets
- Network / remote rendering
- Mobile / OpenGL ES

---

## Testing

- `parse_regression` target runs parser tests without Qt/GL dependencies.
- Tests verify VTK, STL, and OBJ parsing correctness (indices, normals, bounds, scalar fields).
- Mesh quality analysis is validated for degenerate face, open edge, and non-manifold detection.
