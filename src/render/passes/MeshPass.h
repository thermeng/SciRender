#pragma once

#include "render/foundation/gl_raii.h"

#include <glm/glm.hpp>

#include <vector>
#include <utility>

struct RenderRenderState;
struct ShaderSources;
struct MeshUBOData;
class MeshGLManager;
class ColormapManager;

struct MeshPassResult {
    std::vector<std::pair<GLuint, int>> transparentMeshes;
};

class MeshPass {
public:
    void init(const ShaderSources& sources);
    MeshPassResult draw(const RenderRenderState& state,
                         const glm::mat4& view,
                         const glm::mat4& proj,
                         const glm::mat4& model,
                         const std::vector<std::pair<GLuint, int>>& drawList,
                         const std::vector<int>& drawVerts,
                         const MeshGLManager& meshManager,
                         const ColormapManager& colormap);
    void shutdown();

    bool hasProgram() const { return shaderProgram.has(); }
    GLuint uboHandle() const { return meshUbo; }

private:
    void drawOpaque(const RenderRenderState& state,
                    const std::vector<std::pair<GLuint, int>>& drawList);
    void drawOverlays(const RenderRenderState& state,
                       MeshUBOData& ubo,
                       const std::vector<std::pair<GLuint, int>>& drawList,
                       const std::vector<int>& drawVerts,
                       const MeshGLManager& meshManager);

    GlProgram shaderProgram;
    GlProgram clipShaderProgram;
    GlBuffer meshUbo;
    GLuint meshUboIndex = ~0u;
    GLint lutTextureLoc = -1;
    GLint clipLutTextureLoc = -1;
};


