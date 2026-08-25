#pragma once
#include "core/mesh_loader.h"
#include <string>
#include <vector>
#include <algorithm>

// Deep module behind Mesh data model seam.
// Centralizes field name → data resolution that was duplicated across
// RenderSettings, VectorGlyphSet, StreamlineSet, Renderer.
// One interface, N consumers. Hides point vs cell, extrapolation, clamping.
namespace FieldResolver {

// Scalar
std::string resolveActiveScalar(const RenderMesh& mesh, const std::string& requested);
const std::vector<float>* scalarData(const RenderMesh& mesh, const std::string& name, float& outMin, float& outMax);
bool hasScalar(const RenderMesh& mesh);
std::vector<std::string> derivedScalarNames(const RenderMesh& mesh);
std::vector<std::string> availableScalarNamesWithDerived(const RenderMesh& mesh);

// Vector (point vs cell)
struct VectorField {
    const glm::vec3* data = nullptr;
    size_t count = 0;
    bool isCell = false; // true → cellVectors + cellCenters placement
};
VectorField resolveVector(const RenderMesh& mesh, const std::string& requested, int placement);
std::string resolveVectorName(const RenderMesh& mesh, const std::string& requested, int placement);
std::vector<std::string> availableVectorNames(const RenderMesh& mesh);

// Colormap-range bookkeeping for animation playback. Whole-sequence mode holds
// a union over seen frames so colors/colorbar don't flicker; per-frame mode
// passes each frame's own extent through. Pure value type so the reseed/union
// rules are unit-testable headlessly.
struct AnimRangeState {
    bool global = true;
    std::string field;      // field name the accumulated range belongs to
    float rangeMin = 0.0f;
    float rangeMax = 1.0f;

    // frameField/frameMin/frameMax describe THIS frame's displayed field.
    // firstFrame: the sequence just (re)started. Effective colormap range is
    // returned via outMin/outMax.
    void advance(bool firstFrame, const std::string& frameField,
                 float frameMin, float frameMax,
                 float& outMin, float& outMax) {
        if (!global) { outMin = frameMin; outMax = frameMax; return; }
        if (firstFrame || field != frameField) {
            rangeMin = frameMin;
            rangeMax = frameMax;
        } else {
            rangeMin = std::min(rangeMin, frameMin);
            rangeMax = std::max(rangeMax, frameMax);
        }
        field = frameField;
        outMin = rangeMin;
        outMax = rangeMax;
    }

    // Force a reseed on the next advance() (mode switch).
    void invalidate() { field.clear(); }
};

} // namespace FieldResolver
