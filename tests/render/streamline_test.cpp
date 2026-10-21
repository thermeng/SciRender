// Standalone verification of arrow/cone direction on streamlines.
// Uses the Divergent_Flow field from the structured-grid VTK fixture to
// validate that generated geometry points along the local vector direction.
// Compiles the REAL sources (no Qt/GL, no GL context).
#include "render/streamlines/StreamlineSet.h"
#include "core/mesh_loader.h"
#include "doctest/doctest.h"
#include "support/test_support.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

const char* const kReferenceSample = "STRUCTURED_GRID_block_four_fields_vectors_ascii.vtk";

const RenderMesh& referenceMesh() {
    static RenderMesh mesh = [] {
        test_support::requireSample(kReferenceSample);
        return loadMeshFile(test_support::samplePath(kReferenceSample).string());
    }();
    return mesh;
}

float dot3(const glm::vec3& a, const glm::vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

} // namespace

TEST_SUITE("render") {

// ---------------------------------------------------------------------------
// 1. Verify the Divergent_Flow field is present and has 125 vectors
// ---------------------------------------------------------------------------
TEST_CASE("render.streamline.field_present") {
    const RenderMesh& mesh = referenceMesh();
    size_t count = 0;
    const glm::vec3* divData = mesh.vectorFieldData("Divergent_Flow", count);
    REQUIRE_MESSAGE(divData != nullptr, "Divergent_Flow field exists");
    CHECK_MESSAGE(count == 125, "Divergent_Flow count == 125 (5*5*5), got " << count);
}

// ---------------------------------------------------------------------------
// 2. Verify field values at known grid points
//    Grid is 5x5x5, points ordered x + y*5 + z*25.
//    Divergent_Flow is a radial field: vectors point AWAY from center (2,2,2).
// ---------------------------------------------------------------------------
TEST_CASE("render.streamline.field_directions") {
    const RenderMesh& mesh = referenceMesh();
    size_t count = 0;
    const glm::vec3* divData = mesh.vectorFieldData("Divergent_Flow", count);
    const bool divUsable = (divData != nullptr) && (count == 125);
    REQUIRE_MESSAGE(divUsable, "Divergent_Flow field must be usable");

    auto idx = [](int x, int y, int z) { return x + y * 5 + z * 25; };

    struct Expect {
        int ix, iy, iz;
        const char* label;
        glm::vec3 expectedDir; // unit direction the vector should generally point
        float minDot;          // minimum acceptable dot product with expectedDir
    };

    // At corners: vector points away from center
    // At (0,0,0): vector ~ (-0.46,-0.46,-0.46), direction = (-1,-1,-1)/sqrt(3)
    // At (4,4,4): vector ~ (+0.46,+0.46,+0.46), direction = (+1,+1,+1)/sqrt(3)
    // At (2,2,0): vector ~ (0,0,-0.8), direction = (0,0,-1)
    // At (0,2,2): vector ~ (-0.8,0,0), direction = (-1,0,0)
    const Expect expects[] = {
        {0, 0, 0, "(0,0,0) corner", glm::vec3(-1, -1, -1) / std::sqrt(3.0f), 0.5f},
        {4, 4, 4, "(4,4,4) corner", glm::vec3(+1, +1, +1) / std::sqrt(3.0f), 0.5f},
        {2, 2, 0, "(2,2,0) bottom", glm::vec3(0, 0, -1), 0.7f},
        {0, 2, 2, "(0,2,2) left", glm::vec3(-1, 0, 0), 0.7f},
        {4, 2, 2, "(4,2,2) right", glm::vec3(+1, 0, 0), 0.7f},
        {2, 0, 2, "(2,0,2) back", glm::vec3(0, -1, 0), 0.7f},
        {2, 4, 2, "(2,4,2) front", glm::vec3(0, +1, 0), 0.7f},
    };

    for (const auto& ex : expects) {
        CAPTURE(ex.label);
        const int i = idx(ex.ix, ex.iy, ex.iz);
        glm::vec3 v = divData[i];
        float m = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        REQUIRE_MESSAGE(m > 1e-6f, "magnitude > 0");
        glm::vec3 d = v / m;
        float dp = dot3(d, ex.expectedDir);
        CHECK_MESSAGE(dp > ex.minDot, "direction correct (dot=" << dp << ")");
    }
}

// ---------------------------------------------------------------------------
// 3. Verify buildFrame orthonormality
// ---------------------------------------------------------------------------
TEST_CASE("render.streamline.build_frame") {
    glm::mat3 f = StreamlineSet::buildFrame(glm::vec3(1.0f, 2.0f, 3.0f));
    float eps = 1e-5f;
    CHECK_MESSAGE(std::abs(dot3(f[0], f[1])) < eps, "buildFrame: n dot b == 0");
    CHECK_MESSAGE(std::abs(dot3(f[0], f[2])) < eps, "buildFrame: n dot t == 0");
    CHECK_MESSAGE(std::abs(dot3(f[1], f[2])) < eps, "buildFrame: b dot t == 0");
    CHECK_MESSAGE(std::abs(std::sqrt(dot3(f[0], f[0])) - 1.0f) < eps, "buildFrame: |n| == 1");
    CHECK_MESSAGE(std::abs(std::sqrt(dot3(f[1], f[1])) - 1.0f) < eps, "buildFrame: |b| == 1");
    CHECK_MESSAGE(std::abs(std::sqrt(dot3(f[2], f[2])) - 1.0f) < eps, "buildFrame: |t| == 1");
    CHECK_MESSAGE(f[2].x > 0.0f, "buildFrame: t aligns with +X input");

    // Edge case: near-Y axis
    glm::mat3 f2 = StreamlineSet::buildFrame(glm::vec3(0.0f, 1.0f, 0.0f));
    CHECK_MESSAGE(std::abs(dot3(f2[0], f2[1])) < eps, "buildFrame(Y): n dot b == 0");
    CHECK_MESSAGE(std::abs(dot3(f2[0], f2[2])) < eps, "buildFrame(Y): n dot t == 0");
    CHECK_MESSAGE(std::abs(dot3(f2[1], f2[2])) < eps, "buildFrame(Y): b dot t == 0");
}

// ---------------------------------------------------------------------------
// 4. Verify generateArrowhead geometry
//    - apex is ahead of pos in dir direction
//    - base center is slightly behind pos
//    - base cap normal faces backward (-dir)
//    - cone face normals have positive component along dir
// ---------------------------------------------------------------------------
TEST_CASE("render.streamline.arrowhead_geometry") {
    glm::vec3 pos(0.0f, 0.0f, 0.0f);
    glm::vec3 dir(1.0f, 0.0f, 0.0f);
    const glm::vec3 comp(0.5f, 0.25f, 0.75f); // field vector carried per vertex
    float height = 0.5f;
    float radius = 0.2f;
    // Vertex layout: position(3) | magnitude(1) | field vector(3) | normal(3).
    constexpr int kStride = 10;
    std::vector<float> av =
        StreamlineSet::generateArrowhead(pos, dir, height, radius, 16, 1.0f, comp);
    REQUIRE_MESSAGE(!av.empty(), "generateArrowhead produces vertices");
    CHECK_MESSAGE(av.size() % kStride == 0, "generateArrowhead vertex stride is 10");

    // Apex should be at approximately pos + dir * height = (0.5, 0, 0)
    // Find the vertex farthest along +X - that's the apex.
    float maxX = -1e30f;
    for (size_t i = 0; i < av.size(); i += kStride)
        maxX = std::max(maxX, av[i + 0]);
    CHECK_MESSAGE(maxX > 0.4f, "generateArrowhead apex is ahead of pos in +X");

    // The field vector is stored per vertex.
    bool compOk = true;
    for (size_t i = 0; i < av.size(); i += kStride) {
        if (std::abs(av[i + 4] - comp.x) > 1e-6f || std::abs(av[i + 5] - comp.y) > 1e-6f ||
            std::abs(av[i + 6] - comp.z) > 1e-6f)
            compOk = false;
    }
    CHECK_MESSAGE(compOk, "generateArrowhead stores the field vector per vertex");

    // Base center should be slightly behind pos
    // BaseCenter = pos - dir * (height * 0.15) = (-0.075, 0, 0)
    // The base ring vertices should cluster around x ~ -0.075
    float baseXSum = 0.0f;
    int baseVerts = 0;
    for (size_t i = 0; i < av.size(); i += kStride) {
        if (av[i + 0] < 0.0f) {
            baseXSum += av[i + 0];
            baseVerts++;
        }
    }
    REQUIRE_MESSAGE(baseVerts > 0, "generateArrowhead has base vertices behind pos");
    float avgBaseX = baseXSum / baseVerts;
    CHECK_MESSAGE(avgBaseX < -0.05f, "generateArrowhead base center is behind pos");

    // Base cap normals should be -dir = (-1, 0, 0)
    // Cone face normals should have positive X component (slant angle)
    int capNormals = 0;
    int forwardNormals = 0;
    for (size_t i = 0; i < av.size(); i += kStride) {
        const float nx = av[i + 7];
        if (nx < -0.5f)
            capNormals++;
        if (nx > 0.3f)
            forwardNormals++;
    }
    CHECK_MESSAGE(capNormals > 0, "generateArrowhead cap normals face backward");
    CHECK_MESSAGE(forwardNormals > 0, "generateArrowhead cone normals face forward");
}

// ---------------------------------------------------------------------------
// 5. Verify evalFieldTrilinear with known fields
// ---------------------------------------------------------------------------
TEST_CASE("render.streamline.trilinear_sampling") {
    const RenderMesh& mesh = referenceMesh();

    // Wind_Flow: uniform (1, 0, 0) everywhere
    size_t wfCount = 0;
    const glm::vec3* wfData = mesh.vectorFieldData("Wind_Flow", wfCount);
    REQUIRE_MESSAGE(wfData != nullptr, "Wind_Flow field exists");
    REQUIRE_MESSAGE(wfCount == 125, "Wind_Flow count == 125, got " << wfCount);

    auto wfGrid = StreamlineSet::buildStructuredGridInfo(mesh, wfData, wfCount);
    CHECK_MESSAGE(wfGrid.dimX == 5, "Wind_Flow grid dimX == 5");
    CHECK_MESSAGE(wfGrid.dimY == 5, "Wind_Flow grid dimY == 5");
    CHECK_MESSAGE(wfGrid.dimZ == 5, "Wind_Flow grid dimZ == 5");

    // Wind_Flow is V = (1, z*0.4, 0). At z=0 the field is exactly (1,0,0).
    glm::vec3 vZ0 = StreamlineSet::evalFieldTrilinear(wfGrid, glm::vec3(1.5f, 2.5f, 0.0f));
    CHECK_MESSAGE(std::abs(vZ0.x - 1.0f) < 1e-4f, "Wind_Flow evalFieldTrilinear at z=0 X == 1");
    CHECK_MESSAGE(std::abs(vZ0.y) < 1e-4f, "Wind_Flow evalFieldTrilinear at z=0 Y == 0");
    CHECK_MESSAGE(std::abs(vZ0.z) < 1e-4f, "Wind_Flow evalFieldTrilinear at z=0 Z == 0");

    // At z=2 the Y component is 0.8 everywhere.
    glm::vec3 vZ2 = StreamlineSet::evalFieldTrilinear(wfGrid, glm::vec3(1.5f, 2.5f, 2.0f));
    CHECK_MESSAGE(std::abs(vZ2.x - 1.0f) < 1e-4f, "Wind_Flow evalFieldTrilinear at z=2 X == 1");
    CHECK_MESSAGE(std::abs(vZ2.y - 0.8f) < 1e-3f, "Wind_Flow evalFieldTrilinear at z=2 Y == 0.8");
    CHECK_MESSAGE(std::abs(vZ2.z) < 1e-4f, "Wind_Flow evalFieldTrilinear at z=2 Z == 0");

    // Divergent_Flow: direction should point away from center
    size_t divCount = 0;
    const glm::vec3* divData = mesh.vectorFieldData("Divergent_Flow", divCount);
    const bool divUsable = (divData != nullptr) && (divCount == 125);
    REQUIRE_MESSAGE(divUsable, "Divergent_Flow field must be usable");
    auto divGrid = StreamlineSet::buildStructuredGridInfo(mesh, divData, divCount);
    CHECK_MESSAGE(divGrid.dimX == 5, "Divergent_Flow grid dimX == 5");

    // At corner (0,0,0), field should point in -X,-Y,-Z direction
    glm::vec3 vCorner = StreamlineSet::evalFieldTrilinear(divGrid, glm::vec3(0.1f, 0.1f, 0.1f));
    CHECK_MESSAGE(vCorner.x < -0.1f, "Divergent_Flow corner points -X");
    CHECK_MESSAGE(vCorner.y < -0.1f, "Divergent_Flow corner points -Y");
    CHECK_MESSAGE(vCorner.z < -0.1f, "Divergent_Flow corner points -Z");

    // At corner (4,4,4), field should point in +X,+Y,+Z direction
    glm::vec3 vFar = StreamlineSet::evalFieldTrilinear(divGrid, glm::vec3(3.9f, 3.9f, 3.9f));
    CHECK_MESSAGE(vFar.x > 0.1f, "Divergent_Flow far corner points +X");
    CHECK_MESSAGE(vFar.y > 0.1f, "Divergent_Flow far corner points +Y");
    CHECK_MESSAGE(vFar.z > 0.1f, "Divergent_Flow far corner points +Z");

    // At bottom center (2,2,0), Z should be negative (pointing down/away)
    glm::vec3 vBottom = StreamlineSet::evalFieldTrilinear(divGrid, glm::vec3(2.0f, 2.0f, 0.1f));
    CHECK_MESSAGE(vBottom.z < -0.1f, "Divergent_Flow bottom points -Z");
}

// ---------------------------------------------------------------------------
// 6. Verify evalFieldNearest fallback
// ---------------------------------------------------------------------------
TEST_CASE("render.streamline.nearest_fallback") {
    const RenderMesh& mesh = referenceMesh();
    size_t wfCount = 0;
    const glm::vec3* wfData = mesh.vectorFieldData("Wind_Flow", wfCount);
    const bool wfUsable = (wfData != nullptr) && (wfCount > 0);
    REQUIRE_MESSAGE(wfUsable, "Wind_Flow field must be usable");
    glm::vec3 v = StreamlineSet::evalFieldNearest(mesh, glm::vec3(0.5f, 0.5f, 0.5f), wfData, wfCount,
                                                  wfCount);
    CHECK_MESSAGE(std::abs(v.x - 1.0f) < 1e-4f, "evalFieldNearest Wind_Flow X == 1");
    CHECK_MESSAGE(std::abs(v.y) < 1e-4f, "evalFieldNearest Wind_Flow Y == 0");
    CHECK_MESSAGE(std::abs(v.z) < 1e-4f, "evalFieldNearest Wind_Flow Z == 0");
}

// ---------------------------------------------------------------------------
// 7. Verify compute(): draw count matches 7-float stride, every seed yields a path
// ---------------------------------------------------------------------------
TEST_CASE("render.streamline.compute") {
    const RenderMesh& mesh = referenceMesh();
    StreamlineSet set;
    auto res = set.compute(mesh, 27, 0.05f, 200, "Wind_Flow", "Volume", "Both", 0.5, 0.0, 10, 10,
                           false, 0.15f, 0.02f, 0.01f, 0.3f);
    CHECK_MESSAGE(res.seedCount > 0, "compute: seeds produced");
    REQUIRE_MESSAGE(!res.verts.empty(), "compute: ribbon vertices produced");
    CHECK_MESSAGE(res.lineCount == static_cast<int>(res.verts.size() / 10),
                  "compute: lineCount matches the 10-float vertex stride");
    CHECK_MESSAGE(static_cast<int>(res.paths.size()) == res.seedCount,
                  "compute: every surviving seed yields a streamline");
    bool allLongEnough = true;
    for (const auto& p : res.paths)
        if (p.points.size() < 3) {
            allLongEnough = false;
            break;
        }
    CHECK_MESSAGE(allLongEnough, "compute: every path has >= 3 points");
}

// ---------------------------------------------------------------------------
// 8. computeArrowPlacement: logical arrow distribution invariants
// ---------------------------------------------------------------------------
TEST_CASE("render.streamline.arrow_placement") {
    const float extent = 2.0f;

    // Even spacing with symmetric margins.
    {
        auto pos = StreamlineSet::computeArrowPlacement(10.0f, extent, 0.15f, 0.05f, 0.0f);
        REQUIRE_MESSAGE(!pos.empty(), "placement: arrows produced on a long path");
        bool even = true;
        for (size_t i = 1; i < pos.size(); ++i) {
            if (std::abs((pos[i] - pos[i - 1]) - (pos[1] - pos[0])) > 1e-4f) {
                even = false;
                break;
            }
        }
        CHECK_MESSAGE(even, "placement: arrows evenly spaced");
        const float arrowHeight = 0.05f * extent;
        CHECK_MESSAGE(pos.front() > arrowHeight - 1e-4f, "placement: first arrow clear of the tip");
    }

    // Anti-overlap: spacing never below 2x arrow height.
    {
        auto pos = StreamlineSet::computeArrowPlacement(10.0f, extent, 0.001f, 0.05f, 0.0f);
        bool minGapOk = true;
        for (size_t i = 1; i < pos.size(); ++i)
            if (pos[i] - pos[i - 1] < 2.0f * 0.05f * extent - 1e-4f) {
                minGapOk = false;
                break;
            }
        CHECK_MESSAGE(minGapOk, "placement: anti-overlap floor respected");
    }

    // Short path: no arrows when the arrow would dominate.
    {
        auto pos = StreamlineSet::computeArrowPlacement(0.1f, extent, 0.15f, 0.05f, 0.0f);
        CHECK_MESSAGE(pos.empty(), "placement: short path skipped");
    }

    // Single arrow centered when usable < target spacing.
    {
        auto pos = StreamlineSet::computeArrowPlacement(2.6f, extent, 0.9f, 0.05f, 0.0f);
        REQUIRE_MESSAGE(pos.size() == 1, "placement: single arrow when only one fits, got "
                                              << pos.size());
        CHECK_MESSAGE(std::abs(pos[0] - 1.3f) < 1e-3f, "placement: single arrow centered");
    }

    // Taper pushes arrows further from the tips.
    {
        auto noTaper = StreamlineSet::computeArrowPlacement(10.0f, extent, 0.15f, 0.05f, 0.0f);
        auto tapered = StreamlineSet::computeArrowPlacement(10.0f, extent, 0.15f, 0.05f, 0.5f);
        const bool bothProduced = !tapered.empty() && !noTaper.empty();
        REQUIRE_MESSAGE(bothProduced, "placement: both taper variants produce arrows");
        CHECK_MESSAGE(tapered.front() > noTaper.front(), "placement: taper increases tip margin");
    }

    // Monotonicity: smaller fraction never yields fewer arrows.
    {
        auto coarse = StreamlineSet::computeArrowPlacement(10.0f, extent, 0.45f, 0.05f, 0.0f);
        auto fine = StreamlineSet::computeArrowPlacement(10.0f, extent, 0.05f, 0.05f, 0.0f);
        CHECK_MESSAGE(fine.size() >= coarse.size(), "placement: smaller fraction yields >= arrows");
    }

    // Degenerate inputs.
    {
        CHECK_MESSAGE(StreamlineSet::computeArrowPlacement(0.0f, extent, 0.15f, 0.05f, 0.0f).empty(),
                      "placement: zero-length path has no arrows");
        CHECK_MESSAGE(StreamlineSet::computeArrowPlacement(10.0f, extent, 0.0f, 0.05f, 0.0f).empty(),
                      "placement: zero fraction disables arrows");
    }
}

// ---------------------------------------------------------------------------
// 9. Particle advance invariants (updateParticles via public API)
// ---------------------------------------------------------------------------
TEST_CASE("render.streamline.particle_advance") {
    const RenderMesh& mesh = referenceMesh();
    StreamlineSet set;
    auto res = set.compute(mesh, 27, 0.05f, 200, "Wind_Flow", "Volume", "Both", 0.5, 0.0, 10, 10,
                           false, 0.15f, 0.02f, 0.01f, 0.3f);
    REQUIRE_MESSAGE(!res.paths.empty(), "compute must produce paths for the particle test");

    // CPU-only: move paths in directly (uploadGL needs a GL context,
    // which this suite does not have).
    set.paths = std::move(res.paths);
    set.initParticles(10);
    CHECK_MESSAGE(static_cast<int>(set.paths.size()) > 0, "particles: paths available after uploadGL");

    // Sample t before/after a run of fixed steps; every t must stay in [0,1).
    std::vector<float> before;
    for (const auto& p : set.particles)
        before.push_back(p.t);
    bool rangeOk = true;
    for (int step = 0; step < 100; ++step) {
        set.updateParticles(0.016f, 1.0f);
        for (const auto& p : set.particles) {
            if (!(p.t >= 0.0f && p.t < 1.0f)) {
                rangeOk = false;
                break;
            }
        }
        if (!rangeOk)
            break;
    }
    CHECK_MESSAGE(rangeOk, "particles: t stays in [0,1) across 100 advances");
    bool anyMoved = false;
    for (size_t i = 0; i < set.particles.size(); ++i)
        if (std::abs(set.particles[i].t - before[i]) > 1e-6f) {
            anyMoved = true;
            break;
        }
    CHECK_MESSAGE(anyMoved, "particles: at least one particle advanced");
}

} // TEST_SUITE("render")
