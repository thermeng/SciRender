# Architecture Review — SciRender

> Auto-generated architecture audit. Issues ordered by severity.

---

## Critical (Data Races / Correctness)

| # | Issue | Location |
|---|---|---|
| C1 | **Data race on `m_pendingScalarSrc`**: `updateScalarsOnGPU()` writes `m_pendingScalarSrc` without holding `meshQueueMutex`, racing with `markScalarDirty()` on the GUI thread which locks the mutex. | `renderer.cpp:524` vs `renderer.h:327` |
| C2 | **GL teardown while worker running**: `clearGpuMeshes()` calls `streamlineSet.shutdown()` (deletes GL buffers) without joining `streamlineWorker`. If the worker completes after shutdown, the next `uploadGL()` call operates on a teardown'd state. | `renderer.cpp:503-506` |

---

## High (Architecture / Maintainability)

| # | Issue | Location |
|---|---|---|
| H1 | **God Object**: `Renderer` handles 12+ independent responsibilities — shader compilation, UBO management, LOD dispatch, streamline threading, screenshot capture, camera reset, state management, grid/bbox/particle rendering, colorbar legend, lighting. At 502 header + 1289 impl lines, it violates SRP severely. | `renderer.h` entire file |
| H2 | **Q_PROPERTY bloat**: 127 Q_PROPERTY declarations require 4-site manual synchronization: (a) `RenderRenderState` struct, (b) `RenderSettings` member, (c) `Q_PROPERTY` block, (d) `buildRenderState()`. Adding any setting touches all 4. | `render_settings.h:116-255` |
| H3 | **Duplicated state**: `RenderSettings` GUI members (`showSurface`, `meshColor[3]`, `bgColor[3]`, etc.) mirror `RenderRenderState` fields 1:1. `buildRenderState()` is a 100-line manual copy (`render_settings.cpp:47-147`). Should use `RenderRenderState` as the single source of truth. | `render_settings.h:608-734` |
| H4 | **No RAII for GL resources**: All 6 resource-owning classes (`StreamlineSet`, `VectorGlyphSet`, `ColormapManager`, `MeshGLManager`, `Gizmo`, `ColorbarOverlay`) use manual `shutdown()` calls with destructors `= default`. If `shutdown()` is missed, GL handles leak silently. | `StreamlineSet.h:16`, `VectorGlyphSet.h:19`, etc. |
| H5 | **Shader compilation boilerplate**: 5 identical ~30-line create/compile/link/check-delete blocks in `initShaders()`. Should be a single `GLuint compileProgram(vert, frag)` helper. | `renderer.cpp:170-341` |

---

## Medium (Design / Code Quality)

| # | Issue | Location |
|---|---|---|
| M1 | **StreamlineSet conflates concerns**: Holds GL handles (`vao`, `vbo`), CPU computation results (`paths`), and animation state (`particles`, `particleRng`) in one class. `StructuredGridInfo` (10-field struct) is public but only used inside `compute()`. | `StreamlineSet.h:18-106` |
| M2 | **Lazy UBO creation scattered across `renderFrame()`**: `meshUbo` (line 959), `glyphUbo` (line 1127), `streamlineUbo` (line 1160), `gridUbo` (line 380) are created on first use deep in the draw loop. Should be created alongside their programs in `initShaders()`. | `renderer.cpp:380,959,1127,1160` |
| M3 | **`ChangeFlag` enum mostly unused**: Exists for granular dirty signaling (Display, Lighting, Colormap, etc.) but most setters emit `ChangeFlag::All`, defeating the purpose. | `render_settings.h` (most setters default to `ChangeFlag::All`) |
| M4 | **Duplicated mesh data**: `MeshData` struct is defined but `RenderSettings` keeps parallel members for most of the same data. `m_lastUploadedMesh`, `fullSource_`, and `m_loadedMesh` each hold `shared_ptr<const RenderMesh>`, tripling CPU memory for large meshes. | `render_settings.h:51-97,641-669` |
| M5 | **Inconsistent GL API usage**: Particle VAO uses legacy `glGenVertexArrays`/`glGenBuffers` while everything else uses DSA `glCreateVertexArrays`/`glCreateBuffers`. | `renderer.cpp:1231` vs everywhere else |
| M6 | **Flat CMake with duplicated source lists**: All sources in a single `add_executable`. Test targets re-list parser sources. No static libraries for `core/` or `render/` — every file change recompiles the entire project. | `CMakeLists.txt:44-79,151-176` |
| M7 | **Mixed logging**: `printf()`, `std::cerr`, `qDebug()`, `qWarning()` used interchangeably with no abstraction layer. | `renderer.cpp:43,114,106` etc. |
| M8 | **Synchronous `compute()` blocks render thread on first mesh load**: `uploadMesh()` calls `rebuild()` → `compute()` synchronously. Only subsequent dirty-triggered rebuilds use the async path. | `StreamlineSet.cpp:989-997` via `renderer.cpp:431` |

---

## Low (Style / Polish)

| # | Issue | Location |
|---|---|---|
| L1 | **Inconsistent atomic usage**: `m_destroying` is a plain `bool` while all sibling flags (`streamlineDirty`, `vectorGlyphDirty`, etc.) are `std::atomic<bool>`. | `renderer.h:471` |
| L2 | **Inconsistent indentation in CMakeLists.txt**: Some source paths have extra leading spaces. | `CMakeLists.txt:59-61` |
| L3 | **No `.clang-format` or `.clang-tidy`**: Formatting is inconsistent (braces, spacing, naming). | Project root |
| L4 | **Static `char log[511]` for shader errors**: Could truncate long messages. Should use `std::string` or dynamic allocation. | `renderer.cpp:41,151,187,224,255,289,324,362` |
| L5 | **Overallocation in `compute()`**: `estimatedSegments * 6 * 9` floats reserved upfront can be a massive overallocation for meshes with many seed points. | `StreamlineSet.cpp:717` |
| L6 | **No render-layer unit tests**: Only parser tests exist. No tests for `Renderer`, `MeshGLManager`, `ColormapManager`, or `StreamlineSet`. | `tests/` |

---

## Recommendations (Priority Order)

1. **Fix C1 & C2** — actual data races that can cause crashes or GL corruption.
2. **Extract shader helper** (H5) — quick win, reduces `initShaders()` by ~120 lines.
3. **Introduce `RenderLib` / `CoreLib` CMake targets** (M6) — enables incremental builds and test linking.
4. **Collapse `RenderSettings` ↔ `RenderRenderState` duplication** (H3) — biggest long-term maintainability win.
5. **Wrap GL handles in RAII types** (H4) — `UniqueGLBuffer`, `UniqueGLVertexArray` that call `glDelete*` in destructors.
6. **Split `Renderer` into focused managers** (H1) — e.g. `ShaderManager`, `ScreenshotCapture`, `StreamlineController`, `ParticleAnimator`.
7. **Wire up `ChangeFlag` properly** (M3) — avoid full-state copies on every property change.
8. **Add `.clang-format`** (L3) — enforce consistent formatting.
