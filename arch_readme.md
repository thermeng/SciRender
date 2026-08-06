# Architecture Review — SciRender

> Last reviewed: 2026-08-01. Issues ordered by severity.

---

## Previously Fixed (No Longer Applicable)

| Issue | Original ID | What Changed |
|---|---|---|
| Data race on `m_pendingScalarSrc` | C1 | All accesses now guarded by `meshQueueMutex`: `markScalarDirty()`, `updateScalarsOnGPU()`, `cachedScalars()`. |
| GL teardown while worker running | C2 | `clearGpuMeshes()` now joins the worker before calling `streamlineSet.shutdown()`. Destructor also joins. |
| Shader compilation boilerplate | H5 | Extracted into `inline compileProgram()` in `shader_utils.h`. Called by `Renderer`, `GridRenderer`, `BBoxOverlay`, `StreamlineController` — zero duplication. |
| Flat CMake / no libraries | M6 | `CMakeLists.txt` defines `CoreLib`, `RenderLib`, and `SciRender` as separate static library targets. |
| Duplicated mesh metadata | M4 | `MeshData` struct consolidated: 12 standalone members moved into `m_meshData`. All getters read from `m_meshData.*`. |
| God Object: `Renderer` (partial) | H1 | Extracted 4 subsystems: `GridRenderer`, `BBoxOverlay`, `QualityOverlayRenderer`, `StreamlineController`. Renderer dropped from 1678 to 1099 total lines (34% reduction). `renderFrame()` delegates grid, bbox, quality overlay, streamline compute/draw, and seed draw. |
| Q_PROPERTY bloat (flag wiring) | H2 | All 74 bare `emit viewChanged()` calls replaced with specific `ChangeFlag` (Camera, Lighting, Colormap, Display, Slicing, Vectors). `publishRenderState()` skips copy when `m_stateDirty==false`. |
| Quality overlay unconditional copy | N4 | Quality overlay vectors are `shared_ptr<const vector<float>>`. `publishRenderState()` nulls them before copy when `showQualityOverlay==false`, restoring after. |

---

## High (Architecture / Maintainability)

| # | Issue | Location |
|---|---|---|
| H1r | **`Renderer` still owns mesh draw loop + particles + UBOs**: After extraction, `Renderer` is 397 header + 702 impl lines (~1099 total). Remaining responsibilities: mesh UBO fill, mesh draw loop (surface/wireframe/points), vector glyph draw, particle draw, screenshot capture, camera reset, gizmo/colorbar orchestration, animation clock. Still ~20 raw `GLuint` members. | `renderer.h:375-463`, `renderer.cpp` (702 lines) |
| H2 | **Q_PROPERTY bloat**: 136 `Q_PROPERTY` declarations. Each requires 4-site manual synchronization: (a) `RenderRenderState` struct, (b) `RenderSettings` member, (c) `Q_PROPERTY` block, (d) `publishRenderState()`. Adding any new setting requires touching all 4. Flag wiring is now correct (H2 fix) but the structural duplication remains. | `render_settings.h:90-255` |
| H3 | **Duplicated state (`RenderSettings` ↔ `RenderRenderState`)**: `RenderSettings` holds GUI-thread mirrors of every field in `RenderRenderState` (136+ properties). `publishRenderState()` performs a deep value-copy of the entire struct on every property change. The `RenderRenderState` struct itself contains heap-allocated `std::vector<float>` quality overlay data that is copied into the render-thread state — expensive for large meshes. | `render_settings.h:580-609`, `renderer.h:75-206` |
| H4 | **No RAII for GL resources**: All resource-owning classes (`StreamlineSet`, `VectorGlyphSet`, `ColormapManager`, `MeshGLManager`, `Gizmo`, `ColorbarOverlay`, `GridRenderer`, `BBoxOverlay`, `QualityOverlayRenderer`, `StreamlineController`) use manual `shutdown()` calls. Only `ColorbarOverlay` calls `shutdown()` from its destructor. If `shutdown()` is missed, GL handles leak silently. | `StreamlineSet.h:85`, `VectorGlyphSet.h:50`, `ColormapManager.h:44`, `gizmo.h:19`, `colorbar_overlay.h:25-28` |

---

## Medium (Design / Code Quality)

