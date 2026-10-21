// Headless checks for Cartesian detection + derived vector calculus fields.
// Links the real post_fields sources through CoreLib (no Qt, no GL).
#include "core/post_fields.h"
#include "doctest/doctest.h"
#include "support/mesh_fixtures.h"

#include <cmath>

using namespace test_support;

TEST_SUITE("core") {

TEST_CASE("core.postfields.uniform_flow") {
    auto g = linspace(0, 4, 5);
    RenderMesh m = makeFlowGrid(g, g, g,
        [](float, float, float) { return glm::vec3(2.0f, -1.0f, 0.5f); },
        [](float x, float y, float z) { return 3.0f * x - 2.0f * y + 0.5f * z + 1.0f; });
    postfields::CartesianGrid grid;
    CHECK_MESSAGE(postfields::extractCartesianGrid(m, grid), "uniform: cartesian");
    postfields::PointVectorField vf;
    CHECK_MESSAGE(postfields::extractPointVectors(m, grid, "velocity", vf), "uniform: velocity fetch");
    postfields::PointScalarField pf;
    CHECK_MESSAGE(postfields::extractPointScalars(m, grid, "pressure", pf), "uniform: pressure fetch");

    postfields::PostFieldsRequest req;
    req.velocity = vf.data;
    req.pressure = pf.data;
    auto r = postfields::computePostFields(grid, req);
    CHECK_MESSAGE(r.ok, "uniform: ok");
    CHECK_MESSAGE(maxVecAbs(r.vorticity) < 1e-4f, "uniform: vorticity ~ 0");
    CHECK_MESSAGE(maxAbs(r.qCriterion) < 1e-4f, "uniform: Q ~ 0");
    CHECK_MESSAGE(maxAbs(r.helicity) < 1e-4f, "uniform: helicity ~ 0");
    bool gradOk = true;
    for (auto& t : r.gradP) {
        if (std::abs(t.x - 3.0f) > 1e-4f || std::abs(t.y + 2.0f) > 1e-4f ||
            std::abs(t.z - 0.5f) > 1e-4f)
            gradOk = false;
    }
    CHECK_MESSAGE(gradOk, "uniform: grad-p exact (3,-2,0.5)");
}

TEST_CASE("core.postfields.solid_rotation") {
    auto g = linspace(-2, 2, 7);
    RenderMesh m = makeFlowGrid(g, g, g,
        [](float x, float y, float) { return glm::vec3(-y, x, 0.0f); },
        [](float, float, float) { return 0.0f; });
    postfields::CartesianGrid grid;
    CHECK_MESSAGE(postfields::extractCartesianGrid(m, grid), "rotation: cartesian");
    postfields::PointVectorField vf;
    CHECK_MESSAGE(postfields::extractPointVectors(m, grid, "velocity", vf), "rotation: velocity fetch");
    postfields::PostFieldsRequest req;
    req.velocity = vf.data;
    req.pressure = nullptr;
    req.wantGradP = false;
    auto r = postfields::computePostFields(grid, req);
    CHECK_MESSAGE(r.ok, "rotation: ok");
    bool omOk = true, qOk = true;
    for (auto& t : r.vorticity) {
        if (std::abs(t.x) > 1e-3f || std::abs(t.y) > 1e-3f || std::abs(t.z - 2.0f) > 1e-3f)
            omOk = false;
    }
    for (float q : r.qCriterion) {
        if (std::abs(q - 1.0f) > 1e-3f)
            qOk = false;
    }
    CHECK_MESSAGE(omOk, "rotation: vorticity == (0,0,2)");
    CHECK_MESSAGE(qOk, "rotation: Q == 1");
    CHECK_MESSAGE(maxAbs(r.helicity) < 1e-3f, "rotation: helicity ~ 0 (planar)");
}

TEST_CASE("core.postfields.poiseuille") {
    auto g = linspace(-1, 1, 9);
    RenderMesh m = makeFlowGrid(g, g, linspace(0, 1, 4),
        [](float, float y, float) { return glm::vec3(1.0f - y * y, 0.0f, 0.0f); },
        [](float, float, float) { return 0.0f; });
    postfields::CartesianGrid grid;
    CHECK_MESSAGE(postfields::extractCartesianGrid(m, grid), "poiseuille: cartesian");
    postfields::PointVectorField vf;
    CHECK_MESSAGE(postfields::extractPointVectors(m, grid, "velocity", vf), "poiseuille: fetch");
    postfields::PostFieldsRequest req;
    req.velocity = vf.data;
    req.pressure = nullptr;
    req.wantHelicity = false;
    req.wantQ = false;
    req.wantGradP = false;
    auto r = postfields::computePostFields(grid, req);
    const bool subsetOk = r.ok && r.hasVorticity && !r.hasHelicity;
    CHECK_MESSAGE(subsetOk, "poiseuille: subset flags");
    // Central differences of a quadratic are exact - check interior nodes only
    // (one-sided boundary stencils are first-order).
    bool ok = true;
    for (int k = 0; k < grid.dZ; ++k)
        for (int j = 1; j < grid.dY - 1; ++j)
            for (int i = 1; i < grid.dX - 1; ++i) {
                const size_t n = grid.index(i, j, k);
                const float wz = r.vorticity[n].z;
                if (std::abs(r.vorticity[n].x) > 1e-4f || std::abs(r.vorticity[n].y) > 1e-4f)
                    ok = false;
                if (std::abs(wz - 2.0f * grid.y[static_cast<size_t>(j)]) > 1e-3f)
                    ok = false;
            }
    CHECK_MESSAGE(ok, "poiseuille: interior omega_z == 2y");
}

TEST_CASE("core.postfields.non_uniform_rectilinear") {
    const std::vector<float> xs = {0.0f, 0.5f, 2.0f, 3.0f};
    const std::vector<float> ys = {-1.0f, 0.25f, 1.0f};
    const std::vector<float> zs = {0.0f, 4.0f, 5.0f, 9.0f, 10.0f};
    RenderMesh m = makeFlowGrid(xs, ys, zs,
        [](float x, float y, float z) { return glm::vec3(x + 2 * y, y - z, 3 * x + z); },
        [](float x, float y, float z) { return x - 2 * y + 3 * z; });
    postfields::CartesianGrid grid;
    CHECK_MESSAGE(postfields::extractCartesianGrid(m, grid), "rectilinear: cartesian");
    const bool axisSizesOk = (static_cast<int>(grid.x.size()) == 4) &&
                             (static_cast<int>(grid.z.size()) == 5);
    CHECK_MESSAGE(axisSizesOk, "rectilinear: axis sizes");
    postfields::PointVectorField vf;
    postfields::PointScalarField pf;
    CHECK_MESSAGE(postfields::extractPointVectors(m, grid, "velocity", vf), "rectilinear: velocity");
    CHECK_MESSAGE(postfields::extractPointScalars(m, grid, "pressure", pf), "rectilinear: pressure");
    postfields::PostFieldsRequest req;
    req.velocity = vf.data;
    req.pressure = pf.data;
    auto r = postfields::computePostFields(grid, req);
    CHECK_MESSAGE(r.ok, "rectilinear: ok");
    // w = (dw/dy-dv/dz, du/dz-dw/dx, dv/dx-du/dy) = (0-(-1), 0-3, 0-2) = (1,-3,-2)
    bool omOk = true;
    for (auto& t : r.vorticity) {
        if (std::abs(t.x - 1.0f) > 1e-3f || std::abs(t.y + 3.0f) > 1e-3f ||
            std::abs(t.z + 2.0f) > 1e-3f)
            omOk = false;
    }
    CHECK_MESSAGE(omOk, "rectilinear: vorticity == (1,-3,-2)");
    bool gOk = true;
    for (auto& t : r.gradP) {
        if (std::abs(t.x - 1.0f) > 1e-4f || std::abs(t.y + 2.0f) > 1e-4f ||
            std::abs(t.z - 3.0f) > 1e-4f)
            gOk = false;
    }
    CHECK_MESSAGE(gOk, "rectilinear: grad-p == (1,-2,3)");
}

TEST_CASE("core.postfields.rejections") {
    auto g = linspace(0, 2, 3);
    // Curvilinear: sinusoidal z displacement breaks axis alignment.
    RenderMesh curv = makeFlowGrid(g, g, g,
        [](float, float, float) { return glm::vec3(1, 0, 0); },
        [](float, float, float) { return 0.0f; });
    for (size_t n = 0; n < curv.vertices.size() / 3; ++n)
        curv.vertices[n * 3 + 2] += 0.1f * std::sin(curv.vertices[n * 3]);
    postfields::CartesianGrid grid;
    CHECK_MESSAGE(!postfields::extractCartesianGrid(curv, grid), "curvilinear rejected");

    // Unstructured: no grid dims.
    RenderMesh ug = curv;
    ug.gridDimX = ug.gridDimY = ug.gridDimZ = 0;
    CHECK_MESSAGE(!postfields::extractCartesianGrid(ug, grid), "unstructured rejected");

    // 2D slab - supported for VTI 2D CFD (vorticity out-of-plane).
    RenderMesh slab = makeFlowGrid(g, g, {0.0f},
        [](float, float, float) { return glm::vec3(1, 0, 0); },
        [](float, float, float) { return 0.0f; });
    CHECK_MESSAGE(postfields::extractCartesianGrid(slab, grid), "2D slab accepted");
    {
        postfields::PointVectorField vf2;
        postfields::PointScalarField pf2;
        CHECK_MESSAGE(postfields::extractPointVectors(slab, grid, "velocity", vf2), "2D velocity fetch");
        CHECK_MESSAGE(postfields::extractPointScalars(slab, grid, "pressure", pf2), "2D pressure fetch");
        postfields::PostFieldsRequest req2;
        req2.velocity = vf2.data;
        req2.pressure = pf2.data;
        auto r2 = postfields::computePostFields(grid, req2);
        CHECK_MESSAGE(r2.ok, "2D post fields ok");
        float maxV = 0;
        for (auto& v : r2.vorticity)
            maxV = std::max({maxV, std::abs(v.x), std::abs(v.y), std::abs(v.z)});
        CHECK_MESSAGE(maxV < 1e-3f, "2D vorticity ~0");
    }

    // Missing fields / size mismatch.
    RenderMesh m = makeFlowGrid(g, g, g,
        [](float, float, float) { return glm::vec3(1, 0, 0); },
        [](float, float, float) { return 0.0f; });
    CHECK_MESSAGE(postfields::extractCartesianGrid(m, grid), "fixture cartesian");
    postfields::PointVectorField vf;
    CHECK_MESSAGE(!postfields::extractPointVectors(m, grid, "nope", vf), "missing velocity rejected");
    postfields::PointScalarField pf;
    CHECK_MESSAGE(!postfields::extractPointScalars(m, grid, "nope", pf), "missing pressure rejected");
    postfields::PostFieldsRequest bad{};
    auto rb = postfields::computePostFields(grid, bad);
    CHECK_MESSAGE(!rb.ok, "empty request rejected");

    // Naming.
    CHECK_MESSAGE(postfields::vorticityName("U") == "U_vorticity", "vorticity name");
    CHECK_MESSAGE(postfields::helicityName("U") == "U_helicity", "helicity name");
    CHECK_MESSAGE(postfields::qCriterionName("U") == "U_Q", "Q name");
    CHECK_MESSAGE(postfields::gradPName("p") == "p_grad", "grad-p name");
}

TEST_CASE("core.postfields.planar_rotation_2d") {
    auto g = linspace(-2, 2, 7);
    RenderMesh m = makeFlowGrid(g, g, {0.0f},
        [](float x, float y, float) { return glm::vec3(-y, x, 0.0f); },
        [](float, float, float) { return 0.0f; });
    postfields::PostFieldsRequest req;
    req.velocity = m.pointVectorsData.data();
    req.pressure = m.attributes->pointScalars["pressure"].data();
    auto r = postfields::computePostFields(m, req);
    CHECK_MESSAGE(r.ok, "2D rotation: ok");
    float maxErr = 0;
    for (auto& v : r.vorticity)
        maxErr = std::max({maxErr, std::abs(v.x), std::abs(v.y), std::abs(v.z - 2.0f)});
    CHECK_MESSAGE(maxErr < 1e-3f, "2D rotation: vorticity == (0,0,2)");
    float maxQ = 0;
    for (float q : r.qCriterion)
        maxQ = std::max(maxQ, std::abs(q - 1.0f));
    CHECK_MESSAGE(maxQ < 1e-3f, "2D rotation: Q == 1");
}

TEST_CASE("core.postfields.unstructured_2d_lsq") {
    RenderMesh m;
    const int dX = 5, dY = 5;
    const size_t N = static_cast<size_t>(dX) * dY;
    m.gridDimX = m.gridDimY = m.gridDimZ = 0; // force unstructured path
    m.datasetType = "UNSTRUCTURED_GRID";
    m.vertices.resize(N * 3);
    m.pointVectorsData.resize(N);
    m.pointVectorCount = N;
    m.pointVectorOffset["velocity"] = 0;
    m.availableVectorNames.push_back("velocity");
    m.vectorName = "velocity";
    m.attributes = DatasetAttributes{};
    std::vector<float>& ps = m.attributes->pointScalars["pressure"];
    ps.resize(N, 0.0f);
    auto node = [&](int i, int j) { return static_cast<size_t>(i) + static_cast<size_t>(j) * dX; };
    for (int j = 0; j < dY; ++j)
        for (int i = 0; i < dX; ++i) {
            const size_t n = node(i, j);
            m.vertices[n * 3 + 0] = 0.5f * i;
            m.vertices[n * 3 + 1] = 0.5f * j;
            m.vertices[n * 3 + 2] = 0.0f;
            m.pointVectorsData[n] = glm::vec3(-0.5f * j, 0.5f * i, 0.0f);
        }
    for (int j = 0; j + 1 < dY; ++j)
        for (int i = 0; i + 1 < dX; ++i) {
            const uint32_t a = static_cast<uint32_t>(node(i, j));
            const uint32_t b = static_cast<uint32_t>(node(i + 1, j));
            const uint32_t c = static_cast<uint32_t>(node(i + 1, j + 1));
            const uint32_t d = static_cast<uint32_t>(node(i, j + 1));
            m.indices.push_back(a);
            m.indices.push_back(b);
            m.indices.push_back(c);
            m.indices.push_back(a);
            m.indices.push_back(c);
            m.indices.push_back(d);
        }
    m.bounds.minX = 0;
    m.bounds.maxX = 2;
    m.bounds.minY = 0;
    m.bounds.maxY = 2;
    postfields::PostFieldsRequest req;
    req.velocity = m.pointVectorsData.data();
    req.pressure = ps.data();
    req.wantGradP = false;
    auto r = postfields::computePostFieldsUnstructured(m, req);
    CHECK_MESSAGE(r.ok, "2D unstructured: ok");
    if (r.ok) {
        float maxErr = 0;
        size_t badN = 0;
        for (auto& v : r.vorticity) {
            const float e = std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z - 2.0f)});
            if (e > 0.1f)
                ++badN;
            maxErr = std::max(maxErr, e);
        }
        MESSAGE("max|err|=", maxErr, " nodes>0.1=", badN, "/", r.vorticity.size());
        CHECK_MESSAGE(badN == 0, "2D unstructured: all nodes omega==(0,0,2) (rotation exact for LSQ)");
    }
}

} // TEST_SUITE("core")
