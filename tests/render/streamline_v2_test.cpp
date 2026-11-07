// Regression tests for the reimplemented streamline pipeline (plan §10):
// domain validation, parameter budgets, determinism, integrator semantics.
#include "render/streamlines/domain/FieldDomain.h"
#include "render/streamlines/domain/Sampling.h"
#include "render/streamlines/integrate/Integration.h"
#include "render/streamlines/geometry/GeometryBuilder.h"
#include "doctest/doctest.h"
#include <atomic>
#include <cmath>
#include <limits>

using namespace streamline;

namespace {
std::vector<glm::vec3> gridPositions(int nx, int ny, int nz, float step = 1.0f) {
    std::vector<glm::vec3> p;
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) p.emplace_back(i*step, j*step, k*step);
    return p;
}
std::vector<glm::vec3> uniformField(size_t n, glm::vec3 v) { return std::vector<glm::vec3>(n, v); }
}

TEST_SUITE("render") {

TEST_CASE("render.streamline_v2.domain_rejects_nan") {
    auto pos = gridPositions(2,2,2);
    auto val = uniformField(pos.size(), {1,0,0});
    pos[3].x = std::numeric_limits<float>::quiet_NaN();
    CHECK(FieldDomain::build(pos, val, StructuredDims{2,2,2}) == std::nullopt);
}

TEST_CASE("render.streamline_v2.domain_rejects_incoherent_grid") {
    auto pos = gridPositions(2,2,2);
    auto val = uniformField(pos.size(), {1,0,0});
    std::swap(pos[1], pos[6]); // break index->position coherence
    CHECK(FieldDomain::build(pos, val, StructuredDims{2,2,2}) == std::nullopt);
}

TEST_CASE("render.streamline_v2.domain_accepts_coherent_grid") {
    auto pos = gridPositions(3,3,3);
    auto val = uniformField(pos.size(), {0,1,0});
    auto d = FieldDomain::build(pos, val, StructuredDims{3,3,3});
    REQUIRE(d.has_value());
    CHECK(d->axisX().size() == 3);
    CHECK(std::abs(d->bounds().extent - 2.0f) < 1e-5f);
}

TEST_CASE("render.streamline_v2.effective_steps_honours_both_caps") {
    IntegrationParameters p;
    p.stepSize = 0.1f; p.maxSteps = 10; p.maxPathLength = 100.0f;
    CHECK(p.effectiveMaxSteps(1.0f) == 10);          // maxSteps binds
    p.maxSteps = 5000; p.maxPathLength = 0.5f;
    CHECK(p.effectiveMaxSteps(1.0f) == 5);           // maxPathLength binds
}

TEST_CASE("render.streamline_v2.validate_rejects_bad_params") {
    auto pos = gridPositions(2,2,2);
    auto val = uniformField(pos.size(), {1,0,0});
    auto d = FieldDomain::build(pos, val, StructuredDims{2,2,2});
    REQUIRE(d.has_value());
    IntegrationParameters p; p.stepSize = -1.0f;
    CHECK(validateParameters(p, *d) == std::nullopt);
    p.stepSize = std::numeric_limits<float>::quiet_NaN();
    CHECK(validateParameters(p, *d) == std::nullopt);
    p.stepSize = 0.1f; p.seedCount = 999999;
    CHECK(validateParameters(p, *d) == std::nullopt);
}

TEST_CASE("render.streamline_v2.deterministic_seeds") {
    auto pos = gridPositions(2,2,2);
    auto val = uniformField(pos.size(), {1,0,0});
    auto d = FieldDomain::build(pos, val, StructuredDims{2,2,2});
    REQUIRE(d.has_value());
    IntegrationParameters p; p.seedCount = 27; p.jitter = 1.0; p.rngSeed = 42;
    auto a = generateSeeds(*d, p);
    auto b = generateSeeds(*d, p);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);
}

TEST_CASE("render.streamline_v2.uniform_field_straight_line") {
    auto pos = gridPositions(5,5,5);
    auto val = uniformField(pos.size(), {1,0,0});
    auto d = FieldDomain::build(pos, val, StructuredDims{5,5,5});
    REQUIRE(d.has_value());
    IntegrationParameters p;
    p.stepSize = 0.25f; p.maxSteps = 200; p.seedCount = 8; p.seedMode = SeedMode::Volume;
    std::atomic<bool> cancel{false};
    auto data = integrateAll(*d, p, cancel);
    REQUIRE(!data.paths.empty());
    for (auto& path : data.paths) {
        REQUIRE(path.valid());
        for (auto& pt : path.points) CHECK(std::abs(pt.y - path.points[0].y) < 1e-4f);
        CHECK(path.totalLength > 0.5f);
    }
}

TEST_CASE("render.streamline_v2.zero_field_no_paths") {
    auto pos = gridPositions(3,3,3);
    auto val = uniformField(pos.size(), {0,0,0});
    auto d = FieldDomain::build(pos, val, StructuredDims{3,3,3});
    REQUIRE(d.has_value());
    IntegrationParameters p; p.seedCount = 27;
    std::atomic<bool> cancel{false};
    auto data = integrateAll(*d, p, cancel);
    CHECK(data.paths.empty());
}

TEST_CASE("render.streamline_v2.geometry_vertex_count") {
    auto pos = gridPositions(5,5,5);
    auto val = uniformField(pos.size(), {1,0,0});
    auto d = FieldDomain::build(pos, val, StructuredDims{5,5,5});
    REQUIRE(d.has_value());
    IntegrationParameters p; p.stepSize = 0.25f; p.maxSteps = 50; p.seedCount = 4;
    std::atomic<bool> cancel{false};
    auto data = integrateAll(*d, p, cancel);
    REQUIRE(!data.paths.empty());
    GeometryConfig cfg; cfg.style = 0; cfg.baseWidth = 0.01f; cfg.extent = 4.0f;
    auto geom = buildGeometry(data, cfg);
    // Line style: exactly 2 vertices per segment.
    size_t segments = 0;
    for (auto& path : data.paths) {
        segments += path.points.size() - 1;
        // Splice regression: backward tip must be kept, seed not duplicated.
        CHECK(path.points.front().x < path.points.back().x);
        for (size_t i = 1; i < path.points.size(); ++i)
            CHECK(glm::length(path.points[i] - path.points[i-1]) > 1e-9f);
    }
    CHECK(geom.lineCount == (int)(segments * 2));
    CHECK(geom.verts.size() == (size_t)geom.lineCount * 10);
}

TEST_CASE("render.streamline_v2.parallel_transport_continuity") {
    Frame f = initialFrame({0,1,0});
    Frame g = transportFrame(f, {0.01f, 1.0f, 0.0f});
    CHECK(std::abs(glm::dot(g.normal, g.tangent)) < 1e-4f);
    CHECK(std::abs(glm::length(g.normal) - 1.0f) < 1e-4f);
    CHECK(std::abs(glm::length(g.binormal) - 1.0f) < 1e-4f);
}

} // TEST_SUITE
