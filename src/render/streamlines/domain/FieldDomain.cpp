#include "render/streamlines/domain/FieldDomain.h"
#include <algorithm>
#include <cmath>

namespace streamline {

std::optional<FieldDomain> FieldDomain::build(
    std::vector<glm::vec3> positions,
    std::vector<glm::vec3> values,
    std::optional<StructuredDims> dims)
{
    if (positions.empty() || values.empty()) return std::nullopt;
    if (positions.size() != values.size())   return std::nullopt;
    constexpr size_t kMaxPoints = 4'000'000;
    if (positions.size() > kMaxPoints)       return std::nullopt;

    glm::vec3 mn = positions[0], mx = positions[0];
    for (size_t i = 0; i < positions.size(); ++i) {
        const auto& p = positions[i];
        const auto& v = values[i];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
            !std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
            return std::nullopt;
        mn = glm::min(mn, p); mx = glm::max(mx, p);
    }
    if (!(mx.x >= mn.x && mx.y >= mn.y && mx.z >= mn.z))
        return std::nullopt;
    float extent = std::max({mx.x - mn.x, mx.y - mn.y, mx.z - mn.z});
    if (!(extent > 0.0f) || !std::isfinite(extent))
        return std::nullopt;

    FieldDomain d;
    d.m_positions = std::move(positions);
    d.m_values    = std::move(values);
    d.m_bounds    = {mn, mx, extent};
    const float vol = std::abs((mx.x-mn.x)*(mx.y-mn.y)*(mx.z-mn.z));
    d.m_avgSpacing = (vol > 0.0f)
        ? std::pow(vol / static_cast<float>(d.m_positions.size()), 1.0f/3.0f)
        : extent;
    if (!std::isfinite(d.m_avgSpacing) || d.m_avgSpacing <= 0.0f)
        d.m_avgSpacing = extent;

    if (dims) {
        const long long prod = (long long)dims->nx * dims->ny * dims->nz;
        if (dims->nx <= 0 || dims->ny <= 0 || dims->nz <= 0 ||
            prod != (long long)d.m_positions.size())
            return std::nullopt;

        // Derive axes by *sampling the index order*, not by sorting all
        // coordinates (sorting made the old monotonicity check vacuous).
        const int nx = dims->nx, ny = dims->ny, nz = dims->nz;
        auto at = [&](int i, int j, int k) -> const glm::vec3& {
            return d.m_positions[(size_t)i + (size_t)j*nx + (size_t)k*nx*ny];
        };
        d.m_axisX.resize(nx); d.m_axisY.resize(ny); d.m_axisZ.resize(nz);
        for (int i = 0; i < nx; ++i) d.m_axisX[i] = at(i,0,0).x;
        for (int j = 0; j < ny; ++j) d.m_axisY[j] = at(0,j,0).y;
        for (int k = 0; k < nz; ++k) d.m_axisZ[k] = at(0,0,k).z;
        // Coherence: every point must sit on its index-derived axis coords.
        const float tol = std::max(1e-5f * extent, 1e-9f);
        for (int k = 0; k < nz; ++k)
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i) {
                    const auto& p = at(i,j,k);
                    if (std::abs(p.x - d.m_axisX[i]) > tol ||
                        std::abs(p.y - d.m_axisY[j]) > tol ||
                        std::abs(p.z - d.m_axisZ[k]) > tol)
                        return std::nullopt;
                }
        d.m_dims = dims;
    }
    return d;
}

} // namespace streamline
