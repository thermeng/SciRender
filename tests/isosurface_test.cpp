// tests/isosurface_test.cpp
// Standalone regression check for the table-free marching-cubes extractor.
// Compiles the REAL CoreLib sources (no Qt/GL). Run via tests/run_tests.{bat,sh}
#include "core/mesh_loader.h"
#include "core/isosurface.h"
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg) do { ++checks; if(!(cond)){ \
    printf("  [FAIL] %s\n", msg); ++failures; } } while(0)

// Build a 2x2x2 unit cube grid with node values 0,0.5,0.5,1,0.5,0.5,1,1
// (IJK order). Node 0 at the origin is the lone low corner for iso<=0.75.
static RenderMesh makeBoxGrid() {
    RenderMesh m;
    m.gridDimX = 2; m.gridDimY = 2; m.gridDimZ = 2;
    m.datasetType = "IMAGEDATA";
    m.vertices = {
        0,0,0,  1,0,0,  0,1,0,  1,1,0,  0,0,1,  1,0,1,  0,1,1,  1,1,1
    };
    m.scalars = {0.0f, 0.5f, 0.5f, 1.0f, 0.5f, 0.5f, 1.0f, 1.0f};
    m.scalarName = "scalars";
    mesh_utils::computeBounds(m);
    return m;
}

// Vertex-position equality with tolerance (order is fan-dependent).
static bool hasPoint(const RenderMesh& r, float x, float y, float z, float eps=1e-4f) {
    for (size_t i = 0; i < r.vertices.size()/3; ++i) {
        float dx = r.vertices[i*3]-x, dy = r.vertices[i*3+1]-y, dz = r.vertices[i*3+2]-z;
        if (dx*dx+dy*dy+dz*dz < eps*eps) return true;
    }
    return false;
}

// Edge-manifold closure check over the FINAL (post normal-split) mesh.
struct Closure { int openEdges, nonManifoldEdges, degenerateFaces; bool watertight; };
static Closure closureOf(const RenderMesh& r) {
    Closure c{0,0,0,false};
    if (r.indices.size() % 3 != 0 || r.vertices.empty()) return c;
    const size_t nv = r.vertices.size()/3;
    std::map<std::pair<uint32_t,uint32_t>, int> edgeCount;
    for (size_t t = 0; t + 2 < r.indices.size(); t += 3) {
        uint32_t a = r.indices[t], b = r.indices[t+1], cc = r.indices[t+2];
        if (a==b || b==cc || a==cc) { c.degenerateFaces++; continue; }
        if (a>=nv || b>=nv || cc>=nv) { c.nonManifoldEdges++; continue; }
        // count each undirected edge once
        auto bump = [&](uint32_t u, uint32_t v){
            if (u>v) std::swap(u,v);
            edgeCount[{u,v}]++;
        };
        bump(a,b); bump(b,cc); bump(cc,a);
    }
    for (auto& [_, cnt] : edgeCount) {
        if (cnt == 1) c.openEdges++;
        else if (cnt > 2) c.nonManifoldEdges++;
    }
    c.watertight = (c.openEdges == 0 && c.nonManifoldEdges == 0 && c.degenerateFaces == 0);
    return c;
}

// Euler characteristic over the triangulated result.
static int eulerChi(const RenderMesh& r) {
    if (r.indices.size() % 3 != 0) return 0;
    const size_t nv = r.vertices.size()/3;
    const size_t nt = r.indices.size()/3;
    std::map<std::pair<uint32_t,uint32_t>, int> edgeCount;
    for (size_t t = 0; t + 2 < r.indices.size(); t += 3) {
        uint32_t a = r.indices[t], b = r.indices[t+1], c = r.indices[t+2];
        auto bump = [&](uint32_t u, uint32_t v){ if(u>v)std::swap(u,v); edgeCount[{u,v}]++; };
        bump(a,b); bump(b,c); bump(c,a);
    }
    size_t ne = edgeCount.size();
    // V - E + F (using unique vertices actually referenced)
    std::vector<bool> used(nv, false);
    for (uint32_t i : r.indices) if (i<nv) used[i]=true;
    size_t vu = std::count(used.begin(), used.end(), true);
    return (int)vu - (int)ne + (int)nt;
}

static bool fileExists(const std::string& p){ std::ifstream f(p); return f.good(); }

