#pragma once

#include <glad/gl.h>
#include <glm/glm.hpp>

#include <vector>
#include <utility>

struct RenderRenderState;
struct ShaderSources;
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

    bool hasProgram() const { return shaderProgram != 0; }
    GLuint uboHandle() const { return meshUbo; }

private:
    void drawOpaque(const RenderRenderState& state,
                    const std::vector<std::pair<GLuint, int>>& drawList);
    void drawOverlays(const RenderRenderState& state,
                      const std::vector<std::pair<GLuint, int>>& drawList,
                      const std::vector<int>& drawVerts,
                      const MeshGLManager& meshManager);

    GLuint shaderProgram = 0;
    GLuint meshUbo = 0;
    GLuint meshUboIndex = GL_INVALID_INDEX;
    GLint lutTextureLoc = -1;
};
