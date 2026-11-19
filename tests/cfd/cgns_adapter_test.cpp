// cfd::loadMesh on a CGNS file: the full CFD ingestion contract — solver mesh,
// boundary patches, and the RenderMesh the volume/slice/isosurface paths use.
// No-op when CGNS is not built.

#include "doctest/doctest.h"

#ifdef SCIRENDER_HAVE_CGNS

#include "cfd/cfd_mesh_adapter.h"
#include "core/cell_topology.h"
#include "core/unstructured_volume.h"
#include "mesh/Mesh.h"
#include "support/cgns_fixtures.h"

#include <filesystem>
#include <string>

TEST_CASE("cfd.cgns.loadmesh_produces_solver_and_render_mesh") {
    const std::string dir = SCIRENDER_TEST_TMP_DIR;
    std::filesystem::create_directories(dir);
    const std::string path = dir + "/cgns_adapter_cube.cgns";
    cgns_fixtures::writeHexCube(path);

    auto res = cfd::loadMesh(path);
    REQUIRE_MESSAGE(res.error.empty(), res.error);
    CHECK(res.dim == 3);
    REQUIRE(res.mesh3);
    REQUIRE(res.renderMesh);
    CHECK(res.mesh3->nCells() == 8);
    // 8 hexes: 24 boundary faces + 12 internal shared faces.
    CHECK(res.mesh3->nFaces() == 36);

    REQUIRE(res.patches.size() == 6);
    for (const auto& p : res.patches) CHECK(p.faceCount == 4);

    const auto& rm = *res.renderMesh;
    CHECK(rm.fileFormat == "CGNS");
    CHECK(rm.hasUnstructuredVolumeCells());
    CHECK(rm.unstructuredVolumeCellCount == 8);
    CHECK(rm.cellConnectivity.size() == 8 * 8);
    for (std::uint8_t t : rm.cellTypes) CHECK(t == cell_topology::kHexahedron);
    CHECK(volume::eligible(rm));
}

#else

TEST_CASE("cfd.cgns.not_built_is_noop") {
    MESSAGE("CGNS not built (SCIRENDER_HAVE_CGNS undefined); adapter test skipped");
    CHECK(true);
}

#endif // SCIRENDER_HAVE_CGNS
