# Unstructured volume, slice and isosurface rendering

Status: planned (3D CFD solve/save/export shipped in `f7f13bc`)

## Goal

Volume rendering, axis-aligned slice planes and isosurfaces work on the
unstructured 3D meshes produced by the CFD path (`datasetType == "CFD MESH"`,
`cellConnectivity` / `cellOffsets` / `cellTypes` populated), with the same UI
and controls as structured grids.

Success means a solved 3D case can be inspected without exporting to VTK:
the volume is masked to the real cells, slices show the field only where cells
exist, and the isosurface is a watertight surface at the requested level.

## Non-goals

- Arbitrary-orientation slice planes (not supported for structured grids either).
- Unstructured support for `.vtu` / legacy `.vtk` files: their parsers do not
  populate `RenderMesh::cellConnectivity` / `cellOffsets` / `cellTypes`
  (`vtk_xml_parser.cpp:967` uses the topology only for edge extraction). Only
  the CFD adapter feeds the new paths in this phase.
- Polyhedron cells (VTK type 42): no canonical corner order and no single
  face template. Such cells are skipped by the volume/iso builders; a case
  consisting only of them keeps the existing "not available" behavior.
- Time-accurate volume animation beyond rebuilding the texture on scalar
  changes (see Risks).

## Why it does not work today

The renderer keys everything off a structured grid:

- `RenderMesh::hasVolumeGrid()` (`src/core/mesh_loader.h:146`) requires
  `gridDimX/Y/Z > 1`; the CFD path leaves them 0.
- `VolumeTextureCache::textureForField` (`src/render/passes/VolumeTextureCache.cpp:24`)
  returns 0 when the dims are 0, and uploads `data->size()` floats into a
  `dimX*dimY*dimZ` texture without validating the count.
- `isosurface::canExtract` (`src/core/isosurface.cpp:50`) requires
  `hasVolumeGrid()`; `extractIsosurface` then indexes nodes with
  `x + y*dX + z*dX*dY`.
- `render_settings_apply.cpp` (CFD `applyLoadedMesh`) never calls
  `IsosurfaceController::setCurrentMesh`, so availability would stay stale
  even after `canExtract` learns about cells.
- The volume box is the mesh AABB (`renderer.cpp:1265-1269`), so an
  unstructured mesh would fog the whole bounding box.
- `VolumeSliceOverlay` draws a full AABB quad and samples the 3D texture with
  no notion of empty space (`volume_slice.frag:18`).
- `main_window.cpp:649-663` prints the structured-grid-required banner for
  every unstructured mesh.

## Design

### Data flow

```
CFD Mesh<3>
  -> RenderMesh (canonical cell order, cell types/centres)      [existing + phase 0]
  -> cell_topology::tets(cell)                                  [phase 1, shared]
       |-> isosurface::extractUnstructured  (per-cell marching tets, async)
       |-> volume::build  (derived voxel grid: cellId + tet per voxel, async)
                 |-> volume::scatter(field) -> R32F + R8 mask   (render thread)
                        |-> VolumePass / VolumeSliceOverlay
```

The derived voxel grid is geometry only: it does not change when a scalar
changes, so transient frames only re-scatter the field.

### Phase 0 - canonical cell vertex order

`ANSYSMeshReader::reconstructCells` builds 3D cells as the union of their face
vertices (`src/io/ANSYSMeshReader.cpp:411-419`). That order is arbitrary and
breaks the VTK hex/wedge/pyramid templates (and the VTU export, which writes
`Mesh<3>::cells()` in `VTKXMLWriter::writeCells_`).

Add `src/core/cell_topology.{h,cpp}` (CoreLib) with:

