// tests/post_fields_test.cpp
// Headless checks for Cartesian detection + derived vector calculus fields.
// Compiles the REAL post_fields sources (no Qt/GL). Run via tests/run_tests.{bat,sh}
#include "core/post_fields.h"
#include <cstdio>
#include <cmath>
#include <functional>
#include <string>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg) do { ++checks; if(!(cond)){ \
    printf("  [FAIL] %s\n", msg); ++failures; } } while(0)

using VFn = std::function<glm::vec3(float,float,float)>;
using PFn = std::function<float(float,float,float)>;

// Cartesian grid mesh with analytic velocity/pressure sampled at nodes.
// xs/ys/zs give (possibly non-uniform) axis coordinates.
static RenderMesh makeFlowGrid(const std::vector<float>& xs,
                               const std::vector<float>& ys,
                               const std::vector<float>& zs,
                               VFn vel, PFn pres,
                               const std::string& vName = "velocity",
                               const std::string& pName = "pressure") {
    RenderMesh m;
    const int dX = (int)xs.size(), dY = (int)ys.size(), dZ = (int)zs.size();
    m.gridDimX = dX; m.gridDimY = dY; m.gridDimZ = dZ;
    m.datasetType = "IMAGEDATA";
    const size_t N = (size_t)dX * dY * dZ;
    m.vertices.resize(N * 3);
    m.pointVectorsData.resize(N);
    m.pointVectorCount = N;
    m.pointVectorOffset[vName] = 0;
    m.availableVectorNames.push_back(vName);
    m.vectorName = vName;
    m.attributes = DatasetAttributes{};
    std::vector<float>& ps = m.attributes->pointScalars[pName];
    ps.resize(N);
    for (int k = 0; k < dZ; ++k)
        for (int j = 0; j < dY; ++j)
            for (int i = 0; i < dX; ++i) {
                const size_t n = (size_t)i + (size_t)j * (size_t)dX + (size_t)k * (size_t)dX * (size_t)dY;
                const float x = xs[(size_t)i], y = ys[(size_t)j], z = zs[(size_t)k];
                m.vertices[n*3+0] = x; m.vertices[n*3+1] = y; m.vertices[n*3+2] = z;
                const glm::vec3 v = vel(x, y, z);
                m.pointVectorsData[n] = v;
                ps[n] = pres(x, y, z);
            }
    m.availableScalarNames.push_back(pName);
    m.scalarName = pName;
    m.scalars = ps;
    m.bounds.minX = xs.front(); m.bounds.maxX = xs.back();
    m.bounds.minY = ys.front(); m.bounds.maxY = ys.back();
    m.bounds.minZ = zs.front(); m.bounds.maxZ = zs.back();
    return m;
}

static std::vector<float> linspace(float a, float b, int n) {
    std::vector<float> v;
    for (int i = 0; i < n; ++i) v.push_back(a + (b - a) * i / (n - 1));
    return v;
}

static float maxAbs(const std::vector<float>& v) {
    float m = 0;
    for (float f : v) m = std::max(m, std::abs(f));
    return m;
}

static float maxVecAbs(const std::vector<glm::vec3>& v) {
    float m = 0;
    for (auto& t : v) m = std::max({m, std::abs(t.x), std::abs(t.y), std::abs(t.z)});
    return m;
}

static void testUniformFlow() {
    printf("[TEST] uniform flow -> zero vorticity/Q/helicity, exact grad-p\n");
    auto g = linspace(0, 4, 5);
    RenderMesh m = makeFlowGrid(g, g, g,
        [](float, float, float) { return glm::vec3(2.0f, -1.0f, 0.5f); },
        [](float x, float y, float z) { return 3.0f*x - 2.0f*y + 0.5f*z + 1.0f; });
    postfields::CartesianGrid grid;
    CHECK(postfields::extractCartesianGrid(m, grid), "uniform: cartesian");
    postfields::PointVectorField vf;
    CHECK(postfields::extractPointVectors(m, grid, "velocity", vf), "uniform: velocity fetch");
    postfields::PointScalarField pf;
    CHECK(postfields::extractPointScalars(m, grid, "pressure", pf), "uniform: pressure fetch");

    postfields::PostFieldsRequest req;
    req.velocity = vf.data; req.pressure = pf.data;
    auto r = postfields::computePostFields(grid, req);
    CHECK(r.ok, "uniform: ok");
    CHECK(maxVecAbs(r.vorticity) < 1e-4f, "uniform: vorticity ~ 0");
    CHECK(maxAbs(r.qCriterion) < 1e-4f, "uniform: Q ~ 0");
    CHECK(maxAbs(r.helicity) < 1e-4f, "uniform: helicity ~ 0");
    bool gradOk = true;
    for (auto& t : r.gradP) {
        if (std::abs(t.x - 3.0f) > 1e-4f || std::abs(t.y + 2.0f) > 1e-4f || std::abs(t.z - 0.5f) > 1e-4f)
            gradOk = false;
    }
    CHECK(gradOk, "uniform: grad-p exact (3,-2,0.5)");
}

