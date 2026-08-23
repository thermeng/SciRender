#pragma once
#include "core/mesh_loader.h"
#include <string>
#include <vector>

// Deep module behind Mesh data model seam.
// Centralizes field name → data resolution that was duplicated across
// RenderSettings, VectorGlyphSet, StreamlineSet, Renderer.
// One interface, N consumers. Hides point vs cell, extrapolation, clamping.
namespace FieldResolver {

// Scalar
std::string resolveActiveScalar(const RenderMesh& mesh, const std::string& requested);
const std::vector<float>* scalarData(const RenderMesh& mesh, const std::string& name, float& outMin, float& outMax);
bool hasScalar(const RenderMesh& mesh);

// Vector (point vs cell)
struct VectorField {
    const glm::vec3* data = nullptr;
    size_t count = 0;
    bool isCell = false; // true → cellVectors + cellCenters placement
};
VectorField resolveVector(const RenderMesh& mesh, const std::string& requested, int placement);
std::string resolveVectorName(const RenderMesh& mesh, const std::string& requested, int placement);
std::vector<std::string> availableVectorNames(const RenderMesh& mesh);

} // namespace FieldResolver
