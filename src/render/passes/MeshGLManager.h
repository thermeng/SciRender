#pragma once

#include "render/foundation/gl_raii.h"

#include <glad/gl.h>

#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <memory>

#include "core/mesh_loader.h"

struct Mesh {
    GlVao vao;
    GlBuffer vbo;
    GlBuffer nbo;
    GlBuffer ebo;
    GlBuffer sbo;
    GlVao edgeVao;   // separate VAO sharing VBO/NBO, bound to edge EBO for GL_LINES
    GlBuffer edgeEbo;
    int indexCount = 0;
    int vertexCount = 0;
    int edgeCount = 0;  // number of indices in edgeEbo (edge pairs for GL_LINES)

    Mesh() = default;
    Mesh(Mesh&&) = default;
    Mesh& operator=(Mesh&&) = default;
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
};

// Owns the full-resolution and decimated (LOD) GPU meshes plus the meshChanged
// flag consumed by the render thread. Handles GL upload, the vertex-clustering
// decimation, and thread-safe teardown. All GL handles are owned here.
class MeshGLManager {
public:
    MeshGLManager() = default;
    ~MeshGLManager() = default;

    // Builds GPU meshes from a shared, immutable CPU RenderMesh (full +
    // optional decimated LOD), wiping the previous handles first. The shared_ptr
    // is stored (NOT copied) so only ONE heavy CPU copy of the geometry exists.
    // Guarded by the internal mutex so it cannot race with clear() on another
    // thread.
    void upload(std::shared_ptr<const RenderMesh> renderMesh);

    // Re-uploads ONLY the per-vertex scalar buffer (sbo) for the already-built
    // meshes. Used when the active scalar field changes so we avoid re-uploading
    // the (potentially huge) vertex/normal/index arrays. The scalar payload is
    // handed off as a shared_ptr (zero-copy, exactly like the mesh pipeline) —
    // no deep vector copy on the GUI or render thread. On the GPU we orphan the
    // previous sbo (glBufferData with nullptr) before filling it, so the driver
    // can stream the new data into fresh memory instead of stalling on a
    // reallocation. If the payload is null/empty, the sbo is detached.
    // Mutex-guarded to avoid racing clear()/snapshotDrawList on other threads.
    void updateScalars(std::shared_ptr<const std::vector<float>> scalars);

    // Frees all GPU handles and clears both mesh lists. Mutex-guarded.
    void clear();

    // Isosurface (marching-cubes output) mesh slot. The isosurface is a derived
    // triangle mesh handed to the same buildMeshGL() path, so it gets VAO/VBO/
    // NBO/EBO/SBO + optional LOD decimation for free -- it is then appended to
    // the MeshPass draw list independently of showSurface.
    void uploadIsosurface(std::shared_ptr<const RenderMesh> isoMesh);
    void snapshotIsosurfaceDrawList(std::vector<std::pair<GLuint, int>>& out,
                                    bool useLod, bool cameraMoving,
                                    std::vector<int>& outVerts) const;

    // Snapshots the draw-list under the mutex so the caller can iterate without
    // the vector being mutated mid-draw. `useLod` + `cameraMoving` select the
    // decimated set while the camera is in motion. Each entry is (vao, drawCount)
    // where drawCount is the index count for triangle meshes or the vertex count
    // for point meshes; `outMode` carries 0=indexed / 1=points per entry;
    // `outVerts` carries the raw vertex count per entry for GL_POINTS draws.
    // Snapshots the draw-list under the mutex so the caller can iterate without
    // the vector being mutated mid-draw. `useLod` + `cameraMoving` select the
    // decimated set while the camera is in motion. Each entry is (vao, drawCount)
    // where drawCount is the index count for triangle meshes or the vertex count
    // for point meshes; `outMode` carries 0=indexed / 1=points per entry;
    // `outVerts` carries the raw vertex count per entry for GL_POINTS draws.
    // `outEdges` (optional) receives (edgeVao, edgeCount) pairs for cell-boundary
    // wireframe; decimated LOD meshes skip edges (edges live only on full-res mesh).
    void snapshotDrawList(std::vector<std::pair<GLuint, int>>& out,
                           bool useLod, bool cameraMoving,
                           std::vector<int>& outVerts,
                           std::vector<std::pair<GLuint, int>>* outEdges = nullptr) const;