```cpp
namespace cell_topology {
bool isVolumeCell(uint8_t type);                 // 10, 12, 13, 14
const std::vector<std::vector<int>>& faces(uint8_t type); // canonical local faces
// Reorder `verts` (global ids) to VTK order using the cell's face loops.
// Returns false when the loops do not match the expected type; `verts` is
// then left untouched and the caller marks the cell type 42.
// Face loops are the reader's per-face vertex lists (size_t ids).
bool canonicalize(std::vector<uint32_t>& verts, uint8_t type,
                  const std::vector<std::vector<std::size_t>>& faceLoops);
}
```

- tet: any 4-order is valid (VTK tetra has no face ambiguity); keep the loop
  order for consistency.
- hex: pick the face whose centroid is lowest along the cell's longest axis,
  order its loop CCW about the outward normal, then order the opposite face's
  loop to match vertex-to-vertex; the other four faces are implied. Validate
  that all 12 expected edges exist.
- wedge: order the two triangle faces CCW-outward and pair them by nearest
  centroid (slots 0-2 bottom, 3-5 top).
- pyramid: order the single quad face CCW-outward, apex last.

Call it from `reconstructCells` (Dim == 3) where `faces`/`faceVertices` are
available. `GmshReader` already emits canonical order; still run the validator
in the adapter test. `Mesh<3>` volume/centroid math is order-agnostic
(`Mesh.cpp:546-576`), so this changes ordering only.

Acceptance for this phase: VTU cell connectivity matches VTK order, adjacent
cells traverse shared faces in opposite directions, and `cellType3D` /
`Mesh<3>::cellVolume` are unchanged for `samples/cfd/pipe_3d.msh`.

### Phase 1 - per-cell tets (shared by volume and isosurface)

`cell_topology::tets(type)` returns tets as four **slot** ids. Slots
`0..n-1` are the canonical cell corners; slot `n` is the cell centroid.
Decomposition: for every canonical face, rotate the loop so it starts at the
smallest **global** vertex id and fan-triangulate it; each face triangle forms
one tet with the centroid. The caller maps slot ids to global ids and, for
slot `n`, to the cell centre / a virtual id.

The faces come from the canonical type templates in phase 0, not from the
reader: `RenderMesh` stores only flat `cellConnectivity`, so the templates are
the contract between the adapter and both builders.

Two properties matter:

- The centroid is inside a convex cell, so the fan tiles the cell exactly
  (the same argument `Mesh<3>` already uses for its divergence-theorem
  volume). Cells with a non-positive tet volume are skipped.
- The face rule uses global ids, so the same face is triangulated identically
  from both incident cells, regardless of their canonical local order. This
  is what makes the extracted surface watertight. A fixed 5/6-tet hex
  decomposition does **not** have this property (the body/face diagonal is
  chosen per cell) and must not be used for isosurfaces.

Tetrahedra are the one exception: a tet is its own decomposition (one tet of
its four corners, no centroid slot); no interior diagonal exists, so no
centroid is needed.

### Phase 2 - derived voxel grid (volume + slice)

New `src/core/unstructured_volume.{h,cpp}` (CoreLib, no GL):

```cpp
namespace volume {
struct BuildOptions { int maxDim = 192; size_t maxVoxels = 16u << 20; };

struct DerivedGrid {
    int dimX = 0, dimY = 0, dimZ = 0;
    glm::dvec3 origin{0.0}, extent{1.0}; // == mesh bounds
    std::vector<uint32_t> cellId;   // kEmpty = outside every cell
    std::vector<uint8_t>  tet;      // local tet inside cellId
    static constexpr uint32_t kEmpty = 0xFFFFFFFFu;
    bool empty() const { return dimX <= 0 || cellId.empty(); }
};

struct ScalarGrid {
    std::vector<float>   value;      // dimX*dimY*dimZ
    std::vector<uint8_t> occupancy;  // 1 where a cell exists
    std::array<std::vector<float>,3> planeMin, planeMax; // [axis][slice]
};

bool eligible(const RenderMesh&);   // 3D cells + scalars + 3 positive extents
void build(const RenderMesh&, const BuildOptions&, DerivedGrid& out);
void scatter(const RenderMesh&, const DerivedGrid&,
             const std::vector<float>* pointValues,
             const std::vector<float>* cellValues, ScalarGrid& out);
}
```

