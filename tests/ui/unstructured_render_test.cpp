// Unstructured rendering end-to-end precondition check.
//
// Volume rendering, axis-aligned slices and isosurfaces are derived from the
// cells of a CFD mesh instead of an IJK grid. This suite proves the real
// ANSYS sample satisfies the geometry preconditions, that a field scattered
// over the derived voxel grid actually occupies voxels, and that a contour can
// be extracted from the same mesh. It stays at CoreLib/CfdLib level: RenderLib
// needs QtWidgets + an OpenGL context and is not linked into the tests.

#include "doctest/doctest.h"

#include "cfd/cfd_mesh_adapter.h"
#include "core/isosurface.h"
#include "core/mesh_loader.h"
#include "core/unstructured_volume.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

std::string samplePath(const char* relative) {
    return std::string(SCIRENDER_SAMPLES_DIR) + "/" + relative;
}

// Nodal x + y + z over the pre-split vertex space (the space cellConnectivity
// indexes). A bare mesh has no result fields, which is exactly the state a
// solved case is in before its first result arrives.
void addSyntheticScalar(RenderMesh& mesh) {
    const std::size_t nSrc = mesh.sourcePointCount > 0
        ? static_cast<std::size_t>(mesh.sourcePointCount)
        : mesh.vertices.size() / 3;
    std::vector<float>& s = mesh.scalars;
    s.assign(nSrc, 0.0f);
    for (std::size_t i = 0; i < nSrc; ++i) {
        s[i] = mesh.vertices[i * 3] + mesh.vertices[i * 3 + 1] + mesh.vertices[i * 3 + 2];
    }
    mesh.scalarName = "synthetic";
    mesh.availableScalarNames = {"synthetic"};
}

} // namespace

TEST_CASE("ui.unstructured_render.pipe_3d_supports_volume_slice_isosurface") {
    const auto res = cfd::loadMesh(samplePath("cfd/pipe_3d.msh"));
    REQUIRE_MESSAGE(res.error.empty(), res.error);
    REQUIRE(res.renderMesh);
    REQUIRE(res.dim == 3);

    RenderMesh mesh = *res.renderMesh;
    CHECK(mesh.hasUnstructuredVolumeCells());
    // Geometry-only eligibility: volume and slices work before any result
    // field exists.
    CHECK(volume::eligible(mesh));
    CHECK_FALSE(mesh.hasScalarData());

    volume::DerivedGrid grid;
    REQUIRE(volume::build(mesh, volume::BuildOptions{}, grid));
    REQUIRE_FALSE(grid.empty());
    CHECK(grid.voxelCount() == static_cast<std::size_t>(grid.dimX)
                                  * static_cast<std::size_t>(grid.dimY)
                                  * static_cast<std::size_t>(grid.dimZ));

    // The pipe is a tube inside its AABB: the derived grid must be partially
    // occupied, not a solid box (that is the whole point of the mask).
    std::size_t occupied = 0;
    for (std::uint32_t id : grid.cellId) {
        if (id != volume::DerivedGrid::kEmpty) ++occupied;
    }
    CHECK(occupied > 0);
    CHECK(occupied < grid.voxelCount());

    // Scatter a nodal field into the grid: the volume texture path.
    addSyntheticScalar(mesh);
    REQUIRE(mesh.hasScalarData());
    volume::ScalarGrid scattered;
    volume::scatter(mesh, grid, &mesh.scalars, false, scattered);
    REQUIRE_FALSE(scattered.empty());
    std::size_t scatteredOccupied = 0;
    float mn = std::numeric_limits<float>::max();
    float mx = std::numeric_limits<float>::lowest();
    for (std::size_t i = 0; i < grid.voxelCount(); ++i) {
        if (!scattered.occupancy[i]) continue;
        ++scatteredOccupied;
        mn = std::min(mn, scattered.value[i]);
        mx = std::max(mx, scattered.value[i]);
    }
    CHECK(scatteredOccupied > 0);
    CHECK(mn < mx); // a real field, not a constant fill
    float planeMin = 0.0f, planeMax = 1.0f;
    CHECK(scattered.planeRange(0, grid.dimX / 2, planeMin, planeMax));
    CHECK(planeMin <= planeMax);

    // Isosurface through the same cells at the mid-level of the field.
    REQUIRE(isosurface::canExtract(mesh));
    const float iso = 0.5f * (mn + mx);
    const auto surface = isosurface::extractIsosurface(mesh, {iso});
    CHECK_FALSE(surface.vertices.empty());
    CHECK(surface.indices.size() % 3 == 0);
    CHECK(surface.normals.size() / 3 == surface.vertices.size() / 3);
    for (std::uint32_t i : surface.indices) CHECK(i < surface.vertices.size() / 3);
}

TEST_CASE("ui.unstructured_render.cavity_2d_is_not_a_volume_mesh") {
    const auto res = cfd::loadMesh(samplePath("cfd/CavityQuad_40x40.msh"));
    REQUIRE_MESSAGE(res.error.empty(), res.error);
    REQUIRE(res.renderMesh);
    REQUIRE(res.dim == 2);

    RenderMesh mesh = *res.renderMesh;
    // 2D cells carry connectivity but no volume: volume, slices and
    // isosurfaces must stay unavailable, exactly as before this change.
    CHECK_FALSE(mesh.hasUnstructuredVolumeCells());
    CHECK_FALSE(volume::eligible(mesh));

    addSyntheticScalar(mesh);
    CHECK_FALSE(isosurface::canExtract(mesh));

    volume::DerivedGrid grid;
    CHECK_FALSE(volume::build(mesh, volume::BuildOptions{}, grid));
    CHECK(grid.empty());
}
