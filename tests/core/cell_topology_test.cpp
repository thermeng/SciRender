// cell_topology — canonical cell order and per-cell tetrahedralization.
//
// The renderer relies on two invariants here:
//  * a canonical VTK corner order for tet/hex/wedge/pyramid, so the fixed
//    face templates describe the cell;
//  * a shared-face triangulation that is chosen from global vertex ids, so
//    two incident cells split their common face identically (watertight
//    isosurfaces).

#include "core/cell_topology.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <random>
#include <set>
#include <utility>
#include <vector>

namespace {

using Point = std::array<double, 3>;
using Loop = std::vector<std::size_t>;

std::pair<std::size_t, std::size_t> edgeKey(std::size_t a, std::size_t b) {
    return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
}

std::set<std::pair<std::size_t, std::size_t>>
edgesOfLoops(const std::vector<Loop>& loops) {
    std::set<std::pair<std::size_t, std::size_t>> edges;
    for (const auto& loop : loops) {
        for (std::size_t i = 0; i < loop.size(); ++i) {
            edges.insert(edgeKey(loop[i], loop[(i + 1) % loop.size()]));
        }
    }
    return edges;
}

std::vector<Loop> templateLoops(std::uint8_t type) {
    std::vector<Loop> loops;
    for (const auto& face : cell_topology::faces(type)) {
        Loop loop;
        for (int v : face) loop.push_back(static_cast<std::size_t>(v));
        loops.push_back(std::move(loop));
    }
    return loops;
}

// Every template edge of the canonically-ordered cell must be a real edge of
// the input face topology. This rejects a twisted bottom/top pairing.
void checkCanonicalEdges(std::uint8_t type, const Loop& canonical,
                         const std::vector<Loop>& inputLoops) {
    const auto realEdges = edgesOfLoops(inputLoops);
    const auto& templateFaces = cell_topology::faces(type);
    std::set<std::pair<std::size_t, std::size_t>> seen;
    for (const auto& face : templateFaces) {
        for (std::size_t i = 0; i < face.size(); ++i) {
            const std::size_t a = static_cast<std::size_t>(face[i]);
            const std::size_t b = static_cast<std::size_t>(face[(i + 1) % face.size()]);
            if (!seen.insert(edgeKey(a, b)).second) continue;
            REQUIRE(canonical.size() > std::max(a, b));
            CHECK_MESSAGE(realEdges.count(edgeKey(canonical[a], canonical[b])) == 1,
                          "template edge " << a << "-" << b << " is not a cell edge");
        }
    }
}

std::vector<std::array<std::uint32_t, 4>>
tetsOf(std::uint8_t type, const std::vector<std::uint32_t>& cellVerts) {
    std::vector<std::array<std::uint32_t, 4>> out;
    cell_topology::tets(type, cellVerts, out);
    return out;
}

// Volume of one tetrahedron.
double tetVolume(const Point& a, const Point& b, const Point& c, const Point& d) {
    const Point e1{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const Point e2{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const Point e3{d[0] - a[0], d[1] - a[1], d[2] - a[2]};
    const double cx = e1[1] * e2[2] - e1[2] * e2[1];
    const double cy = e1[2] * e2[0] - e1[0] * e2[2];
    const double cz = e1[0] * e2[1] - e1[1] * e2[0];
    return std::abs(cx * e3[0] + cy * e3[1] + cz * e3[2]) / 6.0;
}

double centroidTetsVolume(std::uint8_t type, const std::vector<Point>& points,
                          const std::vector<std::size_t>& cell) {
    std::vector<std::uint32_t> ids;
    ids.reserve(cell.size());
    for (std::size_t v : cell) ids.push_back(static_cast<std::uint32_t>(v));

    Point centroid{0.0, 0.0, 0.0};
    for (std::size_t v : cell) {
        centroid[0] += points[v][0];
        centroid[1] += points[v][1];
        centroid[2] += points[v][2];
    }
    const double inv = 1.0 / static_cast<double>(cell.size());
    centroid[0] *= inv; centroid[1] *= inv; centroid[2] *= inv;

    double total = 0.0;
    for (const auto& tet : tetsOf(type, ids)) {
        Point p[4];
        for (int k = 0; k < 4; ++k) {
            const std::uint32_t slot = tet[static_cast<std::size_t>(k)];
            p[k] = slot < cell.size() ? points[cell[slot]] : centroid;
        }
        total += tetVolume(p[0], p[1], p[2], p[3]);
    }
    return total;
}

std::vector<Loop> shuffledLoops(std::vector<Loop> loops, std::mt19937& rng) {
    std::shuffle(loops.begin(), loops.end(), rng);
    for (auto& loop : loops) {
        // ANSYS can start a face loop at any node and can traverse it in
        // either direction, but the nodes remain in cyclic order.
        std::rotate(loop.begin(),
                    loop.begin() + static_cast<std::ptrdiff_t>(rng() % loop.size()),
                    loop.end());
        if (rng() & 1u) std::reverse(loop.begin(), loop.end());
    }
    return loops;
}

std::vector<Loop> mapLoops(std::vector<Loop> loops,
                           const std::vector<std::size_t>& cell) {
    for (auto& loop : loops) {
        for (auto& v : loop) v = cell[v];
    }
    return loops;
}

// Unit cube, VTK order: bottom 0-3 at z=0, top 4-7 at z=1.
std::vector<Point> cubePoints() {
    return {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
        {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
    };
}

// Right triangular prism: bottom triangle 0-2 at z=0, top 3-5 at z=1.
std::vector<Point> wedgePoints() {
    return {
        {0, 0, 0}, {1, 0, 0}, {0, 1, 0},
        {0, 0, 1}, {1, 0, 1}, {0, 1, 1},
    };
}

// Square pyramid: base 0-3 at z=0, apex 4.
std::vector<Point> pyramidPoints() {
    return {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
        {0, 0, 1},
    };
}

std::vector<Point> tetPoints() {
    return {
        {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1},
    };
}

} // namespace

TEST_CASE("cell_topology.classify_by_face_shape") {
    CHECK(cell_topology::classifyShape(4, 4, 0) == cell_topology::kTetra);
    CHECK(cell_topology::classifyShape(6, 0, 6) == cell_topology::kHexahedron);
    CHECK(cell_topology::classifyShape(5, 2, 3) == cell_topology::kWedge);
    CHECK(cell_topology::classifyShape(5, 4, 1) == cell_topology::kPyramid);
    CHECK(cell_topology::classifyShape(5, 3, 2) == cell_topology::kPolyhedron);

    CHECK(cell_topology::isVolumeCell(cell_topology::kHexahedron));
    CHECK_FALSE(cell_topology::isVolumeCell(cell_topology::kPolyhedron));
    CHECK(cell_topology::cornerCount(cell_topology::kPyramid) == 5);
    CHECK(cell_topology::faces(cell_topology::kPolyhedron).empty());
}

TEST_CASE("cell_topology.canonicalize_hex_from_shuffled_faces") {
    const auto points = cubePoints();
    std::mt19937 rng(1234);
    const auto loops = templateLoops(cell_topology::kHexahedron);

    for (int trial = 0; trial < 32; ++trial) {
        const auto shuffled = shuffledLoops(loops, rng);
        std::vector<std::size_t> verts = {0, 1, 2, 3, 4, 5, 6, 7};
        std::shuffle(verts.begin(), verts.end(), rng);

        const auto originalVolumeProbe = verts; // keep a copy for the message
        const auto type = cell_topology::canonicalize(verts, shuffled, points);
        REQUIRE_MESSAGE(type == cell_topology::kHexahedron,
                        "trial " << trial << " failed to canonicalize");
        CHECK(std::is_permutation(verts.begin(), verts.end(), originalVolumeProbe.begin()));
        checkCanonicalEdges(type, verts, shuffled);
        CHECK(centroidTetsVolume(type, points, verts) == doctest::Approx(1.0).epsilon(1e-12));
    }
}

TEST_CASE("cell_topology.canonicalize_wedge_pyramid_tet") {
    std::mt19937 rng(99);

    SUBCASE("wedge") {
        const auto points = wedgePoints();
        const auto loops = templateLoops(cell_topology::kWedge);
        for (int trial = 0; trial < 16; ++trial) {
            const auto shuffled = shuffledLoops(loops, rng);
            std::vector<std::size_t> verts = {0, 1, 2, 3, 4, 5};
            std::shuffle(verts.begin(), verts.end(), rng);
            const auto type = cell_topology::canonicalize(verts, shuffled, points);
            REQUIRE(type == cell_topology::kWedge);
            checkCanonicalEdges(type, verts, shuffled);
            CHECK(centroidTetsVolume(type, points, verts) == doctest::Approx(0.5).epsilon(1e-12));
        }
    }

    SUBCASE("pyramid") {
        const auto points = pyramidPoints();
        const auto loops = templateLoops(cell_topology::kPyramid);
        for (int trial = 0; trial < 16; ++trial) {
            const auto shuffled = shuffledLoops(loops, rng);
            std::vector<std::size_t> verts = {0, 1, 2, 3, 4};
            std::shuffle(verts.begin(), verts.end(), rng);
            const auto type = cell_topology::canonicalize(verts, shuffled, points);
            REQUIRE(type == cell_topology::kPyramid);
            checkCanonicalEdges(type, verts, shuffled);
            CHECK(centroidTetsVolume(type, points, verts) == doctest::Approx(1.0 / 3.0).epsilon(1e-12));
        }
    }

    SUBCASE("tetrahedron") {
        const auto points = tetPoints();
        const auto loops = templateLoops(cell_topology::kTetra);
        for (int trial = 0; trial < 16; ++trial) {
            const auto shuffled = shuffledLoops(loops, rng);
            std::vector<std::size_t> verts = {0, 1, 2, 3};
            std::shuffle(verts.begin(), verts.end(), rng);
            const auto type = cell_topology::canonicalize(verts, shuffled, points);
            REQUIRE(type == cell_topology::kTetra);
            checkCanonicalEdges(type, verts, shuffled);
            const auto tets = tetsOf(type, {static_cast<std::uint32_t>(verts[0]),
                                            static_cast<std::uint32_t>(verts[1]),
                                            static_cast<std::uint32_t>(verts[2]),
                                            static_cast<std::uint32_t>(verts[3])});
            REQUIRE(tets.size() == 1);
            CHECK(centroidTetsVolume(type, points, verts) == doctest::Approx(1.0 / 6.0).epsilon(1e-12));
        }
    }
}

TEST_CASE("cell_topology.tet_counts_and_volume") {
    SUBCASE("hex has 12 tets over 6 faces") {
        std::vector<std::uint32_t> cell = {0, 1, 2, 3, 4, 5, 6, 7};
        const auto tets = tetsOf(cell_topology::kHexahedron, cell);
        CHECK(tets.size() == 12);
        for (const auto& tet : tets) CHECK(tet[3] == 8);
        CHECK(centroidTetsVolume(cell_topology::kHexahedron, cubePoints(),
                                 {0, 1, 2, 3, 4, 5, 6, 7}) ==
              doctest::Approx(1.0).epsilon(1e-12));
    }

    SUBCASE("wedge has 8 tets over 5 faces") {
        std::vector<std::uint32_t> cell = {0, 1, 2, 3, 4, 5};
        CHECK(tetsOf(cell_topology::kWedge, cell).size() == 8);
        CHECK(centroidTetsVolume(cell_topology::kWedge, wedgePoints(),
                                 {0, 1, 2, 3, 4, 5}) ==
              doctest::Approx(0.5).epsilon(1e-12));
    }

    SUBCASE("pyramid has 6 tets over 5 faces") {
        std::vector<std::uint32_t> cell = {0, 1, 2, 3, 4};
        CHECK(tetsOf(cell_topology::kPyramid, cell).size() == 6);
        CHECK(centroidTetsVolume(cell_topology::kPyramid, pyramidPoints(),
                                 {0, 1, 2, 3, 4}) ==
              doctest::Approx(1.0 / 3.0).epsilon(1e-12));
    }

    SUBCASE("tetrahedron is a single tet") {
        std::vector<std::uint32_t> cell = {0, 1, 2, 3};
        const auto tets = tetsOf(cell_topology::kTetra, cell);
        REQUIRE(tets.size() == 1);
        for (auto slot : tets[0]) CHECK(slot < 4);
        CHECK(centroidTetsVolume(cell_topology::kTetra, tetPoints(),
                                 {0, 1, 2, 3}) ==
              doctest::Approx(1.0 / 6.0).epsilon(1e-12));
    }
}

TEST_CASE("cell_topology.shared_face_triangulation_matches") {
    // Two stacked unit hexes. The plane z = 1 is shared. Canonicalize both
    // from independently shuffled face loops, then compare the triangles each
    // cell contributes on the shared face.
    std::vector<Point> points = cubePoints();
    points.push_back({0, 0, 2});
    points.push_back({1, 0, 2});
    points.push_back({1, 1, 2});
    points.push_back({0, 1, 2});

    std::mt19937 rng(7);
    const auto loops = templateLoops(cell_topology::kHexahedron);
    const std::vector<std::size_t> upperCanonical = {4, 5, 6, 7, 8, 9, 10, 11};

    std::vector<std::size_t> lower = {0, 1, 2, 3, 4, 5, 6, 7};
    std::vector<std::size_t> upper = upperCanonical;
    const auto lowerType =
        cell_topology::canonicalize(lower, shuffledLoops(loops, rng), points);
    const auto upperType =
        cell_topology::canonicalize(upper, shuffledLoops(mapLoops(loops, upperCanonical), rng), points);
    REQUIRE(lowerType == cell_topology::kHexahedron);
    REQUIRE(upperType == cell_topology::kHexahedron);

    auto faceTriangles = [&](const std::vector<std::size_t>& cell) {
        std::vector<std::uint32_t> ids;
        for (std::size_t v : cell) ids.push_back(static_cast<std::uint32_t>(v));
        std::set<std::array<std::uint32_t, 3>> out;
        for (const auto& tet : tetsOf(cell_topology::kHexahedron, ids)) {
            for (int omit = 0; omit < 4; ++omit) {
                std::array<std::uint32_t, 3> tri;
                int k = 0;
                for (int j = 0; j < 4; ++j) {
                    if (j == omit) continue;
                    const std::uint32_t slot = tet[static_cast<std::size_t>(j)];
                    if (slot >= cell.size()) { k = -1; break; }
                    tri[static_cast<std::size_t>(k++)] = static_cast<std::uint32_t>(cell[slot]);
                }
                if (k < 0) continue;
                // Keep only triangles lying entirely on the shared plane.
                bool onShared = true;
                for (auto v : tri) {
                    if (v != 4 && v != 5 && v != 6 && v != 7) onShared = false;
                }
                if (!onShared) continue;
                std::sort(tri.begin(), tri.end());
                out.insert(tri);
            }
        }
        return out;
    };

    const auto lowerTris = faceTriangles(lower);
    const auto upperTris = faceTriangles(upper);
    // Two triangles per quad, and they must be the same pair from both sides.
    CHECK(lowerTris.size() == 2);
    CHECK(lowerTris == upperTris);
}
