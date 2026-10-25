// Standalone regression check for the table-free marching-cubes extractor.
// Compiles the REAL CoreLib sources (no Qt/GL).
#include "core/mesh_loader.h"
#include "core/isosurface.h"
#include "doctest/doctest.h"
#include "support/test_support.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

// Build a 2x2x2 unit cube grid with node values 0,0.5,0.5,1,0.5,0.5,1,1
// (IJK order). Node 0 at the origin is the lone low corner for iso<=0.75.
RenderMesh makeBoxGrid() {
    RenderMesh m;
    m.gridDimX = 2;
    m.gridDimY = 2;
    m.gridDimZ = 2;
    m.datasetType = "IMAGEDATA";
    m.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1};
    m.scalars = {0.0f, 0.5f, 0.5f, 1.0f, 0.5f, 0.5f, 1.0f, 1.0f};
    m.scalarName = "scalars";
    mesh_utils::computeBounds(m);
    return m;
}

// Vertex-position equality with tolerance (order is fan-dependent).
bool hasPoint(const RenderMesh& r, float x, float y, float z, float eps = 1e-4f) {
    for (size_t i = 0; i < r.vertices.size() / 3; ++i) {
        float dx = r.vertices[i * 3] - x, dy = r.vertices[i * 3 + 1] - y,
              dz = r.vertices[i * 3 + 2] - z;
        if (dx * dx + dy * dy + dz * dz < eps * eps)
            return true;
    }
    return false;
}

// Edge-manifold closure check over the FINAL (post normal-split) mesh.
struct Closure {
    int openEdges;
    int nonManifoldEdges;
    int degenerateFaces;
    bool watertight;
};

Closure closureOf(const RenderMesh& r) {
    Closure c{0, 0, 0, false};
    if (r.indices.size() % 3 != 0 || r.vertices.empty())
        return c;
    const size_t nv = r.vertices.size() / 3;
    std::map<std::pair<uint32_t, uint32_t>, int> edgeCount;
    for (size_t t = 0; t + 2 < r.indices.size(); t += 3) {
        uint32_t a = r.indices[t], b = r.indices[t + 1], cc = r.indices[t + 2];
        if (a == b || b == cc || a == cc) {
            c.degenerateFaces++;
            continue;
        }
        if (a >= nv || b >= nv || cc >= nv) {
            c.nonManifoldEdges++;
            continue;
        }
        // count each undirected edge once
        auto bump = [&](uint32_t u, uint32_t v) {
            if (u > v)
                std::swap(u, v);
            edgeCount[{u, v}]++;
        };
        bump(a, b);
        bump(b, cc);
        bump(cc, a);
    }
    for (auto& entry : edgeCount) {
        const int cnt = entry.second;
        if (cnt == 1)
            c.openEdges++;
        else if (cnt > 2)
            c.nonManifoldEdges++;
    }
    c.watertight = (c.openEdges == 0 && c.nonManifoldEdges == 0 && c.degenerateFaces == 0);
    return c;
}

// Euler characteristic over the triangulated result.
int eulerChi(const RenderMesh& r) {
    if (r.indices.size() % 3 != 0)
        return 0;
    const size_t nv = r.vertices.size() / 3;
    const size_t nt = r.indices.size() / 3;
    std::map<std::pair<uint32_t, uint32_t>, int> edgeCount;
    for (size_t t = 0; t + 2 < r.indices.size(); t += 3) {
        uint32_t a = r.indices[t], b = r.indices[t + 1], c = r.indices[t + 2];
        auto bump = [&](uint32_t u, uint32_t v) {
            if (u > v)
                std::swap(u, v);
            edgeCount[{u, v}]++;
        };
        bump(a, b);
        bump(b, c);
        bump(c, a);
    }
    size_t ne = edgeCount.size();
    // V - E + F (using unique vertices actually referenced)
    std::vector<bool> used(nv, false);
    for (uint32_t i : r.indices)
        if (i < nv)
            used[i] = true;
    size_t vu = static_cast<size_t>(std::count(used.begin(), used.end(), true));
    return static_cast<int>(vu) - static_cast<int>(ne) + static_cast<int>(nt);
}

