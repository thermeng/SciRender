// Standalone check for the PVD collection parser (no Qt/GL).
// Verifies ordering, unique-timestep grouping, relative-path resolution,
// multi-part sequences, and tolerance of malformed entries.
#include "core/pvd_parser.h"
#include "core/mesh_loader.h"
#include "doctest/doctest.h"
#include "support/test_support.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    f << content;
}

// A minimal single-triangle .vtu (matches the parser's ASCII expectations).
std::string miniVtu(float scalar) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%.6f", scalar);
    std::string s = R"(<?xml version="1.0"?>
<VTKFile type="UnstructuredGrid" version="0.1" byte_order="LittleEndian" header_type="UInt32">
  <UnstructuredGrid>
    <Piece NumberOfPoints="3" NumberOfCells="1">
      <Points>
        <DataArray type="Float32" NumberOfComponents="3" format="ascii">
          0 0 0  1 0 0  0 1 0
        </DataArray>
      </Points>
      <Cells>
        <DataArray type="Int32" Name="connectivity" format="ascii">0 1 2</DataArray>
        <DataArray type="Int32" Name="offsets" format="ascii">3</DataArray>
        <DataArray type="Int32" Name="types" format="ascii">5</DataArray>
      </Cells>
      <PointData Scalars="temp">
        <DataArray type="Float32" Name="temp" format="ascii">)" + std::string(buf) + R"(</DataArray>
      </PointData>
    </Piece>
  </UnstructuredGrid>
</VTKFile>
)";
    return s;
}

// Writes the fixture tree into the given scratch directory and returns its path.
std::string writeSandbox(const test_support::TempDir& tmp) {
    const std::string dir = tmp.path().string();

    writeFile(dir + "/frame_000.vtu", miniVtu(10.0f));
    writeFile(dir + "/frame_001.vtu", miniVtu(20.0f));
    writeFile(dir + "/frame_002.vtu", miniVtu(30.0f));

    // Multi-part frame: two pieces merged at t=2.0.
    writeFile(dir + "/part_a_002.vtu", miniVtu(31.0f));
    writeFile(dir + "/part_b_002.vtu", miniVtu(32.0f));

    // Well-formed sequence with: unsorted entries, multi-part timestep,
    // a malformed entry (no file attribute -> skipped at parse time), and an
    // entry with a missing timestep (-> time 0, groups with frame_000).
    writeFile(dir + "/seq.pvd",
              "<?xml version=\"1.0\"?>\n"
              "<VTKFile type=\"Collection\" version=\"0.1\">\n"
              "  <Collection>\n"
              "    <DataSet timestep=\"1.0\" part=\"0\" file=\"frame_001.vtu\"/>\n"
              "    <DataSet timestep=\"0\" part=\"0\" file=\"frame_000.vtu\"/>\n"
              "    <DataSet timestep=\"9.9\"/>\n"
              "    <DataSet timestep=\"2.0\" part=\"1\" file=\"part_b_002.vtu\"/>\n"
              "    <DataSet timestep=\"2.0\" part=\"0\" file=\"part_a_002.vtu\"/>\n"
              "  </Collection>\n"
              "</VTKFile>\n");

    // Missing-file entry must be tolerated at parse time (load failure is a
    // runtime concern, not a parse error).
    writeFile(dir + "/with_missing.pvd",
              "<?xml version=\"1.0\"?>\n"
              "<VTKFile type=\"Collection\"><Collection>\n"
              "  <DataSet timestep=\"0\" part=\"0\" file=\"gone.vtu\"/>\n"
              "  <DataSet timestep=\"1\" part=\"0\" file=\"frame_001.vtu\"/>\n"
              "</Collection></VTKFile>\n");

    // Garbage inputs.
    writeFile(dir + "/not_xml.pvd", "this is not xml at all");
    writeFile(dir + "/empty_collection.pvd",
              "<?xml version=\"1.0\"?><VTKFile type=\"Collection\"><Collection/></VTKFile>\n");
    writeFile(dir + "/missing_root.pvd",
              "<?xml version=\"1.0\"?><SomethingElse><Collection>"
              "<DataSet timestep=\"0\" file=\"frame_000.vtu\"/></Collection></SomethingElse>\n");

    return dir;
}

} // namespace

