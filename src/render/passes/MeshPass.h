#pragma once

#include "render/foundation/gl_raii.h"
#include "render/passes/WireframePass.h"

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
                         const std::vector<std::pair<GLuint, int>>& edgeDrawList,
                         const MeshGLManager& meshManager,
                         const ColormapManager& colormap);


    void drawOverlaysAfterTransparent(const RenderRenderState& state,
                                      const glm::mat4& view,
                                      const glm::mat4& proj,
                                      const glm::mat4& model,
                                      const std::vector<std::pair<GLuint, int>>& drawList,
                                      const std::vector<int>& drawVerts,
                                      const std::vector<std::pair<GLuint, int>>& edgeDrawList,
                                      const MeshGLManager& meshManager,
                                      const ColormapManager& colormap);
    void shutdown();

    bool hasProgram() const { return shaderProgram.has(); }
    bool hasLicProgram() const { return licShaderProgram.has(); }
    GLuint uboHandle() const { return meshUbo; }

    // Tells drawCommon whether DepthPeelPass can consume deferred sub-opaque
    // meshes. When false, sub-opaque surfaces are drawn inline with order-
    // dependent alpha blending instead of being deferred, so a peel shader
    // compile failure degrades quality rather than making the mesh vanish.
    void setOitAvailable(bool available) { m_oitAvailable = available; }

    MeshPassResult drawLic(const RenderRenderState& state,
                  const glm::mat4& view,
                  const glm::mat4& proj,
                  const glm::mat4& model,
                  const std::vector<std::pair<GLuint, int>>& drawList,
                  const std::vector<int>& drawVerts,
                  const std::vector<std::pair<GLuint, int>>& edgeDrawList,
                  const MeshGLManager& meshManager,
                  const ColormapManager& colormap,
                  GLuint licTex);

private:
    void drawOpaque(const RenderRenderState& state,
                    const std::vector<std::pair<GLuint, int>>& drawList);
    void drawOverlays(const RenderRenderState& state,
                       MeshUBOData& ubo,
                       const std::vector<std::pair<GLuint, int>>& drawList,
                       const std::vector<int>& drawVerts,
                       const std::vector<std::pair<GLuint, int>>& edgeDrawList,
                       const MeshGLManager& meshManager);
    void activateProgram(const RenderRenderState& state, const ColormapManager& colormap);
    void activateLicProgram(const RenderRenderState& state, const ColormapManager& colormap, GLuint licTex);
    MeshUBOData makeUbo(const RenderRenderState& state,
                         const glm::mat4& view,
                         const glm::mat4& proj,
                         const glm::mat4& model);




    template <typename Activator>
    MeshPassResult drawCommon(const RenderRenderState& state,
                              const glm::mat4& view,
                              const glm::mat4& proj,
                              const glm::mat4& model,
                              const std::vector<std::pair<GLuint, int>>& drawList,
                              const std::vector<int>& drawVerts,
                              const std::vector<std::pair<GLuint, int>>& edgeDrawList,
                              const MeshGLManager& meshManager,
                              const ColormapManager& colormap,
                              Activator activate,
                              bool licTransparent);

    GlProgram shaderProgram;
    GlProgram clipShaderProgram;
    GlProgram licShaderProgram;
    GlProgram licClipShaderProgram;
    WireframePass wireframePass;
    GlBuffer meshUbo;
    GLuint meshUboIndex = ~0u;
    GLuint licUboIndex = ~0u;
    GLuint licClipUboIndex = ~0u;
    const ColormapManager* m_colormapPtr = nullptr;
    bool m_oitAvailable = true;
    GLint lutTextureLoc = -1;
    GLint clipLutTextureLoc = -1;
    GLint numBandsLoc = -1;
    GLint clipNumBandsLoc = -1;
    GLint licLutTextureLoc = -1;
    GLint licClipLutTextureLoc = -1;
    GLint licNumBandsLoc = -1;
    GLint licClipNumBandsLoc = -1;
    GLint licTexLoc = -1;
    GLint licClipTexLoc = -1;
    GLint licOnlyLoc = -1;
    GLint licClipOnlyLoc = -1;
};