// Radial scalar field on an N^3 grid; low region is a closed interior blob.
RenderMesh makeSphereGrid(int n, float cx, float cy, float cz) {
    RenderMesh m;
    m.gridDimX = n;
    m.gridDimY = n;
    m.gridDimZ = n;
    m.datasetType = "IMAGEDATA";
    const int total = n * n * n;
    m.vertices.resize(static_cast<size_t>(total) * 3);
    m.scalars.resize(total);
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                size_t i = static_cast<size_t>(x + y * n + z * n * n);
                float px = static_cast<float>(x), py = static_cast<float>(y),
                      pz = static_cast<float>(z);
                m.vertices[i * 3 + 0] = px;
                m.vertices[i * 3 + 1] = py;
                m.vertices[i * 3 + 2] = pz;
                float dx = px - cx, dy = py - cy, dz = pz - cz;
                m.scalars[i] = dx * dx + dy * dy + dz * dz; // dist^2
            }
    m.scalarName = "d2";
    mesh_utils::computeBounds(m);
    return m;
}

} // namespace

TEST_SUITE("core") {

TEST_CASE("core.isosurface.single_cell_exact") {
    RenderMesh box = makeBoxGrid();
    CHECK_MESSAGE(isosurface::canExtract(box), "canExtract(box)");

    // 1 low corner (node 0) at iso=0.25 -> exactly one triangle whose vertices
    // are the midpoints of the three edges meeting at the origin.
    auto r1 = isosurface::extractIsosurface(box, {0.25f});
    CHECK_MESSAGE(r1.indices.size() == 3, "iso=0.25 -> 3 indices (1 triangle)");
    CHECK_MESSAGE(r1.vertices.size() / 3 == 3, "iso=0.25 -> 3 vertices");
    CHECK_MESSAGE(r1.normals.size() / 3 == r1.vertices.size() / 3, "iso=0.25 normals match verts");
    CHECK_MESSAGE(r1.scalars.size() == r1.vertices.size() / 3, "iso=0.25 scalars match verts");
    const bool scalarIsLevel = r1.scalars.empty() ? true : (r1.scalars[0] == 0.25f);
    CHECK_MESSAGE(scalarIsLevel, "iso=0.25 scalar value == level");
    CHECK_MESSAGE(hasPoint(r1, 0.5f, 0.0f, 0.0f), "iso=0.25 vertex (0.5,0,0)");
    CHECK_MESSAGE(hasPoint(r1, 0.0f, 0.5f, 0.0f), "iso=0.25 vertex (0,0.5,0)");
    CHECK_MESSAGE(hasPoint(r1, 0.0f, 0.0f, 0.5f), "iso=0.25 vertex (0,0,0.5)");
    for (uint32_t i : r1.indices)
        CHECK_MESSAGE(i < r1.vertices.size() / 3, "iso=0.25 index in range");

    // iso exactly on a node value -> still a single valid triangle (cap).
    auto r2 = isosurface::extractIsosurface(box, {0.5f});
    CHECK_MESSAGE(r2.indices.size() == 3, "iso=0.5 -> 3 indices (1 triangle)");
    CHECK_MESSAGE(r2.vertices.size() / 3 == 3, "iso=0.5 -> 3 vertices");

    // 3 high corners (the {3,6,7} config) -> a 5-gon fan = 3 triangles.
    auto r3 = isosurface::extractIsosurface(box, {0.75f});
    CHECK_MESSAGE(r3.indices.size() == 9, "iso=0.75 -> 9 indices (3 triangles)");
    CHECK_MESSAGE(r3.indices.size() % 3 == 0, "iso=0.75 indices are triangles");
    CHECK_MESSAGE(r3.normals.size() / 3 == r3.vertices.size() / 3, "iso=0.75 normals match verts");
    CHECK_MESSAGE(r3.scalars.size() == r3.vertices.size() / 3, "iso=0.75 scalars match verts");
    for (uint32_t i : r3.indices)
        CHECK_MESSAGE(i < r3.vertices.size() / 3, "iso=0.75 index in range");

    // Out of data range -> no crossing -> empty.
    auto r4 = isosurface::extractIsosurface(box, {1.5f});
    CHECK_MESSAGE(r4.vertices.empty(), "iso=1.5 -> empty (above max)");
    auto r5 = isosurface::extractIsosurface(box, {-0.5f});
    CHECK_MESSAGE(r5.vertices.empty(), "iso=-0.5 -> empty (below min)");

    // Empty isovalue list -> empty mesh.
    CHECK_MESSAGE(isosurface::extractIsosurface(box, {}).vertices.empty(), "empty isovalues -> empty");

    // Multiple contours: one contributes, one is empty -> single contour result.
    auto r6 = isosurface::extractIsosurface(box, {0.25f, 1.5f});
    CHECK_MESSAGE(r6.indices.size() == 3, "multi-iso {0.25,1.5} -> 1 triangle (only 0.25 crosses)");

    // canExtract gate rejects non-grid / degenerate inputs.
    RenderMesh pt = box;
    pt.gridDimZ = 1;
    CHECK_MESSAGE(!isosurface::canExtract(pt), "canExtract rejects 2D grid");
    RenderMesh noscalar = box;
    noscalar.scalars.clear();
    CHECK_MESSAGE(!isosurface::canExtract(noscalar), "canExtract rejects no-scalar");
    CHECK_MESSAGE(!isosurface::canExtract(RenderMesh{}), "canExtract rejects empty mesh");
}

TEST_CASE("core.isosurface.closed_surface") {
    // 5x5x5 grid, scalar = dist^2 from center (2,2,2). iso chosen strictly
    // between node levels so no node lands exactly on the level -> a smooth,
    // closed, manifold sphere with no ambiguous 4-4 cells. (iso=3.0 lands
    // exactly on corner nodes (dist^2=3) and triggers ambiguous faces;
    // iso=2.5 is safely between levels)
    RenderMesh g = makeSphereGrid(5, 2.0f, 2.0f, 2.0f);
    CHECK_MESSAGE(isosurface::canExtract(g), "canExtract(sphere grid)");
    auto r = isosurface::extractIsosurface(g, {2.5f});
    CHECK_MESSAGE(!r.vertices.empty(), "sphere iso=2.5 -> non-empty");
    CHECK_MESSAGE(r.vertices.size() / 3 == r.scalars.size(), "sphere scalars match verts");
    CHECK_MESSAGE(r.vertices.size() / 3 == r.normals.size() / 3, "sphere normals match verts");
    for (uint32_t i : r.indices)
        CHECK_MESSAGE(i < r.vertices.size() / 3, "sphere index in range");
    // Every vertex must sit strictly inside the volume bounds.
    const double bmin[3] = {g.bounds.minX, g.bounds.minY, g.bounds.minZ};
    const double bmax[3] = {g.bounds.maxX, g.bounds.maxY, g.bounds.maxZ};
    bool inBounds = true;
    for (size_t i = 0; i < r.vertices.size() / 3; ++i)
        for (int c = 0; c < 3; ++c)
            if (r.vertices[i * 3 + c] < bmin[c] - 1e-4 || r.vertices[i * 3 + c] > bmax[c] + 1e-4)
                inBounds = false;
    CHECK_MESSAGE(inBounds, "sphere vertices within volume bounds");

    Closure cl = closureOf(r);
    MESSAGE("sphere: verts=", r.vertices.size() / 3, " idx=", r.indices.size(),
            " openEdges=", cl.openEdges, " nonManifold=", cl.nonManifoldEdges, " degen=",
            cl.degenerateFaces, " euler=", eulerChi(r));
    CHECK_MESSAGE(cl.degenerateFaces == 0, "sphere no degenerate triangles");
    CHECK_MESSAGE(cl.nonManifoldEdges == 0, "sphere no non-manifold edges");
    CHECK_MESSAGE(cl.watertight, "sphere isosurface is watertight (closed manifold)");
    CHECK_MESSAGE(eulerChi(r) == 2, "sphere euler characteristic == 2 (genus 0)");

    // Lowering the level grows the sheet; raising above the max collapses it.
    RenderMesh empty = isosurface::extractIsosurface(g, {1e9f});
    CHECK_MESSAGE(empty.vertices.empty(), "sphere iso above max -> empty");
}

TEST_CASE("core.isosurface.watertight_sweep") {
    // Off-center spheres break symmetry so the sweep hits ambiguous saddles
    // and two-loop cells. All levels keep the surface strictly interior, so
    // every result must be a closed manifold (0 open edges). Catches the old
    // fan-all-crossings bridging bug + node-on-level duplicate vertices.
    struct Case {
        int n;
        float cx, cy, cz;
        float iso;
    };
    const Case cases[] = {
        {9, 4.3f, 3.7f, 4.1f, 1.00f},
        {9, 4.3f, 3.7f, 4.1f, 2.37f},
        {9, 4.3f, 3.7f, 4.1f, 3.74f},
        {9, 4.3f, 3.7f, 4.1f, 5.11f},
        {9, 4.3f, 3.7f, 4.1f, 6.48f},
        {9, 4.3f, 3.7f, 4.1f, 7.85f},
        {9, 4.3f, 3.7f, 4.1f, 9.22f},
        {9, 4.3f, 3.7f, 4.1f, 10.59f},
        {9, 4.3f, 3.7f, 4.1f, 11.96f},
        {9, 4.3f, 3.7f, 4.1f, 13.33f},
        {11, 5.5f, 5.5f, 5.5f, 1.00f},
        {11, 5.5f, 5.5f, 5.5f, 2.37f},
        {11, 5.5f, 5.5f, 5.5f, 3.74f},
        {11, 5.5f, 5.5f, 5.5f, 5.11f},
        // Node-on-level degeneracy: node (5,5,7) has dist^2 == 10.59 exactly.
        {9, 4.3f, 3.7f, 4.1f, 10.59f},
    };
    for (const auto& c : cases) {
        CAPTURE(c.n);
        CAPTURE(c.iso);
        RenderMesh g = makeSphereGrid(c.n, c.cx, c.cy, c.cz);
        auto r = isosurface::extractIsosurface(g, {c.iso});
        REQUIRE_MESSAGE(!r.vertices.empty(), "sweep result must be non-empty");
        Closure cl = closureOf(r);
        CHECK_MESSAGE(cl.watertight, "sweep result must be watertight (open=" << cl.openEdges
                                                              << ", nonManifold=" << cl.nonManifoldEdges
                                                              << ", degenerate=" << cl.degenerateFaces << ")");
    }
}

// ---------------------------------------------------------------------------
// Unstructured meshes (CFD path): marching tetrahedra over volume cells.
// ---------------------------------------------------------------------------

namespace {

// n^3 node grid carved into (n-1)^3 canonical VTK hexes, point scalar = squared
// distance from (cx,cy,cz). Mirrors the topology the CFD adapter produces.
RenderMesh makeUnstructuredHexGrid(int n, double cx, double cy, double cz) {
    RenderMesh m;
    m.datasetType = "CFD MESH";
    m.sourcePointCount = n * n * n;
    m.vertices.resize(static_cast<size_t>(n * n * n) * 3);
    m.scalars.resize(static_cast<size_t>(n * n * n));
    auto node = [n](int x, int y, int z) {
        return static_cast<uint32_t>(x + y * n + z * n * n);
    };
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const uint32_t i = node(x, y, z);
                m.vertices[i * 3 + 0] = static_cast<float>(x);
                m.vertices[i * 3 + 1] = static_cast<float>(y);
                m.vertices[i * 3 + 2] = static_cast<float>(z);
                const double dx = x - cx, dy = y - cy, dz = z - cz;
                m.scalars[i] = static_cast<float>(dx * dx + dy * dy + dz * dz);
            }
    for (int z = 0; z + 1 < n; ++z)
        for (int y = 0; y + 1 < n; ++y)
            for (int x = 0; x + 1 < n; ++x) {
                const uint32_t c[8] = {
                    node(x, y, z),         node(x + 1, y, z),
                    node(x + 1, y + 1, z), node(x, y + 1, z),
                    node(x, y, z + 1),     node(x + 1, y, z + 1),
                    node(x + 1, y + 1, z + 1), node(x, y + 1, z + 1),
                };
                for (uint32_t v : c) m.cellConnectivity.push_back(v);
                m.cellOffsets.push_back(static_cast<uint32_t>(m.cellConnectivity.size()));
                m.cellTypes.push_back(12);
                ++m.unstructuredVolumeCellCount;
            }
    m.scalarName = "d2";
    m.availableScalarNames = {"d2"};
    mesh_utils::computeBounds(m);
    return m;
}

} // namespace

