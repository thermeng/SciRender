// Standalone regression check for the VTK parser core.
// Compiles the REAL sources through CoreLib (no Qt/GL).
//
// One test case per sample fixture, so `ctest` names the file that broke.
#include "core/mesh_loader.h"
#include "core/mesh_quality.h"
#include "doctest/doctest.h"
#include "support/test_support.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

// Seam expectations - asserted through the RenderMesh predicates/resolvers
// that renderer-side consumers actually call (hasScalarData, vectorFieldData,
// cellVectorFieldData, cellCenters, attributes->scalarMin/Max). Zero values
// mean "don't care" so existing geometry-only rows stay untouched.
struct SeamExpect {
    float smin = 0.0f;        // expected active-scalar range min
    float smax = 0.0f;        // expected active-scalar range max (smin==smax==0 -> don't care)
    int pointVecFields = -1;  // usable point-vector fields (-1 = don't care)
    int cellVecFields = -1;   // usable cell-vector fields (-1 = don't care)
    int cellCenterCount = -1; // expected cellCenters.size() (-1 = don't care)
};

struct Gold {
    const char* name;
    size_t tris;
    int watertight;
    int open;
    SeamExpect seam;
};

const Gold GOLD[] = {
    {"STRUCTURED_POINTS_ascii.vtk", 972, 1, 0},
    {"STRUCTURED_POINTS_binary.vtk", 972, 1, 0},
    {"RECTILINEAR_shapes_attributes_ascii.vtk", 28812, 1, 0},
    {"RECTILINEAR_shapes_attributes_binary.vtk", 28812, 1, 0},
    {"STRUCTURED_GRID_block_four_fields_vectors_ascii.vtk", 192, 1, 0},
    {"STRUCTURED_GRID_block_four_fields_vectors_binary.vtk", 192, 1, 0},
    {"UNSTRUCTURED_GRID_cube_MultipleScalar_ascii.vtk", 12, 1, 0},
    {"UNSTRUCTURED_GRID_cube_MultipleScalar_binary.vtk", 12, 1, 0},
    {"POLYDATA_triangles_ascii.vtk", 2, 0, 4},
    {"UNSTRUCTURED_GRID_tetra_ascii.vtk", 4, 1, 0},
    {"UNSTRUCTURED_GRID_mixed_cells_ascii.vtk", 30, 1, 0},
    {"UNSTRUCTURED_GRID_field_attrs_ascii.vtk", 4, 1, 0},
    {"cube_ascii.stl", 12, 1, 0},
    {"cube_binary.stl", 12, 1, 0},
    {"STRUCTURED_GRID_xml_ascii.vts", 12, 1, 0},
    {"IMAGE_DATA_xml_ascii.vti", 12, 1, 0},
    {"POLYDATA_xml_ascii.vtp", 1, 0, 3},
    {"UNSTRUCTURED_GRID_xml_ascii.vtu", 4, 1, 0},
    {"RECTILINEAR_GRID_xml_ascii.vtr", 12, 1, 0},
    {"UNSTRUCTURED_GRID_xml_multipiece.vtu", 24, 1, 0},
    {"UNSTRUCTURED_GRID_xml_binary_appended.vtu", 12, 1, 0},
    {"UNSTRUCTURED_GRID_xml_zlib.vtu", 12, 1, 0},
    {"POLYDATA_xml_binary_appended.vtp", 2, 0, 4},
    {"RECTILINEAR_GRID_xml_binary.vtr", 12, 1, 0},
    {"UNSTRUCTURED_GRID_xml_lz4.vtu", 12, 1, 0},
    {"UNSTRUCTURED_GRID_xml_lzma.vtu", 12, 1, 0},
    {"UNSTRUCTURED_GRID_xml_int64.vtu", 12, 1, 0},
    {"POLYDATA_xml_multicomponent.vtp", 1, 0, 3},
    {"MULTIBLOCK_xml_multiblock.vtm", 16, 0, 0},
    // Cell-data seam fixtures: 2 disjoint tets, POINT_DATA "temperature"
    // 100..800, CELL_DATA SCALARS "pressure" + VECTORS "cellVelocity". Same
    // geometry and fields in legacy and XML form - the XML twins must produce
    // the identical seam surface (cell vectors, cell centers, scalar range)
    // or the build breaks.
    {"UNSTRUCTURED_GRID_celldata_ascii.vtk", 8, 1, 0, {100.0f, 800.0f, -1, 1, 2}},
    {"UNSTRUCTURED_GRID_xml_celldata_ascii.vtu", 8, 1, 0, {100.0f, 800.0f, -1, 1, 2}},
    {"UNSTRUCTURED_GRID_xml_int64_celldata.vtu", 8, 1, 0, {100.0f, 800.0f, -1, 1, 2}},
};

