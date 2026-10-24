#pragma once
#include "core/mesh_loader.h"
#include "core/post_fields.h"
#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <glm/glm.hpp>

// Deep module behind Mesh data model seam.
// Single interface hides point vs cell, extrapolation, derived field synthesis, and range.
// Consumers use FieldKey + Placement instead of (string, int) placement plumbing.
//
// Post-processed fields: every resolver takes an optional ComputedSnapshot
// (default null = legacy behavior). When present, snapshot fields shadow mesh
// fields by name — the snapshot owns immutable shared payloads, so this is
// safe to call from the render thread.

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
    // Non-null for synthesized derived scalars: keeps the derived array alive for
    // as long as the caller holds this handle. Without it a raw `data` pointer
    // dangles as soon as the per-thread derived cache is refilled for another
    // mesh — which is every frame while a transient run streams.
    std::shared_ptr<const std::vector<float>> owner;
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
                                Placement placement = Placement::Vertex,
                                const postfields::ComputedSnapshot* snap = nullptr);

// Unified entry for surface rendering: prefers point-sized data for GPU upload
// (extrapolated copy when cell field + point copy exists) but returns range
// from authoritative placement-aware range map. Returns nullptr when not found.
const std::vector<float>* scalarData(const RenderMesh& mesh, const std::string& name,
                                     float& outMin, float& outMax,
                                     Placement placement = Placement::Vertex,
                                     const postfields::ComputedSnapshot* snap = nullptr);

// Volume/isosurface path: returns per-cell data when CellCenter requested (no extrapolation).
// Falls back to other placement when requested not found.
const std::vector<float>* volumeScalarData(const RenderMesh& mesh, const std::string& name,
                                           float& outMin, float& outMax,
                                           Placement placement = Placement::Vertex,
                                           const postfields::ComputedSnapshot* snap = nullptr);

// Typed overloads using FieldKey — preferred interface for new code.
ScalarField resolveScalar(const RenderMesh& mesh, const FieldKey& key,
                          const postfields::ComputedSnapshot* snap = nullptr);
ScalarField resolveScalarForVolume(const RenderMesh& mesh, const FieldKey& key,
                                   const postfields::ComputedSnapshot* snap = nullptr);

// Range primitive — single source for mn/mx logic (DRY).
Range computeRange(const std::vector<float>& values);
Range computeRange(const float* data, size_t count);

// ── Vector seam ───────────────────────────────────────────────────────────

VectorField resolveVector(const RenderMesh& mesh, const std::string& requested,
                          Placement placement,
                          const postfields::ComputedSnapshot* snap = nullptr);

std::string resolveVectorName(const RenderMesh& mesh, const std::string& requested,
                              Placement placement,
                              const postfields::ComputedSnapshot* snap = nullptr);

// ── Derived / discovery ──────────────────────────────────────────────────

std::vector<std::string> derivedScalarNames(const RenderMesh& mesh);
std::vector<std::string> availableScalarNamesWithDerived(const RenderMesh& mesh);
std::vector<std::string> availableVectorNames(const RenderMesh& mesh);
bool hasScalar(const RenderMesh& mesh);

// — Snapshot-aware discovery: single source for UI lists (probes, combos, histogram).
// Merges mesh fields + derived + snapshot scalars/vectors. New computed fields
// appear automatically without touching consumers.
std::vector<std::string> availableScalars(const RenderMesh& mesh,
                                          const postfields::ComputedSnapshot* snap = nullptr);
std::vector<std::string> availableVectors(const RenderMesh& mesh,
                                          const postfields::ComputedSnapshot* snap = nullptr);
bool hasAnyScalar(const RenderMesh& mesh,
                  const postfields::ComputedSnapshot* snap = nullptr);
bool hasScalarField(const RenderMesh& mesh, const std::string& name,
                    const postfields::ComputedSnapshot* snap = nullptr);

// ── Context — natural consumption handle ─────────────────────────────────
// Holds mesh + snapshot once per frame; consumers call ctx.scalar/vector
// without threading snap/placement boilerplate. Thread-local derived cache
// stays inside Context-free functions, but Context eliminates per-call snap
// plumbing and enables inventory via ctx.scalars().

struct Context {
    const RenderMesh* mesh = nullptr;
    const postfields::ComputedSnapshot* snap = nullptr;
    explicit Context(const RenderMesh& m, const postfields::ComputedSnapshot* s = nullptr)
        : mesh(&m), snap(s) {}
    explicit Context(const RenderMesh* m, const postfields::ComputedSnapshot* s = nullptr)
        : mesh(m), snap(s) {}
    bool valid() const { return mesh != nullptr; }
    ScalarField scalar(const FieldKey& key) const {
        return mesh ? resolveScalar(*mesh, key, snap) : ScalarField{};
    }
    ScalarField scalar(const std::string& name, Placement p = Placement::Vertex) const {
        return scalar(FieldKey{name, p});
    }
    ScalarField scalarForVolume(const FieldKey& key) const {
        return mesh ? resolveScalarForVolume(*mesh, key, snap) : ScalarField{};
    }
    VectorField vector(const std::string& name, Placement p) const {
        return mesh ? resolveVector(*mesh, name, p, snap) : VectorField{};
    }
    std::vector<std::string> scalars() const {
        return mesh ? availableScalars(*mesh, snap) : std::vector<std::string>{};
    }
    std::vector<std::string> vectors() const {
        return mesh ? availableVectors(*mesh, snap) : std::vector<std::string>{};
    }
    bool hasScalarData() const { return mesh ? hasAnyScalar(*mesh, snap) : false; }
};

// int <-> Placement helpers for RenderRenderState boundary (single place).
inline Placement toPlacement(int v) { return v == 1 ? Placement::CellCenter : Placement::Vertex; }
inline int fromPlacement(Placement p) { return p == Placement::CellCenter ? 1 : 0; }

// ── Cache ─────────────────────────────────────────────────────────────────

void clearCache();

// ── Helpers for consumers that need Range struct ─────────────────────────

inline Range rangeOf(const ScalarField& f) { return f.range; }

} // namespace FieldStore
