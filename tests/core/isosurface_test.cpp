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
