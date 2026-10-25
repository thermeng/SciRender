// volume::eligible / build / scatter — the derived voxel grid that lets volume
// rendering and slice planes work on an unstructured mesh.
//
// The invariants under test are the ones the shaders depend on:
//  * occupancy is 1 exactly where a volume cell contains the voxel centre, so
//    empty space around a mesh is skipped instead of fogged;
//  * each occupied voxel knows its cell (and tet), so cell-centred fields are
//    exact and point fields interpolate;
//  * the grid is bounded by maxVoxels for pathological resolutions.

#include "core/unstructured_volume.h"

#include "core/mesh_loader.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using Point = std::array<double, 3>;

RenderMesh makeMesh(const std::vector<Point>& points,
                    const std::vector<std::vector<std::uint32_t>>& cells,
                    const std::vector<std::uint8_t>& types) {
    RenderMesh mesh;
    mesh.datasetType = "CFD MESH";
    for (const auto& p : points) {
        mesh.vertices.push_back(static_cast<float>(p[0]));
        mesh.vertices.push_back(static_cast<float>(p[1]));
        mesh.vertices.push_back(static_cast<float>(p[2]));
    }
    for (std::size_t c = 0; c < cells.size(); ++c) {
        for (std::uint32_t v : cells[c]) mesh.cellConnectivity.push_back(v);
        mesh.cellOffsets.push_back(static_cast<std::uint32_t>(mesh.cellConnectivity.size()));
        mesh.cellTypes.push_back(types[c]);
    }
    mesh.unstructuredVolumeCellCount = 0;
    for (std::uint8_t t : types) {
        if (t == 10 || t == 12 || t == 13 || t == 14) ++mesh.unstructuredVolumeCellCount;
    }
    mesh.sourcePointCount = static_cast<int>(points.size());
    mesh_utils::computeBounds(mesh);
    return mesh;
}

// VTK-ordered unit cube with its min corner at `o` and edge length 1.
std::vector<Point> cubePoints(const Point& o = {0, 0, 0}) {
    const double x = o[0], y = o[1], z = o[2];
    return {
        {x, y, z}, {x + 1, y, z}, {x + 1, y + 1, z}, {x, y + 1, z},
        {x, y, z + 1}, {x + 1, y, z + 1}, {x + 1, y + 1, z + 1}, {x, y + 1, z + 1},
    };
}

volume::BuildOptions options(int maxDim) {
    volume::BuildOptions o;
    o.maxDim = maxDim;
    return o;
}

std::pair<int, int> voxelAt(const volume::DerivedGrid& grid, const Point& p) {
    // Returns x/z of the voxel whose centre is nearest to p (used for spot
    // checks in the sheared-hex test).
    int best[2] = {0, 0};
    double bestD = 1e300;
    for (int z = 0; z < grid.dimZ; ++z) {
        for (int x = 0; x < grid.dimX; ++x) {
            const glm::dvec3 c = grid.voxelCenter(x, 0, z);
            const double dx = c.x - p[0];
            const double dz = c.z - p[2];
            const double d = dx * dx + dz * dz;
            if (d < bestD) { bestD = d; best[0] = x; best[1] = z; }
        }
    }
    return {best[0], best[1]};
}

} // namespace

