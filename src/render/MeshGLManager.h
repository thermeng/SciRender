#pragma once

#include <glad/glad.h>

#include <vector>
#include <mutex>
#include <atomic>

struct RenderMesh;

// GPU mesh handle bundle produced by buildMeshGL().
struct Mesh {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint nbo = 0;
    GLuint ebo = 0;
    GLuint sbo = 0;
    int indexCount = 0;
};

// Owns the full-resolution and decimated (LOD) GPU meshes plus the meshChanged
// flag consumed by the render thread. Handles GL upload, the vertex-clustering
// decimation, and thread-safe teardown. All GL handles are owned here.
class MeshGLManager {
public:
    MeshGLManager() = default;
    ~MeshGLManager() = default;

    // Builds GPU meshes from a CPU RenderMesh (full + optional decimated LOD),
    // wiping the previous handles first. Guarded by the internal mutex so it
    // cannot race with clear() on another thread.
    void upload(const RenderMesh& renderMesh);

    // Re-uploads ONLY the per-vertex scalar buffer (sbo) for the already-built
    // meshes. Used when the active scalar field changes so we avoid re-uploading
    // the (potentially huge) vertex/normal/index arrays. If the new scalar array
    // is empty, the sbo is detached. Mutex-guarded to avoid racing clear()/
    // snapshotDrawList on other threads.
    void updateScalars(const std::vector<float>& scalars);

    // Frees all GPU handles and clears both mesh lists. Mutex-guarded.
    void clear();

    // Snapshots the draw-list (vao + indexCount) under the mutex so the caller
    // can iterate without the vector being mutated mid-draw. `useLod` +
    // `cameraMoving` select the decimated set while the camera is in motion.
    void snapshotDrawList(std::vector<std::pair<GLuint, int>>& out,
                          bool useLod, bool cameraMoving) const;

    bool hasMeshes() const { return !meshes_.empty(); }
    bool hasDecimated() const { return hasDecimated_; }

    // GPU-upload-due flag, set by the loader and consumed by the render thread.
    std::atomic<bool> meshChanged{true};

private:
    void buildMeshGL(const RenderMesh& renderMesh, std::vector<Mesh>& out);
    void destroyMesh(Mesh& mesh);
    // Coarse vertex-clustering decimation; empty result when not worthwhile.
    static RenderMesh decimate(const RenderMesh& in);

    std::vector<Mesh> meshes_;
    std::vector<Mesh> decimatedMeshes_;
    bool hasDecimated_ = false;
    mutable std::mutex mutex_; // guards GPU-handle teardown/uploads across threads
};
