#pragma once
// Field sampling behind an interface (plan §4.5). The integrator only
// depends on IFieldSampler; trilinear vs nearest is chosen by the domain.
#include "render/streamlines/domain/FieldDomain.h"
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <vector>

namespace streamline {

// Fixed uniform grid: cell size is derived from the caller's cutoff so a
// 3x3x3 neighbourhood is provably sufficient (retires the old bug where
// cellSize = 2*spacing but cutoff = 3*spacing let the true nearest hide
// two cells away).
class UniformGrid {
public:
    UniformGrid(std::span<const glm::vec3> positions, float cellSize,
                glm::vec3 minB, glm::vec3 maxB);
    // Returns index of nearest point within maxDistSq, or -1.
    int nearest(const glm::vec3& p, float maxDistSq) const;
    float cellSize() const { return m_cellSize; }
private:
    glm::vec3 m_origin{};
    float m_cellSize = 1.0f;
    int m_nx = 1, m_ny = 1, m_nz = 1;
    std::vector<std::vector<int>> m_cells;
    std::span<const glm::vec3> m_positions;
};

class IFieldSampler {
public:
    virtual ~IFieldSampler() = default;
    [[nodiscard]] virtual glm::vec3 sample(const glm::vec3& p) const = 0;
    [[nodiscard]] virtual bool inside(const glm::vec3& p) const = 0;
};

class TrilinearSampler final : public IFieldSampler {
public:
    explicit TrilinearSampler(const FieldDomain& domain);
    glm::vec3 sample(const glm::vec3& p) const override;
    bool inside(const glm::vec3& p) const override;
private:
    const FieldDomain& m_d;
    std::vector<char> m_cellActive; // (nx-1)*(ny-1)*(nz-1), false if any corner is zero
};

class NearestSampler final : public IFieldSampler {
public:
    explicit NearestSampler(const FieldDomain& domain);
    glm::vec3 sample(const glm::vec3& p) const override;
    bool inside(const glm::vec3& p) const override;
private:
    const FieldDomain& m_d;
    UniformGrid m_grid;
    float m_cutoffSq = 0.0f;
};

std::unique_ptr<IFieldSampler> makeSampler(const FieldDomain& domain);

} // namespace streamline
