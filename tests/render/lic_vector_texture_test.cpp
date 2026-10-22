// LIC vector texture: resolution and content of the field the LIC traces.
//
// The LIC does not read the render mesh's vectors directly - it reads a 3D
// texture that VectorTextureCache builds by sampling the field onto a regular
// grid. That grid decides what the user sees: a grid coarser than the data
// averages the small structures away, and an interpolation whose radius is a
// fraction of the DOMAIN blurs everything below that size.
//
// Two defects made the LIC lose the secondary vortices of a lid driven
// cavity while the arrow glyphs (which read the raw per-vertex vectors) still
// showed them:
//   1) the resolution was ceil(cbrt(numPoints)) clamped to [16, 32] - the
//      3D estimate for a 2D field, so a 41x41 cavity became a 16x16 texture;
//   2) the resampler's spatial hash was a fixed 8^3 cube, which made the
//      interpolation radius 1/8 of the domain - five cells of a 41x41 field.
//
// These cases pin the resolution and the content of the sampled grid.
#include "doctest/doctest.h"
#include "render/passes/VectorTextureCache.h"
#include "support/mesh_fixtures.h"

#include <glm/glm.hpp>

#include <cmath>
#include <vector>

using namespace test_support;

namespace {

// A regular point set on a unit square (an unstructured mesh as far as the
// renderer is concerned: gridDim* stays 0) carrying a small swirl in a corner
// patch - the structure the LIC used to lose.
struct SwirlField {
    RenderMesh mesh;
    std::vector<glm::vec3> vectors;
};

SwirlField makeSwirlField(int n, double swirlRadius) {
    SwirlField f;
    RenderMesh& m = f.mesh;
    m.datasetType = "CFD MESH";
    m.gridDimX = m.gridDimY = m.gridDimZ = 0;   // unstructured: forces the resample path

    const size_t N = static_cast<size_t>(n) * n;
    m.vertices.resize(N * 3);
    f.vectors.resize(N);

    const double h = 1.0 / (n - 1);
    // The vortex sits in the middle of the domain so that a full circle
    // around its core stays inside the field.
    const double cx = 0.5, cy = 0.5;
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const size_t k = static_cast<size_t>(i) + static_cast<size_t>(j) * n;
            const double x = i * h;
            const double y = j * h;
            m.vertices[k * 3 + 0] = static_cast<float>(x);
            m.vertices[k * 3 + 1] = static_cast<float>(y);
            m.vertices[k * 3 + 2] = 0.0f;

            // A vortex of radius swirlRadius around (cx, cy), decaying to
            // zero at its edge. Everywhere else the field is at rest.
            const double dx = x - cx, dy = y - cy;
            const double d = std::sqrt(dx * dx + dy * dy);
            glm::vec3 v(0.0f);
            if (d < swirlRadius && d > 1e-9) {
                const double s = 1.0 - d / swirlRadius;
                v = glm::vec3(static_cast<float>(-dy * s), static_cast<float>(dx * s), 0.0f);
            }
            f.vectors[k] = v;
        }
    }
    m.bounds.minX = 0.0f; m.bounds.maxX = 1.0f;
    m.bounds.minY = 0.0f; m.bounds.maxY = 1.0f;
    m.bounds.minZ = 0.0f; m.bounds.maxZ = 0.0f;
    return f;
}

} // namespace

