// CGNSReader contract tests. Fixtures are written with the CGNS mid-level API
// (tests/support/cgns_fixtures.h), so no binary fixtures live in the repo.
// When CGNS is not built (SCIRENDER_HAVE_CGNS undefined) this is a passing
// no-op, mirroring the MPI smoke test convention.

#include "doctest/doctest.h"

#ifdef SCIRENDER_HAVE_CGNS

#include "io/CGNSReader.h"
#include "support/cgns_fixtures.h"

#include <cmath>
#include <filesystem>
#include <string>

namespace {
std::string tmpPath(const char* name) {
    const std::string dir = SCIRENDER_TEST_TMP_DIR;
    std::filesystem::create_directories(dir);
    return dir + "/" + name;
}
} // namespace

TEST_CASE("fvm.cgns.hex_cube_reads_with_boundary_groups") {
    const std::string path = tmpPath("cgns_hex_cube.cgns");
    cgns_fixtures::writeHexCube(path);

    CHECK(fvm::CGNSReader<2>::detectDimension(path) == 3);
    auto r = fvm::CGNSReader<3>::read(path);
    CHECK(r.mesh.nCells() == 8);
    CHECK(r.mesh.vertices().size() == 27);
    REQUIRE(r.boundaryGroups.size() == 6);
    std::size_t grouped = 0;
    for (const auto& [name, faces] : r.boundaryGroups) {
        CHECK(faces.size() == 4);
        for (std::size_t f : faces) {
            CHECK(r.mesh.neighbor(f) == std::size_t(-1));
        }
        grouped += faces.size();
    }
    // A 2x2x2 cube has exactly 24 boundary faces, all assigned.
    CHECK(grouped == 24);
    for (std::size_t c = 0; c < r.mesh.nCells(); ++c) {
        CHECK(r.mesh.cellVolume(c) == doctest::Approx(1.0).epsilon(1e-9));
    }
}

TEST_CASE("fvm.cgns.mixed_tet_wedge_pyramid_volumes") {
    const std::string path = tmpPath("cgns_mixed.cgns");
    cgns_fixtures::writeMixedCells(path);
    CHECK(fvm::CGNSReader<2>::detectDimension(path) == 3);
    auto r = fvm::CGNSReader<3>::read(path);
    REQUIRE(r.mesh.nCells() == 3);
    CHECK(r.mesh.cells()[0].size() == 4);
    CHECK(r.mesh.cells()[1].size() == 6);
    CHECK(r.mesh.cells()[2].size() == 5);
    CHECK(r.mesh.cellVolume(0) == doctest::Approx(1.0 / 6.0).epsilon(1e-9));
    CHECK(r.mesh.cellVolume(1) == doctest::Approx(0.5).epsilon(1e-9));
    CHECK(r.mesh.cellVolume(2) == doctest::Approx(1.0 / 3.0).epsilon(1e-9));
    CHECK(r.boundaryGroups.empty());
}

TEST_CASE("fvm.cgns.2d_quads_with_elementlist_and_pointlist_bcs") {
    const std::string path = tmpPath("cgns_quad2d.cgns");
    cgns_fixtures::write2DQuad(path);
    CHECK(fvm::CGNSReader<2>::detectDimension(path) == 2);
    auto r = fvm::CGNSReader<2>::read(path);
    CHECK(r.mesh.nCells() == 4);
    CHECK(r.mesh.vertices().size() == 9);
    REQUIRE(r.boundaryGroups.size() == 4);
    std::size_t grouped = 0;
    for (const auto& [name, faces] : r.boundaryGroups) {
        CHECK(faces.size() == 2);
        grouped += faces.size();
    }
    CHECK(grouped == 8); // perimeter edges of a 2x2 grid
    for (std::size_t c = 0; c < r.mesh.nCells(); ++c) {
        CHECK(r.mesh.cellVolume(c) == doctest::Approx(1.0).epsilon(1e-9));
    }
}

TEST_CASE("fvm.cgns.mixed_section_stream_with_element_bc") {
    const std::string path = tmpPath("cgns_mixed_section.cgns");
    cgns_fixtures::writeMixedSection(path);
    CHECK(fvm::CGNSReader<2>::detectDimension(path) == 3);
    auto r = fvm::CGNSReader<3>::read(path);
    REQUIRE(r.mesh.nCells() == 2);
    CHECK(r.mesh.cellVolume(0) == doctest::Approx(1.0).epsilon(1e-9));
    CHECK(r.mesh.cellVolume(1) == doctest::Approx(1.0 / 6.0).epsilon(1e-9));
    REQUIRE(r.boundaryGroups.size() == 1);
    CHECK(r.boundaryGroups.at("xminus").size() == 1);
}

TEST_CASE("fvm.cgns.pointrange_bcs_element_and_vertex") {
    const std::string path = tmpPath("cgns_pointrange.cgns");
    cgns_fixtures::writePointRangeBCs(path);
    auto r = fvm::CGNSReader<3>::read(path);
    REQUIRE(r.mesh.nCells() == 1);
    REQUIRE(r.boundaryGroups.size() == 3);
    CHECK(r.boundaryGroups.at("xside").size() == 1);
    CHECK(r.boundaryGroups.at("bottom").size() == 1);
    CHECK(r.boundaryGroups.at("vrange").size() == 1);
    // the vertex range 1..4 selects the same face as element range [3,3]
    CHECK(r.boundaryGroups.at("bottom")[0] == r.boundaryGroups.at("vrange")[0]);
}

TEST_CASE("fvm.cgns.surface_mesh_in_3d_is_rejected") {
    const std::string path = tmpPath("cgns_surface3d.cgns");
    cgns_fixtures::writeSurface3D(path);
    CHECK_THROWS_WITH(fvm::CGNSReader<2>::read(path),
                      doctest::Contains("surface mesh"));
}

TEST_CASE("fvm.cgns.structured_zone_is_rejected") {
    const std::string path = tmpPath("cgns_structured.cgns");
    cgns_fixtures::writeStructured(path);
    CHECK_THROWS_WITH(fvm::CGNSReader<2>::detectDimension(path),
                      doctest::Contains("unstructured"));
    CHECK_THROWS_WITH(fvm::CGNSReader<3>::read(path),
                      doctest::Contains("unstructured"));
}

TEST_CASE("fvm.cgns.polyhedral_elements_are_rejected") {
    const std::string path = tmpPath("cgns_poly.cgns");
    cgns_fixtures::writePolyhedral(path);
    CHECK_THROWS_WITH(fvm::CGNSReader<3>::read(path),
                      doctest::Contains("polyhedral"));
}

#else

TEST_CASE("fvm.cgns.not_built_is_noop") {
    MESSAGE("CGNS not built (SCIRENDER_HAVE_CGNS undefined); reader tests skipped");
    CHECK(true);
}

#endif // SCIRENDER_HAVE_CGNS