TEST_CASE("core.isosurface.unstructured_single_hex_plane") {
    // One hex, scalar = z, iso inside the bottom half. The extracted sheet is a
    // quad at z = 0.25 lying strictly below the cell centroid.
    RenderMesh m = makeUnstructuredHexGrid(2, 0.0, 0.0, 0.0);
    REQUIRE(m.unstructuredVolumeCellCount == 1);
    // Overwrite with the z field (the helper stores dist^2 from the origin).
    m.scalars = {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f};

    CHECK(isosurface::canExtract(m));
    auto r = isosurface::extractIsosurface(m, {0.25f});
    REQUIRE(!r.vertices.empty());
    CHECK(r.indices.size() % 3 == 0);
    CHECK(r.vertices.size() / 3 >= 4); // tet decomposition may split the quad
    // The cell is split into face pyramids, so the crossing polygon is
    // tessellated more finely, but it must still be the planar square at
    // z = 0.25 with unit area.
    double area = 0.0;
    for (size_t t = 0; t + 2 < r.indices.size(); t += 3) {
        const float* a = &r.vertices[r.indices[t] * 3];
        const float* b = &r.vertices[r.indices[t + 1] * 3];
        const float* c = &r.vertices[r.indices[t + 2] * 3];
        CHECK(a[2] == doctest::Approx(0.25f));
        CHECK(b[2] == doctest::Approx(0.25f));
        CHECK(c[2] == doctest::Approx(0.25f));
        const float ux = b[0] - a[0], uy = b[1] - a[1];
        const float vx = c[0] - a[0], vy = c[1] - a[1];
        area += 0.5 * std::abs(ux * vy - uy * vx);
    }
    CHECK(area == doctest::Approx(1.0).epsilon(1e-4));
    for (size_t i = 0; i < r.vertices.size() / 3; ++i) {
        CHECK(r.vertices[i * 3 + 0] >= -1e-4f);
        CHECK(r.vertices[i * 3 + 0] <= 1.0f + 1e-4f);
        CHECK(r.vertices[i * 3 + 1] >= -1e-4f);
        CHECK(r.vertices[i * 3 + 1] <= 1.0f + 1e-4f);
    }
    for (float v : r.scalars) CHECK(v == doctest::Approx(0.25f));
    CHECK(r.scalarName == "d2");

    // A level the whole cell is above produces nothing.
    CHECK(isosurface::extractIsosurface(m, {1.5f}).vertices.empty());
    CHECK(isosurface::extractIsosurface(m, {-0.5f}).vertices.empty());
}