    bool hasMeshes() const { return !meshes_.empty(); }
    bool hasDecimated() const { return hasDecimated_; }
    bool hasFullSource() const { return hasFullSource_; }
    bool hasIsosurfaceMeshes() const { return !isosurfaceMeshes_.empty(); }

    const RenderMesh* getFullSource() const { return fullSource_.get(); }

    // GPU compute shader LOD helpers
    void setComputeShaderSources(const std::string& accumSrc, const std::string& outputSrc, const std::string& trisSrc);
    void initLodCompute(const std::string& accumSrc, const std::string& outputSrc, const std::string& trisSrc);
    void cleanupLodCompute();
    bool dispatchLodCompute(const RenderMesh& mesh, Mesh& outMesh);
    const std::string& lastLodError() const { return lastLodError_; }

    // Replace the first decimated mesh with a new one (used after compute LOD).
    // Locking variant for callers outside meshManager.mutex_.
    void replaceDecimatedMesh(int index, Mesh newMesh) {
        std::lock_guard<std::mutex> lock(mutex_);
        replaceDecimatedMeshLocked(index, std::move(newMesh));
        decimatedFromGpu_ = true;
    }

    // GPU-upload-due flag, set by the loader and consumed by the render thread.
    std::atomic<bool> meshChanged{true};

private:
    void buildMeshGL(const RenderMesh& renderMesh, std::vector<Mesh>& out);
    // Unlocked variant — callers already holding mutex_ (updateScalars).
    void replaceDecimatedMeshLocked(int index, Mesh newMesh) {
        if (index < 0 || index >= static_cast<int>(decimatedMeshes_.size())) return;
        decimatedMeshes_[index] = std::move(newMesh);
    }
    // Coarse vertex-clustering decimation; empty result when not worthwhile.
    static RenderMesh decimate(const RenderMesh& in);

    // Re-derives the per-vertex scalar array of the decimated LOD mesh by
    // averaging the full-resolution scalars using the SAME clustering that
    // decimate() used for the geometry. Returns an empty vector when the LOD
    // mesh is absent or the geometry mismatch prevents a safe downsample.
    std::vector<float> decimateScalars(const std::vector<float>& fullScalars) const;

    // Cached source of truth for the full-resolution mesh. Needed so a scalar-
    // only field switch can recompute the decimated LOD scalars without a full
    // (expensive) re-upload of every vertex/normal/index buffer. Stored as a
    // shared_ptr (NOT a copy) so only one heavy CPU copy of the geometry exists.
    std::shared_ptr<const RenderMesh> fullSource_;
    bool hasFullSource_ = false;
    // Logs once when a scalar payload matches neither the post-split vertex
    // count nor the pre-split source-node count (caller contract violation).
    bool warnedScalarSpaceMismatch_ = false;

     std::vector<Mesh> meshes_;
    std::vector<Mesh> decimatedMeshes_;
    bool hasDecimated_ = false;
    // True while decimatedMeshes_[0] came from the GPU compute pass — its
    // vertex order cannot be reproduced by the CPU clusterer, so a scalar-only
    // re-upload must rebuild it (see updateScalars).
    bool decimatedFromGpu_ = false;

    // Isosurface (marching-cubes) GPU meshes. Mirrors meshes_/decimatedMeshes_
    // but kept separate so isosurface visibility is independent of showSurface.
    std::vector<Mesh> isosurfaceMeshes_;
    std::vector<Mesh> isosurfaceDecimatedMeshes_;
    bool hasIsoDecimated_ = false;

    mutable std::mutex mutex_;

    // PBO double-buffer for scalar SBO streaming (Phase 2.1)
    GlBuffer scalarPbo_[2];
    int scalarPboIndex_ = 0;

    // Compute shader LOD (GPU-side vertex clustering)
    GlProgram lodProgramAccum;
    GlProgram lodProgramOutput;
    GlProgram lodProgramTris;
    GlBuffer lodCellSsbo;
    GlBuffer lodRemapSsbo;
    GlBuffer lodParamsUbo;
    GlBuffer lodCounterSsbo;
    int lodCellsPerAxis = 0;
    bool lodGpuDecimationReady = false;
    std::string lodAccumSrc_;
    std::string lodOutputSrc_;
    std::string lodTrisSrc_;
    std::string lastLodError_;
};


