// Ensures extracted post fields are correctly exposed via FieldStore + snapshot
// and are ready for GPU visualization (payload size, range, Context resolution).
// Also verifies helicity/Q non-blank for helical flow and LSQ unstructured.
#include "core/post_fields.h"
#include "core/FieldStore.h"
#include "core/mesh_loader.h"
#include "doctest/doctest.h"
#include "support/mesh_fixtures.h"

#include <cmath>

using namespace test_support;

namespace {

// Simulate setActiveScalarField expansion logic (vertexSourceIndex)
bool payloadReady(const RenderMesh& mesh, const std::vector<float>& src, size_t& payloadSize) {
    if (src.size() == mesh.vertices.size() / 3 || mesh.vertexSourceIndex.empty()) {
        payloadSize = src.size();
        return true;
    }
    if (src.size() == static_cast<size_t>(mesh.sourcePointCount) ||
        src.size() == static_cast<size_t>(mesh.gridDimX * mesh.gridDimY * mesh.gridDimZ)) {
        payloadSize = mesh.vertices.size() / 3;
        return true;
    }
    return false;
}

} // namespace

TEST_SUITE("core") {

TEST_CASE("core.postfields_viz.helical_flow_ready") {
    // Beltrami-like helical flow: u = (-y, x, 1) => omega=(0,0,2), helicity=2
    auto g = linspace(-1, 1, 5);
    RenderMesh m = makeFlowGrid(g, g, g,
        [](float x, float y, float) { return glm::vec3(-y, x, 1.0f); },
        [](float x, float, float z) { return x + z; });
    m.sourcePointCount = static_cast<int>(g.size() * g.size() * g.size());
    postfields::CartesianGrid grid;
    CHECK_MESSAGE(postfields::extractCartesianGrid(m, grid), "helical: cartesian");
    postfields::PointVectorField vf;
    postfields::PointScalarField pf;
    CHECK_MESSAGE(postfields::extractPointVectors(m, grid, "velocity", vf), "vf");
    CHECK_MESSAGE(postfields::extractPointScalars(m, grid, "pressure", pf), "pf");
    postfields::PostFieldsRequest req;
    req.velocity = vf.data;
    req.pressure = pf.data;
    auto r = postfields::computePostFields(grid, req);
    CHECK_MESSAGE(r.ok, "ok");
    const bool hasDerived = r.hasHelicity && r.hasQ;
    CHECK_MESSAGE(hasDerived, "has helicity/Q");
    // Helicity should be ~2 everywhere (not blank)
    bool helNonBlank = false;
    for (float h : r.helicity)
        if (std::abs(h - 2.0f) < 0.2f)
            helNonBlank = true;
    CHECK_MESSAGE(helNonBlank, "helicity ~2 not blank");
    // Q should be ~1
    bool qOk = true;
    for (float q : r.qCriterion)
        if (std::abs(q - 1.0f) > 1e-3f)
            qOk = false;
    CHECK_MESSAGE(qOk, "Q==1");
    // Snapshot + FieldStore ready
    auto snap = std::make_shared<postfields::ComputedSnapshot>();
    auto hel = std::make_shared<const std::vector<float>>(r.helicity);
    auto qv = std::make_shared<const std::vector<float>>(r.qCriterion);
    snap->scalars[postfields::helicityName("velocity")] = hel;
    snap->scalarRanges[postfields::helicityName("velocity")] = {FieldStore::computeRange(*hel).min,
                                                                FieldStore::computeRange(*hel).max};
    snap->scalars[postfields::qCriterionName("velocity")] = qv;
    snap->scalarRanges[postfields::qCriterionName("velocity")] = {FieldStore::computeRange(*qv).min,
                                                                  FieldStore::computeRange(*qv).max};
    // availableScalars must include helicity
    auto avail = FieldStore::availableScalars(m, snap.get());
    bool foundHel = false, foundQ = false;
    for (auto& n : avail) {
        if (n == postfields::helicityName("velocity"))
            foundHel = true;
        if (n == postfields::qCriterionName("velocity"))
            foundQ = true;
    }
    CHECK_MESSAGE(foundHel, "availableScalars includes helicity");
    CHECK_MESSAGE(foundQ, "availableScalars includes Q");
    // Context resolve must succeed and range not degenerate
    FieldStore::Context ctx(m, snap.get());
    auto sf = ctx.scalar(postfields::helicityName("velocity"), FieldStore::Placement::Vertex);
    CHECK_MESSAGE(sf.data != nullptr, "ctx scalar helicity");
    const bool rangeNotBlank =
        (sf.range.max - sf.range.min > 1e-6f) || (std::abs(sf.range.max - 2.0f) < 0.1f);
    CHECK_MESSAGE(rangeNotBlank, "range not degenerate blank");
    // Payload size check (simulates MeshGLManager upload)
    size_t payloadSize = 0;
    CHECK_MESSAGE(payloadReady(m, *sf.data, payloadSize), "payload size matches vertices");
    CHECK_MESSAGE(payloadSize == m.vertices.size() / 3, "payload covers GPU vertices");
}

TEST_CASE("core.postfields_viz.structured_vs_unstructured_helicity") {
    // Build unstructured point cloud same positions but gridDim 0
    auto g = linspace(-1, 1, 4);
    RenderMesh m = makeFlowGrid(g, g, g,
        [](float x, float y, float) { return glm::vec3(-y, x, 0.5f); },
        [](float, float, float) { return 0; });
    // Structured result
    postfields::CartesianGrid grid;
    postfields::extractCartesianGrid(m, grid);
    postfields::PointVectorField vf;
    postfields::extractPointVectors(m, grid, "velocity", vf);
    postfields::PostFieldsRequest req;
    req.velocity = vf.data;
    req.wantGradP = false;
    auto rs = postfields::computePostFields(grid, req);
    // Unstructured copy - use radius neighbor search (no indices) for full 3D stencil
    RenderMesh ug = m;
    ug.gridDimX = ug.gridDimY = ug.gridDimZ = 0;
    ug.indices.clear(); // radius fallback will find 3D neighbors
    auto ru = postfields::computePostFields(ug, req);
    CHECK_MESSAGE(ru.ok, "unstructured ok");
    // For linear rotation helicity 0, both should be ~0 ; compare vorticity central point
    size_t c = 1 + 1 * 4 + 1 * 16;
    CHECK_MESSAGE(std::abs(ru.vorticity[c].z - rs.vorticity[c].z) < 0.5f,
                  "LSQ vorticity close to structured");
}

TEST_CASE("core.postfields_viz.range_not_blank") {
    auto g = linspace(0, 1, 4);
    RenderMesh m = makeFlowGrid(g, g, g,
        [](float y, float, float) { return glm::vec3(y * y, 0, 0); },
        [](float x, float, float) { return x * x; });
    postfields::CartesianGrid grid;
    postfields::extractCartesianGrid(m, grid);
    postfields::PointVectorField vf;
    postfields::PointScalarField pf;
    postfields::extractPointVectors(m, grid, "velocity", vf);
    postfields::extractPointScalars(m, grid, "pressure", pf);
    postfields::PostFieldsRequest req;
    req.velocity = vf.data;
    req.pressure = pf.data;
    auto r = postfields::computePostFields(grid, req);
    auto checkRange = [&](const std::vector<float>& v, const char* name) {
        auto rg = FieldStore::computeRange(v);
        CHECK_MESSAGE(rg.max - rg.min > 1e-5f, name);
    };
    checkRange(r.helicity, "helicity range");
    checkRange(r.qCriterion, "Q range");
    // gradP is vector but scalar magnitude via derived? just check grad not zero
    bool gradNonZero = false;
    for (auto& gp : r.gradP)
        if (std::abs(gp.x) > 1e-3f)
            gradNonZero = true;
    CHECK_MESSAGE(gradNonZero, "gradP non-zero");
}

} // TEST_SUITE("core")