TEST_CASE("core.isosurface.unstructured_hex_stack_is_watertight") {
    // A closed level set inside a 4^3-hex block. Extracted per cell with
    // marching tets, so the surface must still be closed: shared faces are
    // triangulated from the smallest global id and crossing vertices are keyed
    // by the global edge, which makes adjacent cells agree exactly.
    RenderMesh m = makeUnstructuredHexGrid(5, 2.0, 2.0, 2.0);
    REQUIRE(m.unstructuredVolumeCellCount == 64);
    CHECK(isosurface::canExtract(m));

    auto r = isosurface::extractIsosurface(m, {1.5f});
    REQUIRE(!r.vertices.empty());
    CHECK(r.indices.size() % 3 == 0);
    CHECK(r.normals.size() / 3 == r.vertices.size() / 3);
    CHECK(r.scalars.size() == r.vertices.size() / 3);
    for (uint32_t i : r.indices) REQUIRE(i < r.vertices.size() / 3);

    const Closure cl = closureOf(r);
    MESSAGE("unstructured sphere: verts=", r.vertices.size() / 3,
            " idx=", r.indices.size(), " open=", cl.openEdges,
            " nonManifold=", cl.nonManifoldEdges, " degen=", cl.degenerateFaces,
            " euler=", eulerChi(r));
    CHECK(cl.degenerateFaces == 0);
    CHECK(cl.openEdges == 0);
    CHECK(cl.nonManifoldEdges == 0);
    CHECK(eulerChi(r) == 2);

    // Every vertex lies inside the block.
    for (size_t i = 0; i < r.vertices.size() / 3; ++i)
        for (int a = 0; a < 3; ++a) {
            CHECK(r.vertices[i * 3 + a] >= -1e-4f);
            CHECK(r.vertices[i * 3 + a] <= 4.0f + 1e-4f);
        }
}

