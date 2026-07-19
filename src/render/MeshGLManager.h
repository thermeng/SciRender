#pragma once

#include <glad/glad.h>

#include <vector>
#include <mutex>
#include <atomic>

#include "core/mesh_loader.h"

struct Mesh {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint nbo = 0;
    GLuint ebo = 0;
    GLuint sbo = 0;
    int indexCount = 0;
    int vertexCount = 0; // # vertices; draw count for point-cloud meshes
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

    // Snapshots the draw-list under the mutex so the caller can iterate without
    // the vector being mutated mid-draw. `useLod` + `cameraMoving` select the
    // decimated set while the camera is in motion. Each entry is (vao, drawCount)
    // where drawCount is the index count for triangle meshes or the vertex count
    // for point meshes; `outMode` carries 0=indexed / 1=points per entry;
    // `outVerts` carries the raw vertex count per entry for GL_POINTS draws.
    void snapshotDrawList(std::vector<std::pair<GLuint, int>>& out,
                          bool useLod, bool cameraMoving,
                          std::vector<int>& outMode,
                          std::vector<int>& outVerts) const;

    bool hasMeshes() const { return !meshes_.empty(); }
    bool hasDecimated() const { return hasDecimated_; }

    // GPU-upload-due flag, set by the loader and consumed by the render thread.
    std::atomic<bool> meshChanged{true};

private:
    void buildMeshGL(const RenderMesh& renderMesh, std::vector<Mesh>& out);
    void destroyMesh(Mesh& mesh);
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

    std::vector<Mesh> meshes_;
    std::vector<Mesh> decimatedMeshes_;
    bool hasDecimated_ = false;
    mutable std::mutex mutex_; // guards GPU-handle teardown/uploads across threads
};
