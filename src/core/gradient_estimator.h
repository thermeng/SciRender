#pragma once
// Deep module behind post_fields seam: gradient reconstruction for
// arbitrary meshes. Structured path uses weighted finite differences;
// unstructured path uses moving least-squares on 1-ring + spatial neighbors.

#include "core/mesh_loader.h"
#include <glm/glm.hpp>
#include <vector>

namespace gradient {

// 3x3 velocity gradient tensor + pressure gradient
struct GradTensors {
    // G = [ dudx dudy dudz
    //       dvdx dvdy dvdz
    //       dwdx dwdy dwdz ]
    float dudx=0,dudy=0,dudz=0;
    float dvdx=0,dvdy=0,dvdz=0;
    float dwdx=0,dwdy=0,dwdz=0;
    glm::vec3 gradP{0.0f};
};

// Least-squares gradient for unstructured / polydata meshes.
// Neighbors built from cell topology if present, else from triangle indices / radius search.
class LeastSquaresEstimator {
public:
    // Build once per mesh (captures positions, builds spatial index)
    explicit LeastSquaresEstimator(const RenderMesh& mesh);
    bool estimate(size_t pointId, const float* scalarField, glm::vec3& gradOut) const;
    bool estimate(size_t pointId, const glm::vec3* vectorField, GradTensors& g, const float* pressureField) const;
    // Batch for all points (post_fields convenience)
    void estimateAll(const glm::vec3* velocity, const float* pressure,
                     std::vector<GradTensors>& outTensors) const;
    size_t pointCount() const { return m_pointCount; }
private:
    const RenderMesh* m_mesh = nullptr;
    size_t m_pointCount = 0;
    std::vector<std::vector<uint32_t>> m_neighbors; // per-point neighbor ids (includes self-ring)
    // For split meshes (177 verts vs 125 logical points) store deduplicated positions
    // so LSQ dX is computed on logical points, not duplicated split verts (zero-dist).
    std::vector<float> m_logicalVerts; // size m_pointCount*3 when split, empty otherwise
    const float* vertData() const { return m_logicalVerts.empty() ? m_mesh->vertices.data() : m_logicalVerts.data(); }
    // Fallback spatial: uniform grid for radius search when topology absent
    bool buildNeighborsStructured();
    bool buildNeighborsFromTopology();
    bool buildNeighborsFromIndices();
    bool buildNeighborsFromRadius();
    static bool solveLeastSquares(const std::vector<glm::vec3>& dX,
                                  const std::vector<float>& dF,
                                  glm::vec3& grad);
};

} // namespace gradient