TEST_CASE("core.isosurface.unstructured_cell_centered_field") {
    // Per-cell values must reach the contour. FieldStore hands back the
    // point-sized extrapolated copy for CellCenter, so the extractor reads the
    // exact cell array and averages it to the nodes itself.
    RenderMesh m = makeUnstructuredHexGrid(3, 1.0, 1.0, 1.0);
    REQUIRE(m.unstructuredVolumeCellCount == 8);
    auto& attrs = mesh_utils::ensureAttributes(m);
    std::vector<float> cellValues(8);
    for (int i = 0; i < 8; ++i) cellValues[static_cast<size_t>(i)] = static_cast<float>(i);
    attrs.cellScalars["cellId"] = cellValues;
    attrs.cellScalarRanges["cellId"] = {0.0f, 7.0f};

    CHECK(isosurface::canExtract(m, "cellId", 1));
    auto r = isosurface::extractIsosurface(m, {3.5f}, "cellId", 1);
    CHECK(!r.vertices.empty());
    CHECK(r.indices.size() % 3 == 0);
    for (uint32_t i : r.indices) CHECK(i < r.vertices.size() / 3);
    CHECK(r.scalarName == "cellId");

    // A constant cell field has no crossing.
    std::fill(cellValues.begin(), cellValues.end(), 2.0f);
    attrs.cellScalars["cellId"] = cellValues;
    CHECK(isosurface::extractIsosurface(m, {2.5f}, "cellId", 1).vertices.empty());
}