| # | Issue | Location |
|---|---|---|
| M1 | **StreamlineSet conflates concerns**: Holds GL handles (`vao`, `vbo`), CPU computation results (`paths`), and animation state (`particles`, `particleRng`) in one class. `StructuredGridInfo` (10-field struct) is public but only used internally by `compute()`. 848 impl lines. | `StreamlineSet.h:18-106`, `StreamlineSet.cpp` (848 lines) |
| M2 | **Lazy UBO creation scattered across `renderFrame()`**: `meshUbo` and `glyphUbo` are still created on first use deep in the draw loop. Grid and streamline UBOs are now managed by their respective subsystems (`GridRenderer`, `StreamlineController`). | `renderer.cpp` (mesh/glyph UBO sections) |
| M3 | **`ChangeFlag` enum mostly unused** → **Fixed**: All 74 bare `emit viewChanged()` replaced with specific flags (Camera, Lighting, Colormap, Display, Slicing, Vectors). Only `publishRenderState()` guards remain as optimization points. | `render_settings.h` (setters) |
| M7 | **Mixed logging**: `printf()`, `std::cerr`, `qDebug()`, `qWarning()` used interchangeably with no abstraction layer. | Throughout `renderer.cpp`, `render_settings.cpp`, parsers |
| M8 | **Synchronous `compute()` blocks render thread on first mesh load**: `uploadMesh()` calls `rebuild()` → `compute()` synchronously on the first load. Only subsequent dirty-triggered rebuilds use the async background thread path. | `StreamlineSet.cpp` via `renderer.cpp` |

---

## Low (Style / Polish)

| # | Issue | Location |
|---|---|---|
| L1 | **Inconsistent atomic usage**: `m_destroying` is a plain `bool` while all sibling flags (`streamlineComputeRunning`, `streamlineCancelFlag`, `particleCountDirty`, `scalarDirty`) are `std::atomic<bool>`. | `renderer.h` |
| L3 | **No `.clang-format` or `.clang-tidy`**: Formatting is inconsistent. | Project root |
| L4 | **Static `char log[511]` for shader errors**: Could truncate long messages. Should use `std::string` or dynamic allocation. | `shader_utils.h:19` |
| L5 | **Overallocation in `compute()`**: `estimatedSegments * 6 * 9` floats reserved upfront can be a massive overallocation for meshes with many seed points. | `StreamlineSet.cpp` |
| L6 | **No render-layer unit tests**: Only parser tests exist (`parse_regression`, `streamline_direction_test`). No tests for `Renderer`, `MeshGLManager`, `ColormapManager`, `StreamlineSet`, or the UI layer. | `tests/` |

---

## New Issues (Not in Prior Audit)

| # | Issue | Location |
|---|---|---|
| N1 | **UI God Object: `MainWindow`**: 99 header + 1583 impl lines (largest file in the codebase). Owns 6 `QDialog` pointers, 3 `QTimer`s, 3 `QComboBox`es, 8 section page builders, and all sidebar/quickbar/keyboard logic. | `main_window.h:30-114`, `main_window.cpp` (1583 lines) |
| N2 | **Full-state snapshot copy on every property change**: `publishRenderState()` performs a deep value-copy of the entire `RenderRenderState` struct (including `Camera`, `LightingModel`, and three `std::vector<float>` quality overlays) every time any single property changes. Quality overlay copy is now conditional (N4 fix). | `render_settings.h:publishRenderState()` |
| N3 | **`mesh_quality.h` is header-only but listed in CMake sources**: `mesh_quality.h` (250 lines) contains only inline functions and structs. Listing it in `CoreLib` sources is harmless but misleading. | `CMakeLists.txt:42` |
| N5 | **No signal/slot disconnection guard**: `connectSettings()` creates ~100+ signal-slot connections. No explicit disconnect or RAII connection management. | `main_window.cpp:connectSettings()` |

---

## Architecture (Current State)

### Renderer Decomposition

```
Renderer (397h + 702cpp = 1099 lines)
├── GridRenderer (19h + 60cpp)          — ground plane shader/VAO/VBO/UBO
├── BBoxOverlay (17h + 69cpp)           — AABB wireframe overlay
├── QualityOverlayRenderer (15h + 49cpp)— mesh defect highlights
├── StreamlineController (44h + 163cpp) — background compute + streamline/seed draw
├── ColormapManager (64h + 45cpp)       — scalar/vector/streamline LUT textures
├── VectorGlyphSet (48h + 159cpp)       — instanced arrow glyphs
├── StreamlineSet (93h + 848cpp)        — streamline GL resources + computation
├── MeshGLManager (114h + 672cpp)       — full + decimated GPU meshes
├── Gizmo (43h + 366cpp)               — coordinate triad + light markers
└── ColorbarOverlay (49h + 268cpp)      — GPU-composited colorbar legends
```