`eligible` must reject 2D CFD meshes (cell types 5/9/7, zero extent on at
least one axis) even though they carry `cellConnectivity`; otherwise the
Volume page would enable meaninglessly. Require all three extents above an
epsilon, not only z. Cache the 3D-cell count on the `RenderMesh` while the
adapter walks the cells (`std::size_t unstructuredVolumeCellCount`) so page
gating is O(1) per UI signal.

Build:

1. `s = max(extent)/maxDim`; `dim_i = clamp(ceil(extent_i/s)+1, 2, maxDim)`;
   if the product exceeds `maxVoxels`, raise `s` and recompute.
2. Fill `cellId` with `kEmpty`.
3. Read cells with the **end-offset** convention used by
   `mesh_utils::extrapolateCellToPoints`: cell `c` spans
   `cellOffsets[c-1] .. cellOffsets[c]` (start 0 for `c == 0`). This is not
   VTK's start-offset layout. Require `cellOffsets.size() == nCells` and
   `cellTypes.size() == nCells`; skip the builder otherwise.
4. For every volume cell, get its tets, take each tet's AABB, and for every
   voxel whose centre is inside the AABB run a sign-consistent barycentric
   test. Write `cellId`/`tet`. Shared boundaries are last-writer-wins
   (deterministic because cells are visited in index order).
5. Cells thinner than one voxel mark the voxel containing their centre so
   they cannot disappear.

Scatter (per field, on the texture build path):

- Values are indexed in the **pre-split vertex space**: `cellConnectivity`
  ids refer to `sourcePointCount` (the vertex array before
  `computeNormals` duplicated sharp-edge vertices), so do not use the
  `vertexSourceIndex`-expanded array for cell corners.
- Point placement: interpolate at the voxel centre from the stored tet's four
  corner values via barycentrics; clamp to the nearest corner if the point is
  epsilon-outside.
- Cell placement: use the cell's value directly (`attributes->cellScalars` or
  a snapshot scalar sized to the cell count). This is the common CFD case and
  is exact.
- Accumulate `planeMin/planeMax` for every axis/slice in the same pass, so the
  slice auto-range does not need a second scan.

Do not reuse `VectorTextureCache::buildGrid` directly: it inverse-distance
resamples into a regular grid and has no occupancy, which is exactly the
"fog in empty space" problem. The tiling/budget pattern is the model, not the
sampler.

Async ownership: `DerivedVolumeController` (`render/settings/`, mirroring
`IsosurfaceController`) builds on `QtConcurrent` with a generation token and
hands the finished `shared_ptr<const DerivedGrid>` to the renderer through a
mutex-guarded pending slot (same pattern as `setPendingMesh`). The renderer
consumes it at the start of the frame into
`std::shared_ptr<const volume::DerivedGrid> m_derivedVolume`, invalidates the
volume cache, and passes `m_derivedVolume.get()` into `textureFor`. Rebuild
only when the topology signature changes (vertex/cell counts + bounds + a
connectivity hash); scalar-only transient frames reuse the grid and only
re-scatter.

### Phase 3 - volume rendering

- `VolumeTextureCache` gains

  ```cpp
  struct VolumeTexture {
      GLuint scalar = 0;      // R32F
      GLuint occupancy = 0;   // R8, 0 for structured sources
      int dimX = 0, dimY = 0, dimZ = 0;
  };
  VolumeTexture textureFor(const RenderMesh*, const DerivedGrid*,
                           const std::string& field, ..., int placement,
                           const postfields::ComputedSnapshot*);
  std::optional<std::pair<float,float>> planeRange(int axis, int slice) const;
  ```

  The structured branch is unchanged. The derived branch reuses the
  `DerivedGrid`, scatters into a `ScalarGrid`, uploads `value` (exactly
  `dimX*dimY*dimZ` floats) and `occupancy`, and caches by
  field + placement + derived generation. `textureForField` becomes a thin
  wrapper or is migrated at both call sites (`renderer.cpp:1271,1283`).
