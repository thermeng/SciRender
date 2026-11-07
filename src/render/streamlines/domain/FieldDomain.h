#pragma once
// Validated, owned field domain (plan §4.2). Replaces the legacy
// pointer+count quadruple (data/count/verts/vertCount/posStorage)
// whose lifetime and coherence were never checked in one place.
#include <glm/glm.hpp>
#include <cstddef>
#include <optional>
#include <optional>
#include <string>
#include <vector>

namespace streamline {

struct StructuredDims { int nx = 0, ny = 0, nz = 0; };
struct BoundingBox {
    glm::vec3 min{}, max{};
    float extent = 1.0f;
};

enum class DomainError { Empty, SizeMismatch, NonFinite, DegenerateBounds,
                         IncoherentGrid, TooLarge };

class FieldDomain {
public:
    static std::optional<FieldDomain> build(
        std::vector<glm::vec3> positions,
        std::vector<glm::vec3> values,
        std::optional<StructuredDims> dims = std::nullopt);

    const std::vector<glm::vec3>& positions() const { return m_positions; }
    const std::vector<glm::vec3>& values()    const { return m_values; }
    const BoundingBox&            bounds()    const { return m_bounds; }
    const StructuredDims*         dims()      const { return m_dims ? &*m_dims : nullptr; }
    float                         avgSpacing() const { return m_avgSpacing; }
    size_t                        size()      const { return m_positions.size(); }

    // Sorted unique axis coordinates for structured grids (derived and
    // coherence-checked in build()). Empty for unstructured domains.
    const std::vector<float>& axisX() const { return m_axisX; }
    const std::vector<float>& axisY() const { return m_axisY; }
    const std::vector<float>& axisZ() const { return m_axisZ; }

private:
    std::vector<glm::vec3> m_positions, m_values;
    BoundingBox m_bounds{};
    std::optional<StructuredDims> m_dims;
    std::vector<float> m_axisX, m_axisY, m_axisZ;
    float m_avgSpacing = 1.0f;
};

inline const char* toString(DomainError e) {
    switch (e) {
        case DomainError::Empty:            return "empty field";
        case DomainError::SizeMismatch:     return "positions/values size mismatch";
        case DomainError::NonFinite:        return "non-finite position or value";
        case DomainError::DegenerateBounds: return "degenerate bounds";
        case DomainError::IncoherentGrid:   return "structured grid index/position mismatch";
        case DomainError::TooLarge:         return "field exceeds size budget";
    }
    return "unknown domain error";
}

} // namespace streamline