TEST_SUITE("render") {

// The texture of a regular grid must be that grid: the LIC then samples the
// field at its own resolution and no structure is averaged away.
TEST_CASE("render.lic.grid_dims_follow_the_data") {
    constexpr int n = 41;
    const SwirlField f = makeSwirlField(n, 0.12);
    const auto d = VectorTextureCache::chooseGridDims(f.mesh, f.vectors.size(), false);
    CHECK_MESSAGE(d.x == n, "a 41x41 field must give a 41x41 texture");
    CHECK_MESSAGE(d.y == n, "a 41x41 field must give a 41x41 texture");
    CHECK_MESSAGE(d.z == 1, "a 2D slab must give a single slice");
}

// A 2D field has sqrt(n) points per axis, not cbrt(n). The old cubic
// estimate clamped to 16 turned a 41x41 field into a 16x16 texture.
TEST_CASE("render.lic.grid_dims_2d_is_not_cubic") {
    constexpr int n = 41;
    const SwirlField f = makeSwirlField(n, 0.12);
    const auto d = VectorTextureCache::chooseGridDims(f.mesh, f.vectors.size(), false);
    const int cubic = static_cast<int>(std::ceil(std::cbrt(static_cast<double>(n * n))));
    CHECK_MESSAGE(d.x > cubic, "the per-axis count of a 2D field must not be a cubic root");
}

// The regression itself: a small vortex has to survive the resampling. With
// the old 16^3 grid and the domain-sized interpolation radius the vortex was
// averaged into the surrounding (resting) flow and simply disappeared.
TEST_CASE("render.lic.resample_preserves_small_structures") {
    constexpr int n = 41;
    const double radius = 0.12;
    const SwirlField f = makeSwirlField(n, radius);

    VectorTextureCache cache;
    std::vector<glm::vec3> grid;
    int dx = 0, dy = 0, dz = 0;
    REQUIRE(cache.buildGrid(f.mesh, f.vectors.data(), f.vectors.size(), false, grid, dx, dy, dz));
    CHECK_MESSAGE(dx == n, "the sampled grid must keep the data resolution");
    CHECK_MESSAGE(dy == n, "the sampled grid must keep the data resolution");
    CHECK_MESSAGE(dz == 1, "a 2D slab must give a single slice");

    // The strongest velocity of the field lives in the vortex core.
    float srcMax = 0.0f;
    for (const auto& v : f.vectors) srcMax = std::max(srcMax, glm::length(v));
    float gridMax = 0.0f;
    for (const auto& v : grid) gridMax = std::max(gridMax, glm::length(v));
    CHECK_MESSAGE(srcMax > 0.0f, "the source field must have a vortex");
    CHECK_MESSAGE(gridMax > 0.8f * srcMax,
                  "the sampled grid must keep the vortex: the LIC shows what survives here");

    // And the vortex must still be a VORTEX: the direction has to turn around
    // the core, not just be non-zero. The swirl velocity (-y, x) points
    // perpendicular to the radius, so the velocity's own angle has to sweep
    // most of a full turn around the core.
    const double h = 1.0 / (n - 1);
    const double r = 0.5 * radius;              // inside the vortex
    double minDir = 1e9, maxDir = -1e9;
    int sampled = 0;
    for (int a = 0; a < 8; ++a) {
        const double th = a * M_PI / 4.0;
        const double x = 0.5 + r * std::cos(th);
        const double y = 0.5 + r * std::sin(th);
        const int i = static_cast<int>(std::lround(x / h));
        const int j = static_cast<int>(std::lround(y / h));
        if (i < 0 || j < 0 || i >= n || j >= n) continue;
        const glm::vec3 v = grid[static_cast<size_t>(i) + static_cast<size_t>(j) * n];
        if (glm::length(v) < 1e-6) continue;
        ++sampled;
        const double ang = std::atan2(v.y, v.x);
        minDir = std::min(minDir, ang);
        maxDir = std::max(maxDir, ang);
    }
    CHECK_MESSAGE(sampled == 8, "the vortex core must be sampled");
    CHECK_MESSAGE(maxDir - minDir > 2.0,
                  "the sampled vortex must still turn: the direction has to vary around the core");
}

// A point set that is NOT a regular grid (an unstructured volume mesh) must
// still get a sane, 2D-aware resolution.
TEST_CASE("render.lic.grid_dims_generic_slab") {
    RenderMesh m;
    m.datasetType = "CFD MESH";
    m.gridDimX = m.gridDimY = m.gridDimZ = 0;
    const int n = 40;
    const size_t N = static_cast<size_t>(n) * n;
    m.vertices.resize(N * 3);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const size_t k = static_cast<size_t>(i) + static_cast<size_t>(j) * n;
            m.vertices[k * 3 + 0] = static_cast<float>(i) / n;
            m.vertices[k * 3 + 1] = static_cast<float>(j) / n;
            m.vertices[k * 3 + 2] = 0.0f;
        }
    m.bounds.minX = 0.0f; m.bounds.maxX = 1.0f;
    m.bounds.minY = 0.0f; m.bounds.maxY = 1.0f;
    m.bounds.minZ = 0.0f; m.bounds.maxZ = 0.0f;

    const auto d = VectorTextureCache::chooseGridDims(m, N, false);
    CHECK_MESSAGE(d.z == 1, "a slab must give a single slice");
    CHECK_MESSAGE(d.x >= 16, "the resolution must not collapse below the old floor");
    CHECK_MESSAGE(d.y >= 16, "the resolution must not collapse below the old floor");
    CHECK_MESSAGE(d.x == d.y, "a square domain must give a square texture");
}

} // TEST_SUITE("render")
