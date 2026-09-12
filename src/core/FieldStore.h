#pragma once
#include "core/mesh_loader.h"
#include <string>
#include <vector>
#include <optional>
#include <glm/glm.hpp>

// Deep module behind Mesh data model seam.
// Single interface hides point vs cell, extrapolation, derived field synthesis, and range.
// Consumers use FieldKey + Placement instead of (string, int) placement plumbing.

namespace FieldStore {

// ── Domain types ──────────────────────────────────────────────────────────

enum class Placement : int {
    Vertex = 0,      // point data
    CellCenter = 1,  // cell data
};

struct FieldKey {
    std::string name;
    Placement placement = Placement::Vertex;
};

struct Range {
    float min = 0.0f;
    float max = 1.0f;
};

struct ScalarField {
    const std::vector<float>* data = nullptr; // storage owned by RenderMesh or derived cache
    Range range{};
    bool isCell = false; // true when data came from cell storage (size == cellCount)
};

struct VectorField {
    const glm::vec3* data = nullptr;
    size_t count = 0;
    bool isCell = false;
};

// ── Scalar seam ───────────────────────────────────────────────────────────

// Resolve which scalar name actually exists, respecting placement preference.
// Returns empty when no field available.
std::string resolveActiveScalar(const RenderMesh& mesh, const std::string& requested,
                                Placement placement = Placement::Vertex);

// Unified entry for surface rendering: prefers point-sized data for GPU upload
// (extrapolated copy when cell field + point copy exists) but returns range
// from authoritative placement-aware range map. Returns nullptr when not found.
const std::vector<float>* scalarData(const RenderMesh& mesh, const std::string& name,
                                     float& outMin, float& outMax,
                                     Placement placement = Placement::Vertex);

// Volume/isosurface path: returns per-cell data when CellCenter requested (no extrapolation).
// Falls back to other placement when requested not found.
const std::vector<float>* volumeScalarData(const RenderMesh& mesh, const std::string& name,
                                           float& outMin, float& outMax,
                                           Placement placement = Placement::Vertex);

// Typed overloads using FieldKey — preferred interface for new code.
ScalarField resolveScalar(const RenderMesh& mesh, const FieldKey& key);
ScalarField resolveScalarForVolume(const RenderMesh& mesh, const FieldKey& key);

// Range primitive — single source for mn/mx logic (DRY).
Range computeRange(const std::vector<float>& values);
Range computeRange(const float* data, size_t count);

// ── Vector seam ───────────────────────────────────────────────────────────

VectorField resolveVector(const RenderMesh& mesh, const std::string& requested,
                          Placement placement);

std::string resolveVectorName(const RenderMesh& mesh, const std::string& requested,
                              Placement placement);

// ── Derived / discovery ──────────────────────────────────────────────────

std::vector<std::string> derivedScalarNames(const RenderMesh& mesh);
std::vector<std::string> availableScalarNamesWithDerived(const RenderMesh& mesh);
std::vector<std::string> availableVectorNames(const RenderMesh& mesh);
bool hasScalar(const RenderMesh& mesh);

// ── Cache ─────────────────────────────────────────────────────────────────

void clearCache();

// ── Helpers for consumers that need Range struct ─────────────────────────

inline Range rangeOf(const ScalarField& f) { return f.range; }

} // namespace FieldStore