static void testSingleCellExact() {
    printf("[TEST] single-cell exact (synthetic box)\n");
    RenderMesh box = makeBoxGrid();
    CHECK(isosurface::canExtract(box), "canExtract(box)");

    // 1 low corner (node 0) at iso=0.25 -> exactly one triangle whose vertices
    // are the midpoints of the three edges meeting at the origin.
    auto r1 = isosurface::extractIsosurface(box, {0.25f});
    CHECK(r1.indices.size() == 3, "iso=0.25 -> 3 indices (1 triangle)");
    CHECK(r1.vertices.size()/3 == 3, "iso=0.25 -> 3 vertices");
    CHECK(r1.normals.size()/3 == r1.vertices.size()/3, "iso=0.25 normals match verts");
    CHECK(r1.scalars.size() == r1.vertices.size()/3, "iso=0.25 scalars match verts");
    CHECK(r1.scalars.size() == 0 || r1.scalars[0] == 0.25f, "iso=0.25 scalar value == level");
    CHECK(hasPoint(r1, 0.5f, 0.0f, 0.0f), "iso=0.25 vertex (0.5,0,0)");
    CHECK(hasPoint(r1, 0.0f, 0.5f, 0.0f), "iso=0.25 vertex (0,0.5,0)");
    CHECK(hasPoint(r1, 0.0f, 0.0f, 0.5f), "iso=0.25 vertex (0,0,0.5)");
    for (uint32_t i : r1.indices) CHECK(i < r1.vertices.size()/3, "iso=0.25 index in range");

    // iso exactly on a node value -> still a single valid triangle (cap).
    auto r2 = isosurface::extractIsosurface(box, {0.5f});
    CHECK(r2.indices.size() == 3, "iso=0.5 -> 3 indices (1 triangle)");
    CHECK(r2.vertices.size()/3 == 3, "iso=0.5 -> 3 vertices");

    // 3 high corners (the {3,6,7} config) -> a 5-gon fan = 3 triangles.
    auto r3 = isosurface::extractIsosurface(box, {0.75f});
    CHECK(r3.indices.size() == 9, "iso=0.75 -> 9 indices (3 triangles)");
    CHECK(r3.indices.size() % 3 == 0, "iso=0.75 indices are triangles");
    CHECK(r3.normals.size()/3 == r3.vertices.size()/3, "iso=0.75 normals match verts");
    CHECK(r3.scalars.size() == r3.vertices.size()/3, "iso=0.75 scalars match verts");
    for (uint32_t i : r3.indices) CHECK(i < r3.vertices.size()/3, "iso=0.75 index in range");

    // Out of data range -> no crossing -> empty.
    auto r4 = isosurface::extractIsosurface(box, {1.5f});
    CHECK(r4.vertices.empty(), "iso=1.5 -> empty (above max)");
    auto r5 = isosurface::extractIsosurface(box, {-0.5f});
    CHECK(r5.vertices.empty(), "iso=-0.5 -> empty (below min)");

    // Empty isovalue list -> empty mesh.
    CHECK(isosurface::extractIsosurface(box, {}).vertices.empty(), "empty isovalues -> empty");

    // Multiple contours: one contributes, one is empty -> single contour result.
    auto r6 = isosurface::extractIsosurface(box, {0.25f, 1.5f});
    CHECK(r6.indices.size() == 3, "multi-iso {0.25,1.5} -> 1 triangle (only 0.25 crosses)");

    // canExtract gate rejects non-grid / degenerate inputs.
    RenderMesh pt = box; pt.gridDimZ = 1;
    CHECK(!isosurface::canExtract(pt), "canExtract rejects 2D grid");
    RenderMesh noscalar = box; noscalar.scalars.clear();
    CHECK(!isosurface::canExtract(noscalar), "canExtract rejects no-scalar");
    CHECK(!isosurface::canExtract(RenderMesh{}), "canExtract rejects empty mesh");
}

// Radial scalar field on an N^3 grid; low region is a closed interior blob.
static RenderMesh makeSphereGrid(int n, float cx, float cy, float cz) {
    RenderMesh m;
    m.gridDimX = n; m.gridDimY = n; m.gridDimZ = n;
    m.datasetType = "IMAGEDATA";
    const int total = n*n*n;
    m.vertices.resize((size_t)total*3);
    m.scalars.resize(total);
    for (int z = 0; z < n; ++z)
    for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
        size_t i = (size_t)(x + y*n + z*n*n);
        float px = static_cast<float>(x), py = static_cast<float>(y), pz = static_cast<float>(z);
        m.vertices[i*3+0] = px; m.vertices[i*3+1] = py; m.vertices[i*3+2] = pz;
        float dx = px-cx, dy = py-cy, dz = pz-cz;
        m.scalars[i] = dx*dx + dy*dy + dz*dz;  // dist^2
    }
    m.scalarName = "d2";
    mesh_utils::computeBounds(m);
    return m;
}

