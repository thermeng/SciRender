// tests/pvd_test.cpp
// Standalone check for the PVD collection parser (no Qt/GL).
// Verifies ordering, unique-timestep grouping, relative-path resolution,
// multi-part sequences, and tolerance of malformed entries.
#include "core/pvd_parser.h"
#include "core/mesh_loader.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#endif

static int failures = 0;
#define CHECK(cond, msg) do { if(!(cond)){ printf("  [FAIL] %s\n", msg); ++failures; } } while(0)

static void writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    f << content;
}

static void makeDir(const std::string& path) {
#ifdef _WIN32
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0755);
#endif
}

// A minimal single-triangle .vtu (matches the parser's ASCII expectations).
static std::string miniVtu(float scalar) {
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

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // ── Fixture tree in a temp sandbox dir ────────────────────────────────
    const std::string dir = "../samples/pvd_test_sandbox";
    makeDir(dir);

    writeFile(dir + "/frame_000.vtu", miniVtu(10.0f));
    writeFile(dir + "/frame_001.vtu", miniVtu(20.0f));
    writeFile(dir + "/frame_002.vtu", miniVtu(30.0f));

    // Multi-part frame: two pieces merged at t=2.0.
    writeFile(dir + "/part_a_002.vtu", miniVtu(31.0f));
    writeFile(dir + "/part_b_002.vtu", miniVtu(32.0f));

    // Well-formed sequence with: unsorted entries, multi-part timestep,
    // a malformed entry (no file attribute → skipped at parse time), and an
    // entry with a missing timestep (→ time 0, groups with frame_000).
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

    int passed = 0, total = 0;

    // ── Parse + structure checks ──────────────────────────────────────────
    {
        ++total;
        printf("[CASE] seq.pvd\n");
        PvdSequence seq = parsePVD(dir + "/seq.pvd");
        CHECK(seq.frameCount() == 3, "expected 3 unique timesteps");
        if (seq.frameCount() == 3) {
            CHECK(seq.timesteps[0] == 0.0 && seq.timesteps[1] == 1.0 && seq.timesteps[2] == 2.0,
                  "timesteps sorted ascending");
            CHECK(seq.entries.size() == 4, "4 usable entries (malformed skipped)");

            auto f0 = seq.filesForFrame(0);
            CHECK(f0.size() == 1 && f0[0].find("frame_000.vtu") != std::string::npos,
                  "frame 0 resolves relative path");

            auto f1 = seq.filesForFrame(1);
            CHECK(f1.size() == 1 && f1[0].find("frame_001.vtu") != std::string::npos,
                  "frame 1 present");

            // Multi-part frame ordered by part number regardless of doc order.
            auto f2 = seq.filesForFrame(2);
            CHECK(f2.size() == 2, "frame 2 has two parts");
            if (f2.size() == 2) {
                CHECK(f2[0].find("part_a") != std::string::npos &&
                      f2[1].find("part_b") != std::string::npos,
                      "parts ordered by part attribute");
            }
            CHECK(seq.frameTime(2) == 2.0, "frameTime reflects the timestep");
            ++passed;
        }
    }

    {
        ++total;
        printf("[CASE] with_missing.pvd\n");
        PvdSequence seq = parsePVD(dir + "/with_missing.pvd");
        CHECK(seq.frameCount() == 2, "missing referenced file still parses as an entry");
        ++passed;
    }

    {
        ++total;
        printf("[CASE] not_xml.pvd\n");
        PvdSequence seq = parsePVD(dir + "/not_xml.pvd");
        CHECK(seq.frameCount() == 0, "garbage XML yields empty sequence");
        ++passed;
    }

    {
        ++total;
        printf("[CASE] empty_collection.pvd\n");
        PvdSequence seq = parsePVD(dir + "/empty_collection.pvd");
        CHECK(seq.frameCount() == 0, "empty collection yields empty sequence");
        ++passed;
    }

    {
        ++total;
        printf("[CASE] missing_root.pvd\n");
        PvdSequence seq = parsePVD(dir + "/missing_root.pvd");
        CHECK(seq.frameCount() == 0, "missing VTKFile root yields empty sequence");
        ++passed;
    }

    {
        ++total;
        printf("[CASE] nonexistent.pvd\n");
        PvdSequence seq = parsePVD(dir + "/does_not_exist.pvd");
        CHECK(seq.frameCount() == 0, "missing .pvd yields empty sequence");
        ++passed;
    }

    {
        ++total;
        printf("[CASE] frames load through dispatcher\n");
        PvdSequence seq = parsePVD(dir + "/seq.pvd");
        bool allLoaded = seq.frameCount() > 0;
        for (int i = 0; i < seq.frameCount() && allLoaded; ++i) {
            for (const std::string& f : seq.filesForFrame(i)) {
                try {
                    RenderMesh m = loadMeshFile(f);
                    if (m.vertices.empty()) { allLoaded = false; break; }
                } catch (...) { allLoaded = false; break; }
            }
        }
        CHECK(allLoaded, "every referenced frame file parses via loadMeshFile()");
        ++passed;
    }

    // ── Summary ────────────────────────────────────────────────────────────
    printf("\n[PVD TEST] %d/%d case groups passed", passed, total);
    if (failures > 0) printf(" — %d FAILURES\n", failures);
    else printf(" — OK\n");
    return failures == 0 ? 0 : 1;
}