TEST_CASE("volume.one_hex_occupies_every_voxel") {
    const auto points = cubePoints();
    const RenderMesh mesh = makeMesh(points, {{0, 1, 2, 3, 4, 5, 6, 7}}, {12});

    REQUIRE(volume::eligible(mesh));

    volume::DerivedGrid grid;
    REQUIRE(volume::build(mesh, options(8), grid));
    REQUIRE_FALSE(grid.empty());
    CHECK(grid.voxelCount() ==
          static_cast<std::size_t>(grid.dimX) * grid.dimY * grid.dimZ);

    std::size_t occupied = 0;
    for (std::uint32_t id : grid.cellId) {
        if (id == volume::DerivedGrid::kEmpty) continue;
        ++occupied;
        CHECK(id == 0);
    }
    // The unit cube fills its own AABB, so every voxel centre is inside.
    CHECK(occupied == grid.voxelCount());
    CHECK(grid.cellVoxels.size() == grid.voxelCount());
    CHECK(grid.cellVoxelStart.size() == 2);
    CHECK(grid.cellVoxelStart[1] == grid.voxelCount());

    SUBCASE("cell placement is exact and constant") {
        const std::vector<float> cellValues = {2.5f};
        volume::ScalarGrid scattered;
        volume::scatter(mesh, grid, &cellValues, true, scattered);
        REQUIRE(scattered.value.size() == grid.voxelCount());
        for (std::size_t i = 0; i < grid.voxelCount(); ++i) {
            CHECK(scattered.occupancy[i] == 1);
            CHECK(scattered.value[i] == doctest::Approx(2.5f));
        }
        float mn = 0, mx = 0;
        REQUIRE(scattered.planeRange(0, grid.dimX / 2, mn, mx));
        CHECK(mn == doctest::Approx(2.5f));
        CHECK(mx == doctest::Approx(2.5f));
    }

    SUBCASE("point placement of a linear field is linear") {
        std::vector<float> pointValues;
        for (const auto& p : points) pointValues.push_back(static_cast<float>(p[0]));

        volume::ScalarGrid scattered;
        volume::scatter(mesh, grid, &pointValues, false, scattered);

        const double sx = grid.extent.x / grid.dimX;
        const double sy = grid.extent.y / grid.dimY;
        const double sz = grid.extent.z / grid.dimZ;
        double worst = 0.0;
        for (int z = 0; z < grid.dimZ; ++z) {
            for (int y = 0; y < grid.dimY; ++y) {
                for (int x = 0; x < grid.dimX; ++x) {
                    const std::size_t idx = grid.index(x, y, z);
                    REQUIRE(scattered.occupancy[idx] == 1);
                    const double expected = grid.origin.x + (x + 0.5) * sx;
                    worst = std::max(worst, std::fabs(scattered.value[idx] - expected));
                }
            }
        }
        CHECK(worst < 1e-3);
    }
}

TEST_CASE("volume.two_cells_have_a_sharp_interface_and_empty_space") {
    // Two diagonal unit cubes inside a 2x2x2 box: the grid spans the union's
    // AABB, but the two off-diagonal octants belong to no cell.
    std::vector<Point> points;
    auto addCube = [&](const Point& o) {
        const auto c = cubePoints(o);
        points.insert(points.end(), c.begin(), c.end());
    };
    addCube({0, 0, 0});
    addCube({1, 1, 1});

    const RenderMesh mesh = makeMesh(
        points,
        {{0, 1, 2, 3, 4, 5, 6, 7}, {8, 9, 10, 11, 12, 13, 14, 15}},
        {12, 12});
    REQUIRE(volume::eligible(mesh));

    volume::DerivedGrid grid;
    REQUIRE(volume::build(mesh, options(10), grid));
    REQUIRE(grid.voxelCount() > 0);

    const std::vector<float> cellValues = {0.0f, 1.0f};
    volume::ScalarGrid cellScattered;
    volume::scatter(mesh, grid, &cellValues, true, cellScattered);

    std::size_t occupied = 0;
    std::size_t zeros = 0;
    std::size_t ones = 0;
    for (std::size_t i = 0; i < grid.voxelCount(); ++i) {
        if (!cellScattered.occupancy[i]) continue;
        ++occupied;
        if (cellScattered.value[i] == 0.0f) ++zeros;
        else if (cellScattered.value[i] == 1.0f) ++ones;
        else FAIL("cell placement produced a value between the two cells");
    }
    CHECK(occupied > 0);
    CHECK(occupied < grid.voxelCount()); // the off-diagonal octants stay empty
    CHECK(zeros > 0);
    CHECK(ones > 0);
    // Each cube contributes roughly half of the occupied voxels.
    CHECK(static_cast<double>(zeros) / static_cast<double>(occupied) ==
          doctest::Approx(0.5).epsilon(0.15));

    // Point placement interpolates the same linear field through both cells.
    std::vector<float> pointValues;
    for (const auto& p : points) pointValues.push_back(static_cast<float>(p[0] + p[1] + p[2]));
    volume::ScalarGrid pointScattered;
    volume::scatter(mesh, grid, &pointValues, false, pointScattered);
    const double sx = grid.extent.x / grid.dimX;
    const double sy = grid.extent.y / grid.dimY;
    const double sz = grid.extent.z / grid.dimZ;
    for (int z = 0; z < grid.dimZ; ++z) {
        for (int y = 0; y < grid.dimY; ++y) {
            for (int x = 0; x < grid.dimX; ++x) {
                const std::size_t idx = grid.index(x, y, z);
                if (!pointScattered.occupancy[idx]) continue;
                const double expected =
                    grid.origin.x + (x + 0.5) * sx
                  + grid.origin.y + (y + 0.5) * sy
                  + grid.origin.z + (z + 0.5) * sz;
                CHECK(pointScattered.value[idx] ==
                      doctest::Approx(expected).epsilon(1e-4));
            }
        }
    }
}

