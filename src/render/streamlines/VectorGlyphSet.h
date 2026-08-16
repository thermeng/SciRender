#pragma once

#include "render/foundation/gl_raii.h"

#include <string>
#include <vector>
#include <map>

struct RenderMesh;

class VectorGlyphSet {
public:
    VectorGlyphSet() = default;
    ~VectorGlyphSet() = default;

    GlVao vao;
    GlBuffer vbo;
    GlBuffer nbo;
    GlBuffer ebo;
    GlBuffer instVBO;
    int glyphIndexCount = 0;
    int instanceCount = 0;

    float magMin = 0.0f;
    float magMax = 1.0f;
    float meshExtent = 1.0f;

    bool empty() const { return instanceCount == 0; }

    void rebuild(const RenderMesh& mesh, int stride, const std::string& fieldName = "", int magTransform = 0, int placement = 0);
    void shutdown();

private:
    void teardownGL();
};

// Builds a unit arrow (local space, arrow along +Y, height 1) into the supplied
// vertex/normal/index arrays. Free function so it can be reused/tested.
void buildUnitArrow(std::vector<float>& verts,
                    std::vector<float>& norms,
                    std::vector<unsigned int>& idx);


