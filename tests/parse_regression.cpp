// tests/parse_regression.cpp
// Standalone regression check for the VTK parser core.
// Compiles the REAL sources (no Qt/GL). Run via tests/run_tests.{bat,sh}
#include "core/mesh_loader.h"
#include "core/mesh_quality.h"
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <chrono>

static bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

static int failures = 0;
static std::vector<std::string> failedFiles;

#define CHECK(cond, file, msg) do { if(!(cond)){ \
    printf("  [FAIL] %s: %s\n", file, msg); ++failures; failedFiles.push_back(file); } } while(0)


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
    {"cube_ascii.stl",                                     12, 1, 0},
    {"cube_binary.stl",                                    12, 1, 0},
    {"STRUCTURED_GRID_xml_ascii.vts",                      12, 1, 0},
    {"IMAGE_DATA_xml_ascii.vti",                           12, 1, 0},
    {"POLYDATA_xml_ascii.vtp",                              1, 0, 3},
    {"UNSTRUCTURED_GRID_xml_ascii.vtu",                     4, 1, 0},
    {"RECTILINEAR_GRID_xml_ascii.vtr",                     12, 1, 0},
    {"UNSTRUCTURED_GRID_xml_multipiece.vtu",               24, 0, 0},
    {"UNSTRUCTURED_GRID_xml_binary_appended.vtu",          12, 1, 0},
    {"UNSTRUCTURED_GRID_xml_zlib.vtu",                     12, 1, 0},
    {"POLYDATA_xml_binary_appended.vtp",                    2, 0, 4},
    {"RECTILINEAR_GRID_xml_binary.vtr",                    12, 1, 0},
    {"UNSTRUCTURED_GRID_xml_lz4.vtu",                       12, 1, 0},
    {"UNSTRUCTURED_GRID_xml_lzma.vtu",                      12, 1, 0},
    {"UNSTRUCTURED_GRID_xml_int64.vtu",                     12, 1, 0},
    {"POLYDATA_xml_multicomponent.vtp",                      1, 0, 3},
    {"MULTIBLOCK_xml_multiblock.vtm",                        16, 0, 0}
};

int main(){
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string samples = "../samples/";   // run from tests/
    double totalParseTime = 0.0;
    double totalQualityTime = 0.0;
    for (const auto& g : GOLD) {
        std::string path = samples + g.name;
        if (!fileExists(path)) { printf("SKIP (missing): %s\n", g.name); continue; }
        auto t0 = std::chrono::steady_clock::now();
        RenderMesh m = loadMeshFile(path);
        auto t1 = std::chrono::steady_clock::now();
        double parseMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        totalParseTime += parseMs;
        uint32_t vc = (uint32_t)(m.vertices.size()/3);
        uint32_t mx = 0; for (uint32_t x : m.indices) mx = (x>mx)?x:mx;
        size_t tris = m.indices.size()/3;
        bool caseOk = true;
        if (!(mx < vc))                          { CHECK(mx < vc, g.name, "OOB index"); caseOk = false; }
        if (!(tris == g.tris))                   { CHECK(tris == g.tris, g.name, "tris mismatch"); caseOk = false; }
        auto t2 = std::chrono::steady_clock::now();
        MeshQuality q = analyzeMeshQuality(m);
        auto t3 = std::chrono::steady_clock::now();
        double qualityMs = std::chrono::duration<double, std::milli>(t3 - t2).count();
        totalQualityTime += qualityMs;
        if (!((int)q.watertight == g.watertight)){ CHECK((int)q.watertight == g.watertight, g.name, "watertight mismatch"); caseOk = false; }
        if (!((int)q.openEdges == g.open))       { CHECK((int)q.openEdges == g.open, g.name, "open mismatch"); caseOk = false; }
        printf("%s %s (tris=%zu wt=%d open=%d) [parse=%.1fms quality=%.1fms]\n",
               caseOk ? "[PASS]" : "[FAIL]", g.name, tris, (int)q.watertight, q.openEdges, parseMs, qualityMs);
        fflush(stdout);
    }
    if (failures==0) {
        printf("\nALL PARSER REGRESSION CHECKS PASSED (%zu files, %.1f ms parse, %.1f ms quality, %.1f ms total)\n",
               sizeof(GOLD)/sizeof(GOLD[0]), totalParseTime, totalQualityTime, totalParseTime + totalQualityTime);
    } else {
        printf("\n%d CHECK(S) FAILED in %zu file(s):\n", failures, failedFiles.size());
        for (const auto& f : failedFiles) printf("  - %s\n", f.c_str());
    }
    return failures;
}
