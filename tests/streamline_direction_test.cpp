// tests/streamline_direction_test.cpp
// Standalone verification of arrow/cone direction on streamlines.
// Uses the Divergent_Flow field from the structured-grid VTK fixture to
// validate that generated geometry points along the local vector direction.
// Compiles the REAL sources (no Qt/GL). Run via tests/run_tests.{bat,sh}
#include "render/streamlines/StreamlineSet.h"
#include "core/mesh_loader.h"

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <limits>
#include <fstream>

static bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

static int failures = 0;
static std::vector<std::string> failedFiles;
#define CHECK(cond, msg) do { if(!(cond)){ \
    printf("  [FAIL] %s\n", msg); ++failures; failedFiles.push_back(msg); } } while(0)

int main(){
    const std::string samples = "../samples/";
    const std::string refFile = samples + "STRUCTURED_GRID_block_four_fields_vectors_ascii.vtk";
    if (!fileExists(refFile)) {
        printf("SKIP (missing): %s\n", refFile.c_str());
        return 0;
    }

    printf("[load] %s\n", refFile.c_str());
    RenderMesh mesh = loadMeshFile(refFile);

    // -----------------------------------------------------------------------
    // 1. Verify the Divergent_Flow field is present and has 125 vectors
    // -----------------------------------------------------------------------
    size_t count = 0;
    const glm::vec3* divData = mesh.vectorFieldData("Divergent_Flow", count);
    CHECK(divData != nullptr, "Divergent_Flow field exists");
    CHECK(count == 125, "Divergent_Flow count == 125 (5*5*5)");

    if (!divData || count != 125) return failures;

    // -----------------------------------------------------------------------
    // 2. Verify field values at known grid points
    //    Grid is 5x5x5, points ordered x + y*5 + z*25.
    //    Divergent_Flow is a radial field: vectors point AWAY from center (2,2,2).
    // -----------------------------------------------------------------------
    auto idx = [](int x, int y, int z) { return x + y * 5 + z * 25; };
    auto dot3 = [](const glm::vec3& a, const glm::vec3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; };

    glm::vec3 center(2.0f, 2.0f, 2.0f);

    struct Expect {
        int ix, iy, iz;
        const char* label;
        glm::vec3 expectedDir; // unit direction the vector should generally point
        float minDot;          // minimum acceptable dot product with expectedDir
    };

    // At corners: vector points away from center
    // At (0,0,0): vector ≈ (-0.46,-0.46,-0.46), direction = (-1,-1,-1)/sqrt(3)
    // At (4,4,4): vector ≈ (+0.46,+0.46,+0.46), direction = (+1,+1,+1)/sqrt(3)
    // At (2,2,0): vector ≈ (0,0,-0.8), direction = (0,0,-1)
    // At (0,2,2): vector ≈ (-0.8,0,0), direction = (-1,0,0)
    const Expect expects[] = {
        {0, 0, 0, "(0,0,0) corner",  glm::vec3(-1,-1,-1) / std::sqrt(3.0f),  0.5f},
        {4, 4, 4, "(4,4,4) corner",  glm::vec3(+1,+1,+1) / std::sqrt(3.0f),  0.5f},
        {2, 2, 0, "(2,2,0) bottom",  glm::vec3( 0, 0,-1),                    0.7f},
        {0, 2, 2, "(0,2,2) left",    glm::vec3(-1, 0, 0),                    0.7f},
        {4, 2, 2, "(4,2,2) right",   glm::vec3(+1, 0, 0),                    0.7f},
        {2, 0, 2, "(2,0,2) back",    glm::vec3( 0,-1, 0),                    0.7f},
        {2, 4, 2, "(2,4,2) front",   glm::vec3( 0,+1, 0),                    0.7f},
    };

    for (const auto& ex : expects) {
        int i = idx(ex.ix, ex.iy, ex.iz);
        glm::vec3 v = divData[i];
        float m = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
        CHECK(m > 1e-6f, std::string("magnitude > 0 at ") + ex.label);
        if (m > 1e-6f) {
            glm::vec3 d = v / m;
            float dp = dot3(d, ex.expectedDir);
            CHECK(dp > ex.minDot, std::string("direction correct at ") + ex.label);
        }
    }

    // -----------------------------------------------------------------------
    // 3. Verify buildFrame orthonormality
    // -----------------------------------------------------------------------
    {
        glm::mat3 f = StreamlineSet::buildFrame(glm::vec3(1.0f, 2.0f, 3.0f));
        float eps = 1e-5f;
        CHECK(std::abs(dot3(f[0], f[1])) < eps, "buildFrame: n dot b == 0");
        CHECK(std::abs(dot3(f[0], f[2])) < eps, "buildFrame: n dot t == 0");
        CHECK(std::abs(dot3(f[1], f[2])) < eps, "buildFrame: b dot t == 0");
        CHECK(std::abs(std::sqrt(dot3(f[0],f[0])) - 1.0f) < eps, "buildFrame: |n| == 1");
        CHECK(std::abs(std::sqrt(dot3(f[1],f[1])) - 1.0f) < eps, "buildFrame: |b| == 1");
        CHECK(std::abs(std::sqrt(dot3(f[2],f[2])) - 1.0f) < eps, "buildFrame: |t| == 1");
        CHECK(f[2].x > 0.0f, "buildFrame: t aligns with +X input");

        // Edge case: near-Y axis
        glm::mat3 f2 = StreamlineSet::buildFrame(glm::vec3(0.0f, 1.0f, 0.0f));
        CHECK(std::abs(dot3(f2[0], f2[1])) < eps, "buildFrame(Y): n dot b == 0");
        CHECK(std::abs(dot3(f2[0], f2[2])) < eps, "buildFrame(Y): n dot t == 0");
        CHECK(std::abs(dot3(f2[1], f2[2])) < eps, "buildFrame(Y): b dot t == 0");
    }

    // -----------------------------------------------------------------------
    // 4. Verify generateArrowhead geometry
    //    - apex is ahead of pos in dir direction
    //    - base center is slightly behind pos
    //    - base cap normal faces backward (-dir)
    //    - cone face normals have positive component along dir
    // -----------------------------------------------------------------------
    {
        glm::vec3 pos(0.0f, 0.0f, 0.0f);
        glm::vec3 dir(1.0f, 0.0f, 0.0f);
        float height = 0.5f;
        float radius = 0.2f;
        std::vector<float> av = StreamlineSet::generateArrowhead(pos, dir, height, radius, 16, 1.0f);
        CHECK(!av.empty(), "generateArrowhead produces vertices");
         CHECK(av.size() % 7 == 0, "generateArrowhead vertex stride is 7");

        // Apex should be at approximately pos + dir * height = (0.5, 0, 0)
        // Find the vertex farthest along +X — that's the apex.
        float maxX = -1e30f;
        for (size_t i = 0; i < av.size(); i += 7) {
            maxX = std::max(maxX, av[i + 0]);
        }
        CHECK(maxX > 0.4f, "generateArrowhead apex is ahead of pos in +X");

        // Base center should be slightly behind pos
        // BaseCenter = pos - dir * (height * 0.15) = (-0.075, 0, 0)
        // The base ring vertices should cluster around x ≈ -0.075
        float baseXSum = 0.0f;
        int baseVerts = 0;
        for (size_t i = 0; i < av.size(); i += 7) {
            if (av[i + 0] < 0.0f) {
                baseXSum += av[i + 0];
                baseVerts++;
            }
        }
        CHECK(baseVerts > 0, "generateArrowhead has base vertices behind pos");
        if (baseVerts > 0) {
            float avgBaseX = baseXSum / baseVerts;
            CHECK(avgBaseX < -0.05f, "generateArrowhead base center is behind pos");
        }

        // Base cap normals should be -dir = (-1, 0, 0)
        // Cone face normals should have positive X component (slant angle)
        int capNormals = 0;
        int forwardNormals = 0;
        for (size_t i = 0; i < av.size(); i += 7) {
            float nx = av[i + 4];
            if (nx < -0.5f) capNormals++;
            if (nx > 0.3f) forwardNormals++;
        }
        CHECK(capNormals > 0, "generateArrowhead cap normals face backward");
        CHECK(forwardNormals > 0, "generateArrowhead cone normals face forward");
    }

    // -----------------------------------------------------------------------
    // 5. Verify evalFieldTrilinear with known fields
    // -----------------------------------------------------------------------
    {
        // Wind_Flow: uniform (1, 0, 0) everywhere
        size_t wfCount = 0;
        const glm::vec3* wfData = mesh.vectorFieldData("Wind_Flow", wfCount);
        CHECK(wfData != nullptr, "Wind_Flow field exists");
        CHECK(wfCount == 125, "Wind_Flow count == 125");

        if (wfData && wfCount == 125) {
            auto wfGrid = StreamlineSet::buildStructuredGridInfo(mesh, wfData, wfCount);
            CHECK(wfGrid.dimX == 5, "Wind_Flow grid dimX == 5");
            CHECK(wfGrid.dimY == 5, "Wind_Flow grid dimY == 5");
            CHECK(wfGrid.dimZ == 5, "Wind_Flow grid dimZ == 5");

            // Wind_Flow is V = (1, z*0.4, 0). At z=0 the field is exactly (1,0,0).
            glm::vec3 vZ0 = StreamlineSet::evalFieldTrilinear(wfGrid, glm::vec3(1.5f, 2.5f, 0.0f));
            CHECK(std::abs(vZ0.x - 1.0f) < 1e-4f, "Wind_Flow evalFieldTrilinear at z=0 X == 1");
            CHECK(std::abs(vZ0.y) < 1e-4f, "Wind_Flow evalFieldTrilinear at z=0 Y == 0");
            CHECK(std::abs(vZ0.z) < 1e-4f, "Wind_Flow evalFieldTrilinear at z=0 Z == 0");

            // At z=2 the Y component is 0.8 everywhere.
            glm::vec3 vZ2 = StreamlineSet::evalFieldTrilinear(wfGrid, glm::vec3(1.5f, 2.5f, 2.0f));
            CHECK(std::abs(vZ2.x - 1.0f) < 1e-4f, "Wind_Flow evalFieldTrilinear at z=2 X == 1");
            CHECK(std::abs(vZ2.y - 0.8f) < 1e-3f, "Wind_Flow evalFieldTrilinear at z=2 Y == 0.8");
            CHECK(std::abs(vZ2.z) < 1e-4f, "Wind_Flow evalFieldTrilinear at z=2 Z == 0");
        }

        // Divergent_Flow: direction should point away from center
        if (divData && count == 125) {
            auto divGrid = StreamlineSet::buildStructuredGridInfo(mesh, divData, count);
            CHECK(divGrid.dimX == 5, "Divergent_Flow grid dimX == 5");

            // At corner (0,0,0), field should point in -X,-Y,-Z direction
            glm::vec3 vCorner = StreamlineSet::evalFieldTrilinear(divGrid, glm::vec3(0.1f, 0.1f, 0.1f));
            CHECK(vCorner.x < -0.1f, "Divergent_Flow corner points -X");
            CHECK(vCorner.y < -0.1f, "Divergent_Flow corner points -Y");
            CHECK(vCorner.z < -0.1f, "Divergent_Flow corner points -Z");

            // At corner (4,4,4), field should point in +X,+Y,+Z direction
            glm::vec3 vFar = StreamlineSet::evalFieldTrilinear(divGrid, glm::vec3(3.9f, 3.9f, 3.9f));
            CHECK(vFar.x > 0.1f, "Divergent_Flow far corner points +X");
            CHECK(vFar.y > 0.1f, "Divergent_Flow far corner points +Y");
            CHECK(vFar.z > 0.1f, "Divergent_Flow far corner points +Z");

            // At bottom center (2,2,0), Z should be negative (pointing down/away)
            glm::vec3 vBottom = StreamlineSet::evalFieldTrilinear(divGrid, glm::vec3(2.0f, 2.0f, 0.1f));
            CHECK(vBottom.z < -0.1f, "Divergent_Flow bottom points -Z");
        }
    }

    // -----------------------------------------------------------------------
    // 6. Verify evalFieldNearest fallback
    // -----------------------------------------------------------------------
    {
        size_t wfCount = 0;
        const glm::vec3* wfData = mesh.vectorFieldData("Wind_Flow", wfCount);
        if (wfData && wfCount > 0) {
            glm::vec3 v = StreamlineSet::evalFieldNearest(mesh, glm::vec3(0.5f, 0.5f, 0.5f), wfData, wfCount, wfCount);
            CHECK(std::abs(v.x - 1.0f) < 1e-4f, "evalFieldNearest Wind_Flow X == 1");
            CHECK(std::abs(v.y) < 1e-4f, "evalFieldNearest Wind_Flow Y == 0");
            CHECK(std::abs(v.z) < 1e-4f, "evalFieldNearest Wind_Flow Z == 0");
        }
    }

    // -----------------------------------------------------------------------
    // 7. Verify compute(): draw count matches 7-float stride, every seed yields a path
    // -----------------------------------------------------------------------
    {
        StreamlineSet set;
        auto res = set.compute(mesh, 27, 0.05f, 200,
                               "Wind_Flow", "Volume", "Both",
                               0.5, 0.0, 10, 10,
                               false, 5, 0.02f, 0.01f, 0.3f);
        CHECK(res.seedCount > 0, "compute: seeds produced");
        CHECK(!res.verts.empty(), "compute: ribbon vertices produced");
        CHECK(res.lineCount == static_cast<int>(res.verts.size() / 7),
              "compute: lineCount matches 7-float vertex stride");
        CHECK(static_cast<int>(res.paths.size()) == res.seedCount,
              "compute: every surviving seed yields a streamline");
        bool allLongEnough = true;
        for (const auto& p : res.paths)
            if (p.points.size() < 3) { allLongEnough = false; break; }
        CHECK(allLongEnough, "compute: every path has >= 3 points");
    }
    // -----------------------------------------------------------------------
    // Summary
    // -----------------------------------------------------------------------
    if (failures == 0) {
        printf("\nALL STREAMLINE DIRECTION CHECKS PASSED\n");
    } else {
        printf("\n%d CHECK(S) FAILED in %zu file(s):\n", failures, failedFiles.size());
        for (const auto& f : failedFiles) printf("  - %s\n", f.c_str());
    }
    return failures;
}
