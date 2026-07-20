// tests/parse_regression.cpp
// Standalone regression check for the VTK parser core.
// Compiles the REAL sources (no Qt/GL). Run via tests/run_tests.{bat,sh}
#include "core/mesh_loader.h"
#include "core/mesh_quality.h"
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>

static int failures = 0;
#define CHECK(cond, msg) do { if(!(cond)){ printf("FAIL: %s\n", msg); ++failures; } } while(0)

// ponytail: golden table; tris + quality only (verts vary via flat-shading split).
struct Gold { const char* name; size_t tris; int watertight; int open; };
static const Gold GOLD[] = {
    {"STRUCTURED_POINTS_ascii.vtk",                       972, 1, 0},
    {"STRUCTURED_POINTS_binary.vtk",                      972, 1, 0},
    {"RECTILINEAR_shapes_attributes_ascii.vtk",        28812, 1, 0},
    {"RECTILINEAR_shapes_attributes_binary.vtk",       28812, 1, 0},
    {"STRUCTURED_GRID_block_four_fields_vectors_ascii.vtk",  192, 1, 0},
    {"STRUCTURED_GRID_block_four_fields_vectors_binary.vtk", 192, 1, 0},
    {"UNSTRUCTURED_GRID_cube_MultipleScalar_ascii.vtk",     12, 1, 0},
    {"UNSTRUCTURED_GRID_cube_MultipleScalar_binary.vtk",    12, 1, 0},
    {"curvilinear_grid.vtk",                               18, 0, 12},
};

int main(){
    namespace fs = std::filesystem;
    const std::string samples = "../samples/";   // run from tests/
    for (const auto& g : GOLD) {
        std::string path = samples + g.name;
        if (!fs::exists(path)) { printf("SKIP (missing): %s\n", g.name); continue; }
        RenderMesh m = parseVTK(path);
        uint32_t vc = (uint32_t)(m.vertices.size()/3);
        uint32_t mx = 0; for (uint32_t x : m.indices) mx = (x>mx)?x:mx;
        size_t tris = m.indices.size()/3;
        CHECK(mx < vc,                     (std::string("OOB index in ")+g.name).c_str());
        CHECK(tris == g.tris,              (std::string("tris mismatch ")+g.name).c_str());
        MeshQuality q = analyzeMeshQuality(m);
        CHECK((int)q.watertight == g.watertight, (std::string("watertight mismatch ")+g.name).c_str());
        CHECK((int)q.openEdges == g.open,       (std::string("open mismatch ")+g.name).c_str());
        printf("ok %s (tris=%zu wt=%d open=%d)\n", g.name, tris, (int)q.watertight, q.openEdges);
    }
    if (failures==0) printf("\nALL PARSER REGRESSION CHECKS PASSED\n");
    else printf("\n%d CHECK(S) FAILED\n", failures);
    return failures;
}