TEST_SUITE("core") {

TEST_CASE("core.pvd.sequence_structure") {
    test_support::TempDir tmp("pvd");
    const std::string dir = writeSandbox(tmp);

    PvdSequence seq = parsePVD(dir + "/seq.pvd");
    REQUIRE_MESSAGE(seq.frameCount() == 3, "expected 3 unique timesteps, got " << seq.frameCount());
    const bool sorted =
        (seq.timesteps[0] == 0.0) && (seq.timesteps[1] == 1.0) && (seq.timesteps[2] == 2.0);
    CHECK_MESSAGE(sorted, "timesteps sorted ascending");
    CHECK_MESSAGE(seq.entries.size() == 4, "4 usable entries (malformed skipped), got "
                                                << seq.entries.size());

    auto f0 = seq.filesForFrame(0);
    const bool f0Ok = (f0.size() == 1) && (f0[0].find("frame_000.vtu") != std::string::npos);
    CHECK_MESSAGE(f0Ok, "frame 0 resolves relative path");

    auto f1 = seq.filesForFrame(1);
    const bool f1Ok = (f1.size() == 1) && (f1[0].find("frame_001.vtu") != std::string::npos);
    CHECK_MESSAGE(f1Ok, "frame 1 present");

    // Multi-part frame ordered by part number regardless of doc order.
    auto f2 = seq.filesForFrame(2);
    CHECK_MESSAGE(f2.size() == 2, "frame 2 has two parts, got " << f2.size());
    if (f2.size() == 2) {
        const bool partsOrdered =
            (f2[0].find("part_a") != std::string::npos) && (f2[1].find("part_b") != std::string::npos);
        CHECK_MESSAGE(partsOrdered, "parts ordered by part attribute");
    }
    CHECK_MESSAGE(seq.frameTime(2) == 2.0, "frameTime reflects the timestep");
}

TEST_CASE("core.pvd.missing_file_entry_tolerated") {
    test_support::TempDir tmp("pvd");
    const std::string dir = writeSandbox(tmp);
    PvdSequence seq = parsePVD(dir + "/with_missing.pvd");
    CHECK_MESSAGE(seq.frameCount() == 2, "missing referenced file still parses as an entry");
}

TEST_CASE("core.pvd.garbage_input") {
    test_support::TempDir tmp("pvd");
    const std::string dir = writeSandbox(tmp);
    PvdSequence seq = parsePVD(dir + "/not_xml.pvd");
    CHECK_MESSAGE(seq.frameCount() == 0, "garbage XML yields empty sequence");
}

TEST_CASE("core.pvd.empty_collection") {
    test_support::TempDir tmp("pvd");
    const std::string dir = writeSandbox(tmp);
    PvdSequence seq = parsePVD(dir + "/empty_collection.pvd");
    CHECK_MESSAGE(seq.frameCount() == 0, "empty collection yields empty sequence");
}

TEST_CASE("core.pvd.missing_root") {
    test_support::TempDir tmp("pvd");
    const std::string dir = writeSandbox(tmp);
    PvdSequence seq = parsePVD(dir + "/missing_root.pvd");
    CHECK_MESSAGE(seq.frameCount() == 0, "missing VTKFile root yields empty sequence");
}

TEST_CASE("core.pvd.nonexistent_file") {
    test_support::TempDir tmp("pvd");
    const std::string dir = writeSandbox(tmp);
    PvdSequence seq = parsePVD(dir + "/does_not_exist.pvd");
    CHECK_MESSAGE(seq.frameCount() == 0, "missing .pvd yields empty sequence");
}

TEST_CASE("core.pvd.frames_load") {
    test_support::TempDir tmp("pvd");
    const std::string dir = writeSandbox(tmp);
    PvdSequence seq = parsePVD(dir + "/seq.pvd");
    REQUIRE_MESSAGE(seq.frameCount() > 0, "sequence must not be empty");
    bool allLoaded = true;
    for (int i = 0; i < seq.frameCount() && allLoaded; ++i) {
        for (const std::string& f : seq.filesForFrame(i)) {
            try {
                RenderMesh m = loadMeshFile(f);
                if (m.vertices.empty()) {
                    allLoaded = false;
                    break;
                }
            } catch (...) {
                allLoaded = false;
                break;
            }
        }
    }
    CHECK_MESSAGE(allLoaded, "every referenced frame file parses via loadMeshFile()");
}

} // TEST_SUITE("core")
