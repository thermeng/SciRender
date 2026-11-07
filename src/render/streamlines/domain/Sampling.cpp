#include "render/streamlines/domain/Sampling.h"
#include <algorithm>
#include <cmath>

namespace streamline {

UniformGrid::UniformGrid(std::span<const glm::vec3> positions, float cellSize,
                         glm::vec3 minB, glm::vec3 maxB)
    : m_origin(minB), m_positions(positions)
{
    m_cellSize = std::max(cellSize, 1e-9f);
    glm::vec3 extent = maxB - minB;
    auto dim = [&](float e) {
        if (!std::isfinite(e) || e <= 0.0f) return 1;
        return std::min(256, std::max(1, (int)std::ceil(e / m_cellSize)));
    };
    m_nx = dim(extent.x); m_ny = dim(extent.y); m_nz = dim(extent.z);
    constexpr size_t kMaxCells = 1u << 20;
    while ((size_t)m_nx * m_ny * m_nz > kMaxCells && (m_nx > 1 || m_ny > 1 || m_nz > 1)) {
        if (m_nx >= m_ny && m_nx >= m_nz && m_nx > 1) --m_nx;
        else if (m_ny >= m_nz && m_ny > 1) --m_ny;
        else if (m_nz > 1) --m_nz;
    }
    m_cells.resize((size_t)m_nx * m_ny * m_nz);
    for (int i = 0; i < (int)positions.size(); ++i) {
        const auto& p = positions[i];
        int cx = std::clamp((int)((p.x - m_origin.x) / m_cellSize), 0, m_nx - 1);
        int cy = std::clamp((int)((p.y - m_origin.y) / m_cellSize), 0, m_ny - 1);
        int cz = std::clamp((int)((p.z - m_origin.z) / m_cellSize), 0, m_nz - 1);
        m_cells[(size_t)cx + (size_t)cy*m_nx + (size_t)cz*m_nx*m_ny].push_back(i);
    }
}

int UniformGrid::nearest(const glm::vec3& p, float maxDistSq) const {
    int cx = std::clamp((int)((p.x - m_origin.x) / m_cellSize), 0, m_nx - 1);
    int cy = std::clamp((int)((p.y - m_origin.y) / m_cellSize), 0, m_ny - 1);
    int cz = std::clamp((int)((p.z - m_origin.z) / m_cellSize), 0, m_nz - 1);
    float best = maxDistSq;
    int bestIdx = -1;
    for (int dz = -1; dz <= 1; ++dz) {
        int z = cz + dz; if (z < 0 || z >= m_nz) continue;
        for (int dy = -1; dy <= 1; ++dy) {
            int y = cy + dy; if (y < 0 || y >= m_ny) continue;
            for (int dx = -1; dx <= 1; ++dx) {
                int x = cx + dx; if (x < 0 || x >= m_nx) continue;
                for (int idx : m_cells[(size_t)x + (size_t)y*m_nx + (size_t)z*m_nx*m_ny]) {
                    glm::vec3 d = m_positions[idx] - p;
                    float d2 = glm::dot(d, d);
                    if (d2 < best) { best = d2; bestIdx = idx; }
                }
            }
        }
    }
    return bestIdx;
}

// ---------------------------------------------------------------- trilinear
TrilinearSampler::TrilinearSampler(const FieldDomain& domain) : m_d(domain) {
    const auto* dims = m_d.dims();
    if (!dims || dims->nx < 2 || dims->ny < 2 || dims->nz < 2) return;
    const int cx = dims->nx - 1, cy = dims->ny - 1, cz = dims->nz - 1;
    m_cellActive.assign((size_t)cx * cy * cz, 0);
    auto vidx = [&](int i, int j, int k) { return (size_t)i + (size_t)j*dims->nx + (size_t)k*dims->nx*dims->ny; };
    auto vals = m_d.values();
    for (int k = 0; k < cz; ++k)
        for (int j = 0; j < cy; ++j)
            for (int i = 0; i < cx; ++i) {
                bool active = true;
                for (int dk = 0; dk <= 1 && active; ++dk)
                    for (int dj = 0; dj <= 1 && active; ++dj)
                        for (int di = 0; di <= 1 && active; ++di)
                            if (glm::dot(vals[vidx(i+di,j+dj,k+dk)], vals[vidx(i+di,j+dj,k+dk)]) == 0.0f)
                                active = false;
                m_cellActive[(size_t)i + (size_t)j*cx + (size_t)k*cx*cy] = active ? 1 : 0;
            }
}

bool TrilinearSampler::inside(const glm::vec3& p) const {
    const auto& b = m_d.bounds();
    if (p.x < b.min.x || p.x > b.max.x || p.y < b.min.y || p.y > b.max.y ||
        p.z < b.min.z || p.z > b.max.z) return false;
    const auto* dims = m_d.dims();
    if (!dims || m_cellActive.empty()) return true;
    auto locate = [](const std::vector<float>& axis, float v) {
        return std::clamp((int)(std::upper_bound(axis.begin(), axis.end(), v) - axis.begin()) - 1,
                          0, (int)axis.size() - 2);
    };
    int i = locate(m_d.axisX(), p.x), j = locate(m_d.axisY(), p.y), k = locate(m_d.axisZ(), p.z);
    int cx = dims->nx - 1, cy = dims->ny - 1;
    return m_cellActive[(size_t)i + (size_t)j*cx + (size_t)k*cx*cy] != 0;
}

glm::vec3 TrilinearSampler::sample(const glm::vec3& p) const {
    const auto* dims = m_d.dims();
    if (!dims) return {};
    // Bounds check only here (inside() adds the cell-activity test, which
    // the integrator checks separately); this keeps sample() to a single
    // set of axis locates instead of two.
    const auto& b = m_d.bounds();
    if (p.x < b.min.x || p.x > b.max.x || p.y < b.min.y || p.y > b.max.y ||
        p.z < b.min.z || p.z > b.max.z) return {};
    auto vals = m_d.values();
    auto idx = [&](int i, int j, int k) {
        return (size_t)i + (size_t)j*dims->nx + (size_t)k*dims->nx*dims->ny;
    };
    // Degenerate axis: nearest along that axis.
    if (dims->nx < 2 || dims->ny < 2 || dims->nz < 2) {
        auto nearIdx = [](const std::vector<float>& axis, float v) {
            return std::clamp((int)(std::upper_bound(axis.begin(), axis.end(), v) - axis.begin()) - 1,
                              0, (int)axis.size() - 1);
        };
        return vals[idx(nearIdx(m_d.axisX(), p.x), nearIdx(m_d.axisY(), p.y), nearIdx(m_d.axisZ(), p.z))];
    }
    auto locate = [](const std::vector<float>& axis, float v) {
        return std::clamp((int)(std::upper_bound(axis.begin(), axis.end(), v) - axis.begin()) - 1,
                          0, (int)axis.size() - 2);
    };
    int i0 = locate(m_d.axisX(), p.x), j0 = locate(m_d.axisY(), p.y), k0 = locate(m_d.axisZ(), p.z);
    float fx = (m_d.axisX()[i0+1] > m_d.axisX()[i0]) ? (p.x - m_d.axisX()[i0]) / (m_d.axisX()[i0+1] - m_d.axisX()[i0]) : 0.0f;
    float fy = (m_d.axisY()[j0+1] > m_d.axisY()[j0]) ? (p.y - m_d.axisY()[j0]) / (m_d.axisY()[j0+1] - m_d.axisY()[j0]) : 0.0f;
    float fz = (m_d.axisZ()[k0+1] > m_d.axisZ()[k0]) ? (p.z - m_d.axisZ()[k0]) / (m_d.axisZ()[k0+1] - m_d.axisZ()[k0]) : 0.0f;
    glm::vec3 c00 = glm::mix(vals[idx(i0,j0,k0)],   vals[idx(i0+1,j0,k0)],   fx);
    glm::vec3 c10 = glm::mix(vals[idx(i0,j0+1,k0)], vals[idx(i0+1,j0+1,k0)], fx);
    glm::vec3 c01 = glm::mix(vals[idx(i0,j0,k0+1)],   vals[idx(i0+1,j0,k0+1)],   fx);
    glm::vec3 c11 = glm::mix(vals[idx(i0,j0+1,k0+1)], vals[idx(i0+1,j0+1,k0+1)], fx);
    return glm::mix(glm::mix(c00, c10, fy), glm::mix(c01, c11, fy), fz);
}

// ------------------------------------------------------------------ nearest
NearestSampler::NearestSampler(const FieldDomain& domain)
    : m_d(domain)
    , m_grid(domain.positions(), /*cellSize*/ std::max(3.0f * domain.avgSpacing(), 1e-9f),
             domain.bounds().min, domain.bounds().max)
    , m_cutoffSq(9.0f * domain.avgSpacing() * domain.avgSpacing())
{}

bool NearestSampler::inside(const glm::vec3& p) const {
    const auto& b = m_d.bounds();
    return p.x >= b.min.x && p.x <= b.max.x && p.y >= b.min.y && p.y <= b.max.y &&
           p.z >= b.min.z && p.z <= b.max.z;
}

glm::vec3 NearestSampler::sample(const glm::vec3& p) const {
    int idx = m_grid.nearest(p, m_cutoffSq);
    if (idx < 0) return {};
    const auto& v = m_d.values()[idx];
    if (glm::dot(v, v) < 1e-12f) return {};
    return v;
}

std::unique_ptr<IFieldSampler> makeSampler(const FieldDomain& domain) {
    if (domain.dims()) return std::make_unique<TrilinearSampler>(domain);
    return std::make_unique<NearestSampler>(domain);
}

} // namespace streamline
