#pragma once

#include <glad/glad.h>

#include <string>
#include <vector>
#include <map>

struct RenderMesh;

// Owns the instanced arrow glyph GPU resources for VTK VECTORS fields. Builds a
// unit arrow + a per-instance [ox,oy,oz, dx,dy,dz] buffer from the active
// vector field of a cached RenderMesh, and tracks the sampled magnitude range
// (used for LUT normalization). All GL handles are owned here and freed in
// shutdown() (called with a current context).
class VectorGlyphSet {
public:
    VectorGlyphSet() = default;
    ~VectorGlyphSet() = default;

    GLuint vao = 0;
    GLuint vbo = 0;   // arrow vertex positions
    GLuint nbo = 0;   // arrow normals
    GLuint ebo = 0;
    GLuint instVBO = 0; // per-instance [ox,oy,oz, dx,dy,dz]
    int glyphIndexCount = 0;
    int instanceCount = 0;

    // Sampled magnitude range across the (strided) instance set. Defaulted so a
    // hidden colorbar never shows stale bounds before a rebuild.
    float magMin = 0.0f;
    float magMax = 1.0f;

    bool empty() const { return instanceCount == 0; }

    // Rebuilds the GPU glyph set from the active vector field of `mesh`.
    // `stride` subsamples points. Updates magMin/magMagMax. No-op (teardown
    // only) when the mesh has no vector data.
    void rebuild(const RenderMesh& mesh, int stride);

    // Deletes GL handles. Call only with a current GL context.
    void shutdown();

private:
    void teardownGL();
};

// Builds a unit arrow (local space, arrow along +Y, height 1) into the supplied
// vertex/normal/index arrays. Free function so it can be reused/tested.
void buildUnitArrow(std::vector<float>& verts,
                    std::vector<float>& norms,
                    std::vector<unsigned int>& idx);