static void testClosedSurface() {
    printf("[TEST] closed-isosurface topology (synthetic sphere)\n");
    // 5x5x5 grid, scalar = dist^2 from center (2,2,2). iso chosen strictly
    // between node levels so no node lands exactly on the level -> a smooth,
    // closed, manifold sphere with no ambiguous 4-4 cells. (iso=3.0 lands
    // exactly on corner nodes (dist^2=3) and triggers ambiguous faces;
    // iso=2.5 is safely between levels)
    RenderMesh g = makeSphereGrid(5, 2.0f, 2.0f, 2.0f);
    CHECK(isosurface::canExtract(g), "canExtract(sphere grid)");
    auto r = isosurface::extractIsosurface(g, {2.5f});
    CHECK(!r.vertices.empty(), "sphere iso=2.5 -> non-empty");
    CHECK(r.vertices.size()/3 == r.scalars.size(), "sphere scalars match verts");
    CHECK(r.vertices.size()/3 == r.normals.size()/3, "sphere normals match verts");
    for (uint32_t i : r.indices) CHECK(i < r.vertices.size()/3, "sphere index in range");
    // Every vertex must sit strictly inside the volume bounds.
    const double bmin[3] = {g.bounds.minX, g.bounds.minY, g.bounds.minZ};
    const double bmax[3] = {g.bounds.maxX, g.bounds.maxY, g.bounds.maxZ};
    bool inBounds = true;
    for (size_t i = 0; i < r.vertices.size()/3; ++i)
        for (int c = 0; c < 3; ++c)
            if (r.vertices[i*3+c] < bmin[c]-1e-4 || r.vertices[i*3+c] > bmax[c]+1e-4) inBounds=false;
    CHECK(inBounds, "sphere vertices within volume bounds");

    Closure cl = closureOf(r);
    printf("  sphere: verts=%zu idx=%zu openEdges=%d nonManifold=%d degen=%d euler=%d\n",
           r.vertices.size()/3, r.indices.size(), cl.openEdges, cl.nonManifoldEdges,
           cl.degenerateFaces, eulerChi(r));
    CHECK(cl.degenerateFaces == 0, "sphere no degenerate triangles");
    CHECK(cl.nonManifoldEdges == 0, "sphere no non-manifold edges");
    CHECK(cl.watertight, "sphere isosurface is watertight (closed manifold)");
    CHECK(eulerChi(r) == 2, "sphere euler characteristic == 2 (genus 0)");

    // Lowering the level grows the sheet; raising above the max collapses it.
    RenderMesh empty = isosurface::extractIsosurface(g, {1e9f});
    CHECK(empty.vertices.empty(), "sphere iso above max -> empty");
}

static void testParsedSample() {
    printf("[TEST] parsed IMAGEDATA sample\n");
    const std::string path = "../samples/IMAGE_DATA_xml_ascii.vti";
    if (!fileExists(path)) { printf("  SKIP (missing): %s\n", path.c_str()); return; }
    RenderMesh m = loadMeshFile(path);
    CHECK(m.gridDimX == 2 && m.gridDimY == 2 && m.gridDimZ == 2, "sample gridDim 2x2x2");
    CHECK(isosurface::canExtract(m), "sample canExtract");

    // Node values are 0,0.5,0.5,1,0.5,0.5,1,1 -> iso=0.25 -> 1 triangle at the
    // three mid-edges of corner 0 (node 0 is the lone low corner).
    auto r = isosurface::extractIsosurface(m, {0.25f});
    CHECK(!r.vertices.empty(), "sample iso=0.25 -> non-empty");
    CHECK(r.indices.size() == 3, "sample iso=0.25 -> 1 triangle");
    CHECK(r.vertices.size()/3 == 3, "sample iso=0.25 -> 3 vertices");
    CHECK(hasPoint(r, 0.5f, 0.0f, 0.0f), "sample iso=0.25 vertex (0.5,0,0)");
    CHECK(hasPoint(r, 0.0f, 0.5f, 0.0f), "sample iso=0.25 vertex (0,0.5,0)");
    CHECK(hasPoint(r, 0.0f, 0.0f, 0.5f), "sample iso=0.25 vertex (0,0,0.5)");
    CHECK(r.scalars.size() == r.vertices.size()/3, "sample iso=0.25 scalars match verts");
    CHECK(r.normals.size()/3 == r.vertices.size()/3, "sample iso=0.25 normals match verts");

    // Above max (1.0) -> nothing.
    CHECK(isosurface::extractIsosurface(m, {1.5f}).vertices.empty(), "sample iso=1.5 -> empty");
}

int main(){
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testSingleCellExact();
    testClosedSurface();
    testParsedSample();

    printf("\n");
    if (failures == 0) {
        printf("ALL ISOSURFACE CHECKS PASSED (%d checks)\n", checks);
    } else {
        printf("%d/%d CHECK(S) FAILED\n", failures, checks);
    }
    return failures;
}