- `VolumePass::draw` takes the `VolumeTexture`, binds the mask to unit 1 and
  sets `uHasOccupancy`.
- `volume.frag`: sample occupancy first; `if (uHasOccupancy == 1 &&
  texture(uOccupancyTex, uvw).r < 0.5) { advance; continue; }`. Guard the
  gradient taps with the same test. Add a small empty-space skip: after
  `k` consecutive empty samples, advance by `4 * stepSize` until data is
  found, so the 512-step budget is spent inside the mesh.
- `VolumeSliceOverlay::draw` takes `GLuint sliceOccupancy[3]` and binds it
  next to `sliceTex[axis]`.
- `volume_slice.frag`: `if (occupancy < 0.5) discard;` and keep the quad
  outline. Because the slice range now comes from the voxel scatter, move
  `Renderer::updateSliceScalarRange()` after the texture build loop and have
  it read `planeRange` for derived grids; structured grids keep the existing
  exact scan.

The renderer box stays the mesh AABB, and the derived grid spans the same
AABB, so `uvw = (pos - boxMin) / extent` needs no change. Like the structured
path, the mapping is node-centred and carries the existing half-texel offset;
at 192 voxels this is <0.3% of the box.

### Phase 4 - isosurface

- `canExtract`: keep the structured branch; add
  `volumeMesh.hasUnstructuredTopology() && has 3D cells && (point field
  resolvable || cell field resolvable)`.
- `extractIsosurface`: branch before the grid indexing

  ```cpp
  if (!volumeMesh.hasVolumeGrid() && volumeMesh.hasUnstructuredTopology())
      return extractUnstructured(volumeMesh, isovalues, field, placement, snap);
  ```

- `extractUnstructured`:
  - Resolve values: Vertex placement uses `FieldStore::scalarData(..., Vertex)`
    and indexes it by the connectivity id (pre-split `sourcePointCount`
    space, not the split vertex array); CellCenter placement first tries the
    exact cell array, else `mesh_utils::extrapolateCellToPoints` to get corner
    values (documented approximation: cell-centred data is averaged to nodes
    before contouring).
  - For each volume cell and each tet, classify the 4 corners against the
    isovalue. The 16 cases emit 0, 1 or 2 triangles (2-inside/2-outside is a
    quad split along its shorter diagonal). Emit a consistent winding per
    case; the finalize tail derives the smooth normals from it.
  - Vertex sharing: a `uint64_t` key `(min(a,b) << 32) | max(a,b)` over
    **global** vertex ids. The centroid gets a virtual id `nVerts + cellIndex`,
    so centroid edges weld within a cell (where they must) and never across
    cells (where they must not). The face rule from phase 1 guarantees the
    boundary edge points coincide on both sides.
  - Scalar value at a crossing vertex is the isovalue (same as the structured
    path); normals come from the shared finalize tail.
- Factor the existing tail (`isosurface.cpp:585-694`: bitwise weld, collapsed
  triangle removal, smooth normals, bounds, name/shrink) into
  `finalizeSurface(RenderMesh&, const RenderMesh& src, const std::string& field)`
  and call it from both paths.
- Keep the async controller unchanged; it already handles debounce, stale
  tokens and live frames. Only the predicate and the mesh handoff change.

### Phase 5 - UI wiring and copy

- `RenderSettings::hasVolumeData()`: structured check first, else
  `volume::eligible(*loadedMesh) && FieldStore::hasAnyScalar(...)`.
- `RenderSettings::applyLoadedMesh`: call
  `m_isoController.setCurrentMesh(loaded, resolvedScalar, placement, computed)`
  at the end (after the active scalar is resolved) so the Isosurface page
  updates after a CFD run/case load.