const Gold& goldFor(const char* name) {
    for (const auto& g : GOLD)
        if (std::strcmp(g.name, name) == 0)
            return g;
    static const Gold missing{"<unknown sample>", 0, 0, 0, {}};
    return missing;
}

void checkParseCase(const Gold& g) {
    CAPTURE(g.name);

    const std::filesystem::path path = test_support::samplePath(g.name);
    REQUIRE_MESSAGE(std::filesystem::exists(path),
                    "required sample fixture is missing: " << path.string());

    RenderMesh m = loadMeshFile(path.string());

    uint32_t vc = static_cast<uint32_t>(m.vertices.size() / 3);
    uint32_t mx = 0;
    for (uint32_t x : m.indices)
        mx = (x > mx) ? x : mx;
    size_t tris = m.indices.size() / 3;

    CHECK_MESSAGE(mx < vc, "out-of-bounds index " << mx << " (vertex count " << vc << ")");
    CHECK_MESSAGE(tris == g.tris, "triangle count mismatch: got " << tris << ", expected " << g.tris);

    MeshQuality q = analyzeMeshQuality(m);
    CHECK_MESSAGE(static_cast<int>(q.watertight) == g.watertight,
                  "watertight mismatch: got " << static_cast<int>(q.watertight) << ", expected "
                                              << g.watertight);
    CHECK_MESSAGE(static_cast<int>(q.openEdges) == g.open,
                  "open edge mismatch: got " << static_cast<int>(q.openEdges) << ", expected " << g.open);

    // -- Seam assertions ----------------------------------------------------
    const SeamExpect& se = g.seam;
    if (!(se.smin == 0.0f && se.smax == 0.0f)) {
        bool rangeOk = m.attributes.has_value();
        if (rangeOk) {
            const float amin = m.attributes->scalarMin;
            const float amax = m.attributes->scalarMax;
            rangeOk = std::isfinite(amin) && std::isfinite(amax) && std::abs(amin - se.smin) < 1e-3f &&
                      std::abs(amax - se.smax) < 1e-3f;
        }
        CHECK_MESSAGE(rangeOk, "scalar range mismatch (expected " << se.smin << ".." << se.smax << ")");
    }
    if (se.pointVecFields >= 0) {
        int usable = 0;
        for (const auto& n : m.availableVectorNames) {
            size_t cnt = 0;
            if (m.vectorFieldData(n, cnt) != nullptr && cnt > 0)
                ++usable;
        }
        CHECK_MESSAGE(usable == se.pointVecFields, "point vector field count: got " << usable
                                                       << ", expected " << se.pointVecFields);
        const bool hasPointVectors = usable > 0;
        CHECK_MESSAGE(m.meshHasVectors() == hasPointVectors, "meshHasVectors inconsistent");
        if (m.meshHasVectors())
            CHECK_MESSAGE(m.pointVectorCount == m.sourcePointCount,
                          "pointVectorCount != sourcePointCount");
    }
    if (se.cellVecFields >= 0) {
        int usable = 0;
        for (const auto& n : m.availableCellVectorNames) {
            size_t cnt = 0;
            if (m.cellVectorFieldData(n, cnt) != nullptr && cnt > 0)
                ++usable;
        }
        CHECK_MESSAGE(usable == se.cellVecFields, "cell vector field count: got " << usable
                                                      << ", expected " << se.cellVecFields);
        const bool hasCellVectors = usable > 0;
        CHECK_MESSAGE(m.meshHasCellVectors() == hasCellVectors, "meshHasCellVectors inconsistent");
    }
    if (se.cellCenterCount >= 0) {
        bool centersOk = (static_cast<int>(m.cellCenters.size()) == se.cellCenterCount);
        for (const auto& c : m.cellCenters) {
            if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z))
                centersOk = false;
        }
        CHECK_MESSAGE(centersOk, "cellCenters count/finite mismatch");
    }
}

} // namespace