TEST_CASE("core.isosurface.unstructured_single_tet") {
    RenderMesh m;
    m.datasetType = "CFD MESH";
    m.sourcePointCount = 4;
    m.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    m.scalars = {0.0f, 0.0f, 0.0f, 1.0f};
    m.scalarName = "s";
    m.cellConnectivity = {0, 1, 2, 3};
    m.cellOffsets = {4};
    m.cellTypes = {10};
    m.unstructuredVolumeCellCount = 1;
    mesh_utils::computeBounds(m);

    CHECK(isosurface::canExtract(m));
    auto r = isosurface::extractIsosurface(m, {0.5f});
    REQUIRE(r.indices.size() == 3); // one triangle
    CHECK(r.vertices.size() / 3 == 3);
    // Vertex 3 is the only high corner, so the sheet cuts the three edges
    // meeting at it.
    CHECK(hasPoint(r, 0.0f, 0.0f, 0.5f));
    CHECK(hasPoint(r, 0.5f, 0.0f, 0.5f));
    CHECK(hasPoint(r, 0.0f, 0.5f, 0.5f));

    CHECK(isosurface::extractIsosurface(m, {2.0f}).vertices.empty());
}

TEST_CASE("core.isosurface.unstructured_rejects_2d") {
    // A quad in z = 0 is not a volume mesh, even though it carries cell
    // connectivity.
    RenderMesh m;
    m.datasetType = "CFD MESH";
    m.sourcePointCount = 4;
    m.vertices = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    m.scalars = {0.0f, 1.0f, 2.0f, 3.0f};
    m.scalarName = "s";
    m.cellConnectivity = {0, 1, 2, 3};
    m.cellOffsets = {4};
    m.cellTypes = {9};
    m.unstructuredVolumeCellCount = 0;
    mesh_utils::computeBounds(m);

    CHECK_FALSE(isosurface::canExtract(m));
    CHECK(isosurface::extractIsosurface(m, {1.5f}).vertices.empty());
}

TEST_CASE("core.isosurface.parsed_sample") {
    test_support::requireSample("IMAGE_DATA_xml_ascii.vti");
    const std::string path = test_support::samplePath("IMAGE_DATA_xml_ascii.vti").string();
    RenderMesh m = loadMeshFile(path);
    const bool dimsOk = (m.gridDimX == 2) && (m.gridDimY == 2) && (m.gridDimZ == 2);
    CHECK_MESSAGE(dimsOk, "sample gridDim 2x2x2");
    CHECK_MESSAGE(isosurface::canExtract(m), "sample canExtract");

    // Node values are 0,0.5,0.5,1,0.5,0.5,1,1 -> iso=0.25 -> 1 triangle at the
    // three mid-edges of corner 0 (node 0 is the lone low corner).
    auto r = isosurface::extractIsosurface(m, {0.25f});
    CHECK_MESSAGE(!r.vertices.empty(), "sample iso=0.25 -> non-empty");
    CHECK_MESSAGE(r.indices.size() == 3, "sample iso=0.25 -> 1 triangle");
    CHECK_MESSAGE(r.vertices.size() / 3 == 3, "sample iso=0.25 -> 3 vertices");
    CHECK_MESSAGE(hasPoint(r, 0.5f, 0.0f, 0.0f), "sample iso=0.25 vertex (0.5,0,0)");
    CHECK_MESSAGE(hasPoint(r, 0.0f, 0.5f, 0.0f), "sample iso=0.25 vertex (0,0.5,0)");
    CHECK_MESSAGE(hasPoint(r, 0.0f, 0.0f, 0.5f), "sample iso=0.25 vertex (0,0,0.5)");
    CHECK_MESSAGE(r.scalars.size() == r.vertices.size() / 3, "sample iso=0.25 scalars match verts");
    CHECK_MESSAGE(r.normals.size() / 3 == r.vertices.size() / 3, "sample iso=0.25 normals match verts");

    // Above max (1.0) -> nothing.
    CHECK_MESSAGE(isosurface::extractIsosurface(m, {1.5f}).vertices.empty(), "sample iso=1.5 -> empty");
}

} // TEST_SUITE("core")