- `DerivedVolumeController::setCurrentMesh` from the same place, plus the
  transient topology-change path in `render_settings.cpp:1001-1098`.
- `Renderer::hasVolumeData()` (`renderer.h:607`) and the invalidation at
  `renderer.cpp:484` must use `volume::eligible()`, not
  `RenderMesh::hasVolumeData()`; otherwise per-frame scalar updates never
  rebuild the derived texture (`viewport_widget.cpp:219`).
- `main_window.cpp:649-663`: replace the "export to VTK instead" banner with
  a case-aware message: no 3D cells -> "slice/volume/isosurface need a 3D
  volume mesh"; no scalar field -> the existing scalar message. Nothing is
  said for a supported unstructured mesh.
- `volume_page.cpp`, `slice_plane_page.cpp`, `isosurface_page.cpp` and the
  toolbar need no logic changes once the predicates return true.
- Update the "CFD solve" section in `CONTEXT.md` (currently states volume,
  slice and isosurface are structured-only).

## Files

New:

- `src/core/cell_topology.{h,cpp}` — types, face templates, canonicalizer, tets.
- `src/core/unstructured_volume.{h,cpp}` — eligibility, voxelization, scatter.
- `src/render/settings/derived_volume_controller.{h,cpp}` — async build/handoff.
- `tests/core/cell_topology_test.cpp`
- `tests/core/unstructured_volume_test.cpp`
- `tests/ui/unstructured_render_test.cpp`

Modified:

- `src/io/ANSYSMeshReader.cpp` — canonicalize 3D cells.
- `src/cfd/cfd_mesh_adapter.cpp` — 3D cell count, keep canonical order.
- `src/core/mesh_loader.h` — `unstructuredVolumeCellCount` (+ helper).
- `src/core/isosurface.{h,cpp}` — predicate, unstructured extractor, finalize tail.
- `src/render/passes/VolumeTextureCache.{h,cpp}`, `VolumePass.{h,cpp}`.
- `src/render/overlays/VolumeSliceOverlay.{h,cpp}`.
- `src/shaders/volume.frag`, `src/shaders/volume_slice.frag`.
- `src/render/foundation/renderer.{h,cpp}` — derived-grid handoff, box/range,
  eligibility checks at 484/607, slice-range ordering.
- `src/render/settings/render_settings.h`,
  `render_settings.cpp`, `render_settings_apply.cpp`.
- `src/ui/main_window.cpp`, `CONTEXT.md`, `CMakeLists.txt`, `tests/CMakeLists.txt`.

## Test plan

Targeted labels only; do not run the full slow sweep.

- `tests/core/cell_topology_test.cpp`
  - hex/wedge/pyramid/tet canonicalization from shuffled face loops; all 12/9/8/6
    expected edges present; volume and centroid unchanged.
  - tet decomposition sum equals the cell volume for a warped hex and a wedge.
  - shared-face triangulation is identical from both incident cells (compare
    sorted triangle vertex-id sets).
- `tests/core/unstructured_volume_test.cpp`
  - one unit hex: occupancy count == `dimX*dimY*dimZ`, every voxel maps to the
    cell, scatter of a linear field is linear within tolerance.
  - two stacked hexes with different values: cell placement produces a sharp
    interface, point placement interpolates, no voxel outside the union is
    occupied.
  - rotated/sheared hex: voxels inside the cell's AABB but outside the cell
    are not occupied.
  - a tet-only mesh: volume == 1/6 of the enclosing box, surface extraction
    watertight.
  - resolution cap: a 300^3 request is clamped to `maxVoxels` without overflow.
- `tests/cfd/mesh_adapter_test.cpp` (extend)
  - `pipe_3d.msh` cells are canonical, `cellType3D` unchanged, cell volume
    unchanged, `unstructuredVolumeCellCount == nCells`.
  - VTU export written by `VTKXMLWriter` has VTK-ordered hex connectivity.