TEST_SUITE("core") {

#define PARSE_CASE(sample)                                                                        \
    TEST_CASE("core.parse." sample) {                                                             \
        checkParseCase(goldFor(sample));                                                           \
    }

PARSE_CASE("STRUCTURED_POINTS_ascii.vtk")
PARSE_CASE("STRUCTURED_POINTS_binary.vtk")
PARSE_CASE("RECTILINEAR_shapes_attributes_ascii.vtk")
PARSE_CASE("RECTILINEAR_shapes_attributes_binary.vtk")
PARSE_CASE("STRUCTURED_GRID_block_four_fields_vectors_ascii.vtk")
PARSE_CASE("STRUCTURED_GRID_block_four_fields_vectors_binary.vtk")
PARSE_CASE("UNSTRUCTURED_GRID_cube_MultipleScalar_ascii.vtk")
PARSE_CASE("UNSTRUCTURED_GRID_cube_MultipleScalar_binary.vtk")
PARSE_CASE("POLYDATA_triangles_ascii.vtk")
PARSE_CASE("UNSTRUCTURED_GRID_tetra_ascii.vtk")
PARSE_CASE("UNSTRUCTURED_GRID_mixed_cells_ascii.vtk")
PARSE_CASE("UNSTRUCTURED_GRID_field_attrs_ascii.vtk")
PARSE_CASE("cube_ascii.stl")
PARSE_CASE("cube_binary.stl")
PARSE_CASE("STRUCTURED_GRID_xml_ascii.vts")
PARSE_CASE("IMAGE_DATA_xml_ascii.vti")
PARSE_CASE("POLYDATA_xml_ascii.vtp")
PARSE_CASE("UNSTRUCTURED_GRID_xml_ascii.vtu")
PARSE_CASE("RECTILINEAR_GRID_xml_ascii.vtr")
PARSE_CASE("UNSTRUCTURED_GRID_xml_multipiece.vtu")
PARSE_CASE("UNSTRUCTURED_GRID_xml_binary_appended.vtu")
PARSE_CASE("UNSTRUCTURED_GRID_xml_zlib.vtu")
PARSE_CASE("POLYDATA_xml_binary_appended.vtp")
PARSE_CASE("RECTILINEAR_GRID_xml_binary.vtr")
PARSE_CASE("UNSTRUCTURED_GRID_xml_lz4.vtu")
PARSE_CASE("UNSTRUCTURED_GRID_xml_lzma.vtu")
PARSE_CASE("UNSTRUCTURED_GRID_xml_int64.vtu")
PARSE_CASE("POLYDATA_xml_multicomponent.vtp")
PARSE_CASE("MULTIBLOCK_xml_multiblock.vtm")
PARSE_CASE("UNSTRUCTURED_GRID_celldata_ascii.vtk")
PARSE_CASE("UNSTRUCTURED_GRID_xml_celldata_ascii.vtu")
PARSE_CASE("UNSTRUCTURED_GRID_xml_int64_celldata.vtu")

#undef PARSE_CASE

} // TEST_SUITE("core")
