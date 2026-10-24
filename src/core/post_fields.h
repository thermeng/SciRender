#pragma once
// Post-processed vector calculus fields (vorticity, helicity, Q-criterion,
// pressure gradient) derived from velocity + pressure point data.
//
// Supports Cartesian structured (ImageData/RectilinearGrid, 2D & 3D) via weighted
// finite differences AND unstructured/polydata/curvilinear STRUCTURED_GRID
// via least-squares reconstruction (high-performance CFD meshes).
// Pure CPU, no Qt/GL — headlessly unit-testable. Storage for the computed
// arrays (ComputedSnapshot, snapshot-shared with the render thread) lives
// here too; resolution against it is Phase-2 FieldStore integration.

#include "core/mesh_loader.h"
#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace postfields {

// ── Cartesian grid description ─────────────────────────────────────────────
// Per-axis unique node coordinates recovered from the IJK-ordered vertices.
// Non-uniform (rectilinear) spacing is preserved — derivatives use true
// local spacing, never an assumed uniform dx.
struct CartesianGrid {
    int dX = 0, dY = 0, dZ = 0;
    std::vector<float> x; // size dX, strictly increasing
    std::vector<float> y; // size dY, strictly increasing
    std::vector<float> z; // size dZ, strictly increasing

    size_t nodeCount() const { return (size_t)dX * (size_t)dY * (size_t)dZ; }
    size_t index(int i, int j, int k) const {
        return (size_t)i + (size_t)j * (size_t)dX + (size_t)k * (size_t)dX * (size_t)dY;
    }
};

// Returns true + fills `out` when the mesh is a Cartesian structured grid:
// gridDim set, vertex count matches, and every node position equals the
// (x[i], y[j], z[k]) combination (rules out curvilinear STRUCTURED_GRID).
// Supports 2D slabs (one dim ==1) and 3D; unstructured / polydata (gridDim 0) fail closed.
// For 2D, vorticity is out-of-plane (ω_z) and derivatives in collapsed axis are 0.
bool extractCartesianGrid(const RenderMesh& mesh, CartesianGrid& out);
bool isCartesianGrid2D(const CartesianGrid& g);

// ── Field fetch (point data, v1) ────────────────────────────────────────────

struct PointVectorField {
    const glm::vec3* data = nullptr; // one vec3 per grid node, IJK order
    size_t count = 0;
};

// Point velocity vectors in mesh order. For structured, checks nodeCount==grid count;
// for unstructured, any point-sized field (vertices/3) is accepted (LSQ path).
bool extractPointVectors(const RenderMesh& mesh, const CartesianGrid& grid,
                         const std::string& name, PointVectorField& out);
bool extractPointVectorsUnstructured(const RenderMesh& mesh,
                                     const std::string& name, PointVectorField& out);

struct PointScalarField {
    const float* data = nullptr; // one float per grid node, IJK order
    size_t count = 0;
};

// Point pressure scalars in mesh order.
bool extractPointScalars(const RenderMesh& mesh, const CartesianGrid& grid,
                         const std::string& name, PointScalarField& out);
bool extractPointScalarsUnstructured(const RenderMesh& mesh,
                                     const std::string& name, PointScalarField& out);

// ── Outputs ─────────────────────────────────────────────────────────────────

struct PostFieldsResult {
    bool ok = false;
    std::string error;

    std::vector<glm::vec3> vorticity; // ω = ∇×u, per node
    std::vector<float> helicity;      // H = u·ω, per node
    std::vector<float> qCriterion;    // Q = ½(‖Ω‖²−‖S‖²), per node
    std::vector<glm::vec3> gradP;     // ∇p, per node

    // Which outputs were actually computed (subset of the requested set).
    bool hasVorticity = false;
    bool hasHelicity = false;
    bool hasQ = false;
    bool hasGradP = false;
};

struct PostFieldsRequest {
    const glm::vec3* velocity = nullptr; // required for ω/H/Q, node order
    const float* pressure = nullptr;     // required for ∇p, node order
    bool wantVorticity = true;
    bool wantHelicity = true;
    bool wantQ = true;
    bool wantGradP = true;
};

// Single fused O(N) pass over the grid: central differences inside,
// one-sided at domain boundaries, true local spacing per axis (structured).
PostFieldsResult computePostFields(const CartesianGrid& grid,
                                   const PostFieldsRequest& req);
// Unstructured/polydata LSQ path (any point count)
PostFieldsResult computePostFieldsUnstructured(const RenderMesh& mesh,
                                               const PostFieldsRequest& req);
// Auto-dispatch: structured if Cartesian extract succeeds, else unstructured LSQ
PostFieldsResult computePostFields(const RenderMesh& mesh,
                                   const PostFieldsRequest& req);

// ── Computed-field naming (source-encoded, collision-free) ─────────────────

std::string vorticityName(const std::string& velocityField);
std::string helicityName(const std::string& velocityField);
std::string qCriterionName(const std::string& velocityField);
std::string gradPName(const std::string& pressureField);

// ── Immutable snapshot shared with the render thread ───────────────────────
// Populated by Extract; handed to the renderer inside RenderRenderState.
// shared_ptr<const> payloads => safe to read from either thread; a
// re-extract simply publishes a new snapshot, never mutates a live one.
struct ComputedSnapshot {
    std::unordered_map<std::string, std::shared_ptr<const std::vector<glm::vec3>>> vectors;
    std::unordered_map<std::string, std::shared_ptr<const std::vector<float>>> scalars;
    std::unordered_map<std::string, std::pair<float,float>> scalarRanges;

    bool empty() const { return vectors.empty() && scalars.empty(); }
    void clear() { vectors.clear(); scalars.clear(); scalarRanges.clear(); }
};

// ── Thread-safe one-shot extraction ────────────────────────────────────────
// SnapshotRequest captures everything the extraction needs BY VALUE. Capture it
// on the GUI thread (it reads RenderSettings) and hand the copy to a worker:
// extractSnapshot() then touches no application state at all, so nothing has to
// be synchronised with the GUI thread while frames are streaming.
struct SnapshotRequest {
    std::string velocitySource;
    std::string pressureSource;
    bool wantVorticity = true;
    bool wantHelicity = true;
    bool wantQ = true;
    bool wantGradP = true;
    // When false the caller wants the previously published snapshot carried
    // forward instead of a new extraction (see SnapshotResult::keepExisting).
    bool autoReextract = true;

    bool needsVelocity() const { return wantVorticity || wantHelicity || wantQ; }
    bool needsPressure() const { return wantGradP; }
};

struct SnapshotResult {
    std::shared_ptr<const ComputedSnapshot> snapshot; // null on failure
    std::string status;                               // empty on success
    bool ok = false;
    // Auto re-extract is disabled: republish the previous snapshot untouched.
    bool keepExisting = false;
};

// Pure, reentrant, no globals, no Qt: safe to call from any thread, including
// concurrently with the GUI thread publishing a different frame.
SnapshotResult extractSnapshot(const RenderMesh& mesh, const SnapshotRequest& request);

} // namespace postfields