- `tests/core/isosurface_test.cpp` (extend)
  - unstructured hex stack, linear field: isosurface is planar at the expected
    coordinate and every edge is shared by exactly two triangles (watertight).
  - single tet: one isovalue crossing gives 1 triangle, level outside gives an
    empty surface.
  - cell-centered field: output is non-empty and lies within the field's range.
  - `canExtract` is false for a 2D CFD mesh (triangles/quads, z extent 0).
- `tests/ui/unstructured_render_test.cpp`
  - `cfd::loadMesh("samples/cfd/pipe_3d.msh")`: `renderMesh` has
    `unstructuredVolumeCellCount ==` its cell count, `volume::eligible()` is
    true, `isosurface::canExtract()` is true, and extracting at a mid-level
    yields a non-empty mesh.
  - `cfd::loadMesh("samples/cfd/CavityQuad_40x40.msh")`: `volume::eligible()`
    false (2D cells, one zero extent) and `canExtract()` false.
  - (This test stays at CoreLib/CfdLib level; `RenderSettings`/`RenderLib` pulls
    in QtWidgets + OpenGL and is not linked into the test binaries.)
- Structured regression: `scirender_core_tests` and `scirender_render_tests`
  must pass unchanged; existing `scirender_ui_tests` covers the 2D/3D CFD
  round-trips.

Verified on a real ANSYS 3D case: volume masked to the pipe/domain interior,
slice 0.5 on each axis shows data only inside the domain, isosurface closed.
Build < 300 ms for ~100k cells at the default resolution.

## Acceptance criteria

- A solved 3D CFD case enables Volume, Slice Plane and Isosurface with no
  banner; all three render correctly without exporting.
- No volume fog outside the cells; slices discard empty voxels; the isosurface
  is watertight at every tested level.
- 2D CFD and surface-only meshes behave exactly as before.
- Structured-grid rendering, colormaps, transfer functions, clip planes and
  slice auto-range show no regression.
- Transient playback does not rebuild the voxel topology per frame; volume
  textures follow scalar updates.
- Memory is bounded by `maxVoxels` (default 16M): grid ~80 MB, mask ~16 MB,
  R32F ~64 MB. Only fields that are actually displayed are scattered.

## Risks and mitigations

- **Canonicalization failure** on exotic ANSYS elements: validator returns
  false, cell type becomes 42, the cell is skipped by the render paths, and a
  one-time status line reports how many were skipped. No crash, no wrong
  geometry.
- **Non-convex/degenerate cells**: tet volume <= 0 is skipped; a small number
  of holes is preferable to inverted surfaces. Covered by the rotated-hex test.
- **Watertightness across face diagonals**: guaranteed by the global-id face
  rule; the shared-face test is the guard.
- **Centroid fan cost**: up to 12 tets for a hex vs 6. Acceptable for the
  default resolutions; the async isosurface path keeps the UI responsive.
- **Transient volume cost**: scatter is O(voxels) per dirty frame. If it
  stalls playback, throttle the volume rebuild (e.g. 5 Hz) or defer to
  pause/step; the isosurface live path already throttles this way.
- **Half-texel offset** in the node-centred texture mapping is shared with the
  structured path and is below one voxel.
- **Cell-centred isosurface** is nodal after averaging, so it is smoother than
  the true cell-wise field. Documented; point placement is exact.
- **Half-precision mask edges**: 1-voxel masking of sub-voxel features; the
  cell-centre fallback prevents cells from vanishing entirely.

## Follow-ups

- Populate `cellConnectivity`/`cellOffsets`/`cellTypes` in the `.vtu` and
  legacy `.vtk` parsers so non-CFD unstructured datasets get the same support.
- Polyhedron (type 42) support, including per-cell face export in the VTU writer.
- Arbitrary-orientation slice planes (shared with the structured path).
- Empty-space acceleration and a lower default resolution for very large meshes.