static void testSolidRotation() {
    printf("[TEST] solid-body rotation -> omega=(0,0,2), Q=1\n");
    auto g = linspace(-2, 2, 7);
    RenderMesh m = makeFlowGrid(g, g, g,
        [](float x, float y, float) { return glm::vec3(-y, x, 0.0f); },
        [](float, float, float) { return 0.0f; });
    postfields::CartesianGrid grid;
    CHECK(postfields::extractCartesianGrid(m, grid), "rotation: cartesian");
    postfields::PointVectorField vf;
    CHECK(postfields::extractPointVectors(m, grid, "velocity", vf), "rotation: velocity fetch");
    postfields::PostFieldsRequest req;
    req.velocity = vf.data; req.pressure = nullptr;
    req.wantGradP = false;
    auto r = postfields::computePostFields(grid, req);
    CHECK(r.ok, "rotation: ok");
    bool omOk = true, qOk = true;
    for (auto& t : r.vorticity) {
        if (std::abs(t.x) > 1e-3f || std::abs(t.y) > 1e-3f || std::abs(t.z - 2.0f) > 1e-3f) omOk = false;
    }
    for (float q : r.qCriterion) {
        if (std::abs(q - 1.0f) > 1e-3f) qOk = false;
    }
    CHECK(omOk, "rotation: vorticity == (0,0,2)");
    CHECK(qOk, "rotation: Q == 1");
    CHECK(maxAbs(r.helicity) < 1e-3f, "rotation: helicity ~ 0 (planar)");
}

static void testPoiseuille() {
    printf("[TEST] Poiseuille profile -> omega_z = 2y (interior exact)\n");
    auto g = linspace(-1, 1, 9);
    RenderMesh m = makeFlowGrid(g, g, linspace(0, 1, 4),
        [](float, float y, float) { return glm::vec3(1.0f - y*y, 0.0f, 0.0f); },
        [](float, float, float) { return 0.0f; });
    postfields::CartesianGrid grid;
    CHECK(postfields::extractCartesianGrid(m, grid), "poiseuille: cartesian");
    postfields::PointVectorField vf;
    CHECK(postfields::extractPointVectors(m, grid, "velocity", vf), "poiseuille: fetch");
    postfields::PostFieldsRequest req;
    req.velocity = vf.data; req.pressure = nullptr;
    req.wantHelicity = false; req.wantQ = false; req.wantGradP = false;
    auto r = postfields::computePostFields(grid, req);
    CHECK(r.ok && r.hasVorticity && !r.hasHelicity, "poiseuille: subset flags");
    // Central differences of a quadratic are exact — check interior nodes only
    // (one-sided boundary stencils are first-order).
    bool ok = true;
    for (int k = 0; k < grid.dZ; ++k)
        for (int j = 1; j < grid.dY - 1; ++j)
            for (int i = 1; i < grid.dX - 1; ++i) {
                const size_t n = grid.index(i, j, k);
                const float wz = r.vorticity[n].z;
                if (std::abs(r.vorticity[n].x) > 1e-4f || std::abs(r.vorticity[n].y) > 1e-4f) ok = false;
                if (std::abs(wz - 2.0f * grid.y[(size_t)j]) > 1e-3f) ok = false;
            }
    CHECK(ok, "poiseuille: interior omega_z == 2y");
}

static void testNonUniformRectilinear() {
    printf("[TEST] non-uniform rectilinear -> linear fields exact\n");
    const std::vector<float> xs = {0.0f, 0.5f, 2.0f, 3.0f};
    const std::vector<float> ys = {-1.0f, 0.25f, 1.0f};
    const std::vector<float> zs = {0.0f, 4.0f, 5.0f, 9.0f, 10.0f};
    RenderMesh m = makeFlowGrid(xs, ys, zs,
        [](float x, float y, float z) { return glm::vec3(x + 2*y, y - z, 3*x + z); },
        [](float x, float y, float z) { return x - 2*y + 3*z; });
    postfields::CartesianGrid grid;
    CHECK(postfields::extractCartesianGrid(m, grid), "rectilinear: cartesian");
    CHECK((int)grid.x.size() == 4 && (int)grid.z.size() == 5, "rectilinear: axis sizes");
    postfields::PointVectorField vf; postfields::PointScalarField pf;
    CHECK(postfields::extractPointVectors(m, grid, "velocity", vf), "rectilinear: velocity");
    CHECK(postfields::extractPointScalars(m, grid, "pressure", pf), "rectilinear: pressure");
    postfields::PostFieldsRequest req;
    req.velocity = vf.data; req.pressure = pf.data;
    auto r = postfields::computePostFields(grid, req);
    CHECK(r.ok, "rectilinear: ok");
    // ω = (dw/dy−dv/dz, du/dz−dw/dx, dv/dx−du/dy) = (0−(−1), 0−3, 0−2) = (1,−3,−2)
    bool omOk = true;
    for (auto& t : r.vorticity) {
        if (std::abs(t.x - 1.0f) > 1e-3f || std::abs(t.y + 3.0f) > 1e-3f || std::abs(t.z + 2.0f) > 1e-3f)
            omOk = false;
    }
    CHECK(omOk, "rectilinear: vorticity == (1,-3,-2)");
    bool gOk = true;
    for (auto& t : r.gradP) {
        if (std::abs(t.x - 1.0f) > 1e-4f || std::abs(t.y + 2.0f) > 1e-4f || std::abs(t.z - 3.0f) > 1e-4f)
            gOk = false;
    }
    CHECK(gOk, "rectilinear: grad-p == (1,-2,3)");
}