TEST_CASE("volume.sheared_hex_leaves_the_corners_of_its_aabb_empty") {
    // Parallelepiped sheared along x by 0.5 * z. The AABB is 1.5 x 1 x 1 but
    // the cell only occupies half of the x/z bounding rectangle.
    const std::vector<Point> points = {
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {1.0, 1.0, 0.0}, {0.0, 1.0, 0.0},
        {0.5, 0.0, 1.0}, {1.5, 0.0, 1.0}, {1.5, 1.0, 1.0}, {0.5, 1.0, 1.0},
    };
    const RenderMesh mesh = makeMesh(points, {{0, 1, 2, 3, 4, 5, 6, 7}}, {12});
    REQUIRE(volume::eligible(mesh));

    volume::DerivedGrid grid;
    REQUIRE(volume::build(mesh, options(16), grid));

    const std::vector<float> cellValues = {1.0f};
    volume::ScalarGrid scattered;
    volume::scatter(mesh, grid, &cellValues, true, scattered);

    std::size_t occupied = 0;
    for (std::size_t i = 0; i < grid.voxelCount(); ++i) {
        if (scattered.occupancy[i]) ++occupied;
    }
    CHECK(occupied > 0);
    // Roughly half of the 1.5 x 1 x 1 AABB is inside the sheared cell.
    const double fraction = static_cast<double>(occupied) /
                            static_cast<double>(grid.voxelCount());
    CHECK(fraction == doctest::Approx(1.0 / 1.5).epsilon(0.15));

    // A point near (0.1, 0.5, 0.9) lies left of the sheared cell (which
    // starts at x = 0.45 there) and must be empty.
    const auto [x, z] = voxelAt(grid, {0.1, 0.5, 0.9});
    int y = grid.dimY / 2;
    CHECK(grid.cellId[grid.index(x, y, z)] == volume::DerivedGrid::kEmpty);

    // A point at (0.75, 0.5, 0.9) is inside.
    const auto [xi, zi] = voxelAt(grid, {0.75, 0.5, 0.9});
    CHECK(grid.cellId[grid.index(xi, y, zi)] == 0u);
}

TEST_CASE("volume.tet_only_mesh_covers_one_sixth_of_the_box") {
    const std::vector<Point> points = {
        {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1},
    };
    const RenderMesh mesh = makeMesh(points, {{0, 1, 2, 3}}, {10});
    REQUIRE(volume::eligible(mesh));

    volume::DerivedGrid grid;
    REQUIRE(volume::build(mesh, options(24), grid));

    std::size_t occupied = 0;
    for (std::uint32_t id : grid.cellId) {
        if (id != volume::DerivedGrid::kEmpty) ++occupied;
    }
    const double voxelVolume =
        (grid.extent.x / grid.dimX) * (grid.extent.y / grid.dimY) * (grid.extent.z / grid.dimZ);
    const double sampledVolume = static_cast<double>(occupied) * voxelVolume;
    CHECK(sampledVolume == doctest::Approx(1.0 / 6.0).epsilon(0.1));

    const std::vector<float> cellValues = {3.0f};
    volume::ScalarGrid scattered;
    volume::scatter(mesh, grid, &cellValues, true, scattered);
    for (std::size_t i = 0; i < grid.voxelCount(); ++i) {
        CHECK(scattered.occupancy[i] == (grid.cellId[i] != volume::DerivedGrid::kEmpty ? 1 : 0));
    }
}

TEST_CASE("volume.resolution_is_capped_without_overflow") {
    const auto points = cubePoints();
    const RenderMesh mesh = makeMesh(points, {{0, 1, 2, 3, 4, 5, 6, 7}}, {12});

    volume::BuildOptions opts;
    opts.maxDim = 300;   // 301^3 ~ 27.3M > the cap
    opts.maxVoxels = 100000;

    volume::DerivedGrid grid;
    REQUIRE(volume::build(mesh, opts, grid));
    const std::size_t total = static_cast<std::size_t>(grid.dimX)
                            * static_cast<std::size_t>(grid.dimY)
                            * static_cast<std::size_t>(grid.dimZ);
    CHECK(total == grid.voxelCount());
    CHECK(total <= opts.maxVoxels);
    CHECK(total > 0);
    for (int a = 0; a < 3; ++a) CHECK(grid.dim(a) >= 2);
}

TEST_CASE("volume.eligibility_rejects_2d_meshes") {
    // A quad in the z = 0 plane: it carries cell connectivity but no volume.
    const std::vector<Point> points = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
    };
    RenderMesh mesh = makeMesh(points, {{0, 1, 2, 3}}, {9});
    CHECK_FALSE(volume::eligible(mesh));

    volume::DerivedGrid grid;
    CHECK_FALSE(volume::build(mesh, options(8), grid));
    CHECK(grid.empty());
}
