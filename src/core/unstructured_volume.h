#pragma once

#include "core/mesh_loader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

// Derived voxel grid for unstructured 3D meshes.
//
// Volume rendering and axis-aligned slice planes need a dense 3D texture, but
// an unstructured mesh has no grid. This module rasterizes the mesh's volume
// cells into a regular grid spanning the mesh AABB and records, per voxel,
// which cell and which tetrahedron of that cell contains the voxel centre. The
// result is geometry only: it does not change when a scalar field changes, so
// transient frames only re-scatter the field (`scatter`).
//
// Voxel (x,y,z) is centred at
//     origin + (x + 0.5, y + 0.5, z + 0.5) * extent / dims
// which is the same node-agnostic mapping the volume shader uses
// (`uvw = (pos - origin) / extent`). Empty voxels carry `DerivedGrid::kEmpty`
// so the shaders can skip them instead of fogging the whole bounding box.
namespace volume {

struct BuildOptions {
    // Longest grid axis, in voxels. 193^3 ~ 7.2M voxels.
    int maxDim = 192;
    // Hard cap on the total voxel count; the spacing is coarsened until the
    // grid fits, so the memory bound holds for pathological aspect ratios.
    std::size_t maxVoxels = 16u << 20;
};

struct DerivedGrid {
    int dimX = 0, dimY = 0, dimZ = 0;
    glm::dvec3 origin{0.0};
    glm::dvec3 extent{1.0};

    // Per voxel: owning cell (kEmpty = outside every volume cell) and the
    // local tetrahedron index inside that cell (as produced by
    // cell_topology::tets). Cells that did not produce a voxel (thinner than
    // one voxel) are pinned to the voxel containing their centre, with tet 0.
    std::vector<std::uint32_t> cellId;
    std::vector<std::uint8_t>  tet;

    // Voxels grouped by cell, so scatter tetrahedralizes each cell once:
    // cell c owns voxels [cellVoxelStart[c], cellVoxelStart[c + 1]).
    std::vector<std::uint32_t> cellVoxelStart;
    std::vector<std::uint32_t> cellVoxels;

    static constexpr std::uint32_t kEmpty = 0xFFFFFFFFu;

    bool empty() const { return dimX <= 0 || cellId.empty(); }
    std::size_t voxelCount() const { return cellId.size(); }
    int dim(int axis) const { return axis == 0 ? dimX : (axis == 1 ? dimY : dimZ); }

    std::size_t index(int x, int y, int z) const {
        return static_cast<std::size_t>(x)
             + static_cast<std::size_t>(y) * static_cast<std::size_t>(dimX)
             + static_cast<std::size_t>(z) * static_cast<std::size_t>(dimX)
                                          * static_cast<std::size_t>(dimY);
    }

    glm::dvec3 voxelCenter(int x, int y, int z) const {
        const glm::dvec3 d(dimX, dimY, dimZ);
        return origin + (glm::dvec3(x, y, z) + glm::dvec3(0.5)) * extent / d;
    }
};

// One scattered scalar field, laid out like the derived grid.
struct ScalarGrid {
    std::vector<float>   value;      // dimX*dimY*dimZ, valid where occupancy != 0
    std::vector<std::uint8_t> occupancy;

    // Per-axis slice min/max over the *occupied* voxels of that slice. An empty
    // slice leaves min/max inverted, so planeRange reports false for it.
    std::array<std::vector<float>, 3> planeMin;
    std::array<std::vector<float>, 3> planeMax;

    bool empty() const { return value.empty(); }

    bool planeRange(int axis, int slice, float& outMin, float& outMax) const {
        if (axis < 0 || axis > 2) return false;
        const auto& mn = planeMin[static_cast<std::size_t>(axis)];
        const auto& mx = planeMax[static_cast<std::size_t>(axis)];
        if (slice < 0 || static_cast<std::size_t>(slice) >= mn.size()) return false;
        if (!(mx[static_cast<std::size_t>(slice)] >= mn[static_cast<std::size_t>(slice)])) return false;
        outMin = mn[static_cast<std::size_t>(slice)];
        outMax = mx[static_cast<std::size_t>(slice)];
        return true;
    }
};

// Geometry-only eligibility: a hydrated unstructured topology with at least one
// tet/hex/wedge/pyramid cell and three positive extents. Scalar availability is
// intentionally not checked here -- callers gate the UI on both this and
// FieldStore (a bare CFD mesh is eligible but has nothing to display yet).
//
// This rejects 2D CFD meshes: they carry cell connectivity but their cells are
// triangles/quads and the z extent is zero.
bool eligible(const RenderMesh& mesh);

// Resolve point data onto the pre-split vertex space that
// `RenderMesh::cellConnectivity` indexes. FieldStore hands back the post-split
// copy (one value per rendered vertex) for attribute-backed fields, so a remap
// through `vertexSourceIndex` is usually needed. `scratch` owns the remapped
// copy. Returns false when `values` cannot be indexed against the connectivity.
bool sourcePointValues(const RenderMesh& mesh, const std::vector<float>& values,
                       std::vector<float>& scratch,
                       const std::vector<float>*& out);

// Rasterize `mesh` into `out`. Returns false (and leaves `out` empty) when the
// mesh is not eligible or the topology does not match the RenderMesh CSR.
bool build(const RenderMesh& mesh, const BuildOptions& options, DerivedGrid& out);

// Scatter one field into the derived grid.
//
// `cellPlacement == false` treats `values` as point data indexed in the
// pre-split vertex space (the space `RenderMesh::cellConnectivity` indexes);
// a post-split array is remapped through `vertexSourceIndex` when needed.
// `cellPlacement == true` treats `values` as one value per cell, which is the
// exact CFD case.
//
// `out` is resized to the grid; it comes back empty when `values` is null or
// too short for the requested placement.
void scatter(const RenderMesh& mesh, const DerivedGrid& grid,
             const std::vector<float>* values, bool cellPlacement,
             ScalarGrid& out);

} // namespace volume
