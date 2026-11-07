// Standalone CPU benchmark for the streamline integration pipeline.
// No Qt/GL. Build: cmake -DSCIRENDER_BUILD_TOOLS=ON (tools/) or compile
// manually against RenderCore. Prints per-scenario timings; CI compares
// against the budgets in docs/streamline-architecture.md (plan §7).
#include "render/streamlines/domain/FieldDomain.h"
#include "render/streamlines/integrate/Integration.h"
#include "render/streamlines/geometry/GeometryBuilder.h"
#include <atomic>
#include <chrono>
#include <cstdio>

using namespace streamline;

static double bench(const char* name, int nx, int seedCount, int steps) {
    std::vector<glm::vec3> pos, val;
    for (int k = 0; k < nx; ++k)
        for (int j = 0; j < nx; ++j)
            for (int i = 0; i < nx; ++i) {
                pos.emplace_back(i * 0.1f, j * 0.1f, k * 0.1f);
                // Smooth divergent-ish field.
                val.emplace_back(i - nx/2.0f, j - nx/2.0f, k - nx/2.0f);
            }
    auto d = FieldDomain::build(pos, val, StructuredDims{nx, nx, nx});
    if (!d) { std::printf("%s: domain build failed\n", name); return -1; }
    IntegrationParameters p;
    p.stepSize = 0.05f; p.maxSteps = steps; p.seedCount = seedCount;
    std::atomic<bool> cancel{false};
    auto t0 = std::chrono::steady_clock::now();
    auto data = integrateAll(*d, p, cancel);
    GeometryConfig cfg; cfg.style = 1; cfg.baseWidth = 0.005f; cfg.extent = nx * 0.1f;
    auto geom = buildGeometry(data, cfg);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("%s: %.1f ms  paths=%zu verts=%d\n", name, ms, data.paths.size(), geom.lineCount);
    return ms;
}

int main(int argc, char** argv) {
    int which = argc > 1 ? std::atoi(argv[1]) : 0;
    if (which == 0) bench("fixture 5^3 / 27 seeds", 5, 27, 200);
    if (which == 1) bench("mid 32^3 / 1k seeds", 32, 1000, 500);
    if (which == 2) bench("large 64^3 / 4k seeds", 64, 4000, 500);
    return 0;
}