static void testRejections() {
    printf("[TEST] curvilinear / unstructured / 2D / missing-field rejection\n");
    auto g = linspace(0, 2, 3);
    // Curvilinear: sinusoidal z displacement breaks axis alignment.
    RenderMesh curv = makeFlowGrid(g, g, g,
        [](float, float, float) { return glm::vec3(1, 0, 0); },
        [](float, float, float) { return 0.0f; });
    for (size_t n = 0; n < curv.vertices.size() / 3; ++n)
        curv.vertices[n*3+2] += 0.1f * std::sin(curv.vertices[n*3]);
    postfields::CartesianGrid grid;
    CHECK(!postfields::extractCartesianGrid(curv, grid), "curvilinear rejected");

    // Unstructured: no grid dims.
    RenderMesh ug = curv;
    ug.gridDimX = ug.gridDimY = ug.gridDimZ = 0;
    CHECK(!postfields::extractCartesianGrid(ug, grid), "unstructured rejected");

    // 2D slab — now supported for VTI 2D CFD (vorticity out-of-plane).
    RenderMesh slab = makeFlowGrid(g, g, {0.0f},
        [](float, float, float) { return glm::vec3(1, 0, 0); },
        [](float, float, float) { return 0.0f; });
    CHECK(postfields::extractCartesianGrid(slab, grid), "2D slab accepted");
    // 2D vorticity should be ~0 for uniform flow, Q ~0
    {
        postfields::PointVectorField vf2; postfields::PointScalarField pf2;
        CHECK(postfields::extractPointVectors(slab, grid, "velocity", vf2), "2D velocity fetch");
        CHECK(postfields::extractPointScalars(slab, grid, "pressure", pf2), "2D pressure fetch");
        postfields::PostFieldsRequest req2; req2.velocity=vf2.data; req2.pressure=pf2.data;
        auto r2 = postfields::computePostFields(grid, req2);
        CHECK(r2.ok, "2D post fields ok");
        float maxV = 0; for(auto &v: r2.vorticity) maxV = std::max({maxV, std::abs(v.x), std::abs(v.y), std::abs(v.z)});
        CHECK(maxV < 1e-3f, "2D vorticity ~0");
    }

    // Missing fields / size mismatch.
    RenderMesh m = makeFlowGrid(g, g, g,
        [](float, float, float) { return glm::vec3(1, 0, 0); },
        [](float, float, float) { return 0.0f; });
    CHECK(postfields::extractCartesianGrid(m, grid), "fixture cartesian");
    postfields::PointVectorField vf;
    CHECK(!postfields::extractPointVectors(m, grid, "nope", vf), "missing velocity rejected");
    postfields::PointScalarField pf;
    CHECK(!postfields::extractPointScalars(m, grid, "nope", pf), "missing pressure rejected");
    postfields::PostFieldsRequest bad{};
    auto rb = postfields::computePostFields(grid, bad);
    CHECK(!rb.ok, "empty request rejected");

    // Naming.
    CHECK(postfields::vorticityName("U") == "U_vorticity", "vorticity name");
    CHECK(postfields::helicityName("U") == "U_helicity", "helicity name");
    CHECK(postfields::qCriterionName("U") == "U_Q", "Q name");
    CHECK(postfields::gradPName("p") == "p_grad", "grad-p name");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testUniformFlow();
    testSolidRotation();
    testPoiseuille();
    testNonUniformRectilinear();
    testRejections();
    printf("\n");
    if (failures == 0) {
        printf("ALL POST-FIELDS CHECKS PASSED (%d checks)\n", checks);
    } else {
        printf("%d/%d CHECK(S) FAILED\n", failures, checks);
    }
    return failures;
}