### File Inventory (render/)

| File | Lines | Responsibility |
|---|---|---|
| `renderer.h` | 397 | Renderer class + UBO structs (`MeshUBOData`, `GlyphUBOData`, `StreamlineUBOData`, `GridUBOData`) + `RenderRenderState` |
| `renderer.cpp` | 702 | Mesh draw loop, vector glyph draw, particle draw, screenshot, camera, gizmo/colorbar orchestration |
| `render_settings.h` | 561 | `RenderSettings` (Q_PROPERTY facade) + `MeshData` struct |
| `render_settings.cpp` | 476 | Setters, `publishRenderState()`, `onMeshParsed()`, `clearMeshes()` |
| `StreamlineSet.cpp` | 848 | Streamline CPU computation + GL upload + particle simulation |
| `MeshGLManager.cpp` | 672 | GPU mesh upload, LOD compute dispatch, decimated mesh |
| `gizmo.cpp` | 366 | Coordinate triad + light marker rendering |
| `colorbar_overlay.cpp` | 268 | Colorbar legend rendering |
| `StreamlineController.cpp` | 163 | Streamline background compute dispatch + draw |
| `VectorGlyphSet.cpp` | 159 | Vector glyph instanced rendering |
| `BBoxOverlay.cpp` | 69 | Bounding box wireframe |
| `GridRenderer.cpp` | 60 | Ground plane grid |
| `shader_utils.h` | 53 | Shared `compileProgram()` + `readShaderFile()` |
| `QualityOverlayRenderer.cpp` | 49 | Mesh quality defect overlay |
| `ColormapManager.cpp` | 45 | Colormap LUT texture management |
| `LightingModel.cpp` | 76 | 4-point light kit params + presets |

### Thread Model

| Thread | Objects | Operations |
|---|---|---|
| **GUI thread** | `RenderSettings`, `MainWindow`, `ViewportWidget` | Property changes → `publishRenderState()` → deep copy |
| **Render thread** | `Renderer` + all extracted subsystems | `renderFrame()` reads `m_state`, drives GL |
| **Background thread** | `StreamlineController::m_worker` | `StreamlineSet::compute()` (CPU-heavy, no GL) |

Cross-thread sync: `meshQueueMutex` (mesh/scalar handoff), `StreamlineController::m_resultMutex` (compute results), atomics for dirty flags.

### `renderFrame()` Call Graph (Post-Extraction)

```
renderFrame()
├── Animation clock + LOD debounce
├── Mesh handoff (meshQueueMutex)
├── m_streamlines.dispatchCompute()  ← StreamlineController
├── m_streamlines.consumeResult()    ← StreamlineController
├── GL state setup + projection matrix
├── Colormap update
├── Mesh UBO fill + draw loop (surface/wireframe/points)
├── m_qualityOverlay.draw()          ← QualityOverlayRenderer
├── m_bbox.draw()                    ← BBoxOverlay
├── Vector glyph draw
├── m_streamlines.draw()             ← StreamlineController (streamlines + seeds)
├── Particle draw
├── m_grid.draw()                    ← GridRenderer
├── drawGizmo()
└── drawColorbarLegends()
```

---

## Recommendations (Priority Order)

1. ~~**Fix C1 & C2**~~ — Done.
2. ~~**Extract shader helper** (H5)~~ — Done.
3. ~~**Introduce `RenderLib` / `CoreLib` CMake targets** (M6)~~ — Done.
4. ~~**Consolidate `MeshData`** (M4)~~ — Done.
5. ~~**Split `Renderer` into focused managers** (H1)~~ — Done. Grid, BBox, Quality, Streamlines extracted. Remaining: mesh draw loop, particles, screenshot, camera.
6. **Wrap GL handles in RAII types** (H4) — `UniqueGLBuffer`, `UniqueGLVertexArray` that call `glDelete*` in destructors.
7. **Split `MainWindow` into page widgets** (N1) — Each `build*Page()` should be a standalone `QWidget` subclass.
8. ~~**Wire up `ChangeFlag` properly** (M3)~~ — Done. All 74 setters now use specific flags.
9. ~~**Lazy-copy quality overlays** (N4)~~ — Done. `publishRenderState()` skips overlay vectors when `showQualityOverlay==false`.
10. **Add `.clang-format`** (L3) — Enforce consistent formatting.
