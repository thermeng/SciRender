#include "support/mesh_fixtures.h"

#include <algorithm>
#include <cmath>

namespace test_support {

RenderMesh makeFlowGrid(const std::vector<float>& xs,
                        const std::vector<float>& ys,
                        const std::vector<float>& zs,
                        VelocityFn vel,
                        PressureFn pres,
                        const std::string& vName,
                        const std::string& pName) {
    RenderMesh m;
    const int dX = static_cast<int>(xs.size());
    const int dY = static_cast<int>(ys.size());
    const int dZ = static_cast<int>(zs.size());
    m.gridDimX = dX;
    m.gridDimY = dY;
    m.gridDimZ = dZ;
    m.datasetType = "IMAGEDATA";
    const size_t N = static_cast<size_t>(dX) * dY * dZ;
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
                const size_t n = static_cast<size_t>(i) + static_cast<size_t>(j) * dX +
                                static_cast<size_t>(k) * dX * dY;
                const float x = xs[static_cast<size_t>(i)];
                const float y = ys[static_cast<size_t>(j)];
                const float z = zs[static_cast<size_t>(k)];
                m.vertices[n * 3 + 0] = x;
                m.vertices[n * 3 + 1] = y;
                m.vertices[n * 3 + 2] = z;
                m.pointVectorsData[n] = vel(x, y, z);
                ps[n] = pres(x, y, z);
            }
    m.availableScalarNames.push_back(pName);
    m.scalarName = pName;
    m.scalars = ps;
    m.bounds.minX = xs.front();
    m.bounds.maxX = xs.back();
    m.bounds.minY = ys.front();
    m.bounds.maxY = ys.back();
    m.bounds.minZ = zs.front();
    m.bounds.maxZ = zs.back();
    return m;
}

std::vector<float> linspace(float a, float b, int n) {
    std::vector<float> v;
    v.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        v.push_back(a + (b - a) * i / (n - 1));
    return v;
}

float maxAbs(const std::vector<float>& v) {
    float m = 0;
    for (float f : v)
        m = std::max(m, std::abs(f));
    return m;
}

float maxVecAbs(const std::vector<glm::vec3>& v) {
    float m = 0;
    for (const auto& t : v)
        m = std::max({m, std::abs(t.x), std::abs(t.y), std::abs(t.z)});
    return m;
}

} // namespace test_support
