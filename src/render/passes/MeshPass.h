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

    struct LicResources {
        GLuint vectorTex = 0;
        GLuint noiseTex = 0;
        glm::vec3 boxMin{0.0f};
        glm::vec3 boxMax{0.0f};
        int texDimX = 0;
        int texDimY = 0;
        int texDimZ = 0;
        bool valid() const { return vectorTex != 0 && noiseTex != 0; }
    };
    MeshPassResult drawLic(const RenderRenderState& state,
                  const glm::mat4& view,
                  const glm::mat4& proj,
                  const glm::mat4& model,
                  const std::vector<std::pair<GLuint, int>>& drawList,
                  const std::vector<int>& drawVerts,
                  const std::vector<std::pair<GLuint, int>>& edgeDrawList,
                  const MeshGLManager& meshManager,
                  const ColormapManager& colormap,
                  const LicResources& licRes);

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
    void activateLicProgram(const RenderRenderState& state, const ColormapManager& colormap,
                            GLuint vectorTex, GLuint noiseTex,
                            const glm::vec3& boxMin, const glm::vec3& boxMax,
                            int texDimX, int texDimY, int texDimZ);
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
    GLint lutTextureLoc = -1;
    GLint clipLutTextureLoc = -1;
    GLint numBandsLoc = -1;
    GLint clipNumBandsLoc = -1;
    GLint licLutTextureLoc = -1;
    GLint licClipLutTextureLoc = -1;
    GLint licNumBandsLoc = -1;
    GLint licClipNumBandsLoc = -1;
    GLint licVectorTexLoc = -1;
    GLint licClipVectorTexLoc = -1;
    GLint licNoiseTexLoc = -1;
    GLint licClipNoiseTexLoc = -1;
    GLint licBoxMinLoc = -1;
    GLint licClipBoxMinLoc = -1;
    GLint licBoxMaxLoc = -1;
    GLint licClipBoxMaxLoc = -1;
    GLint licStepsLoc = -1;
    GLint licClipStepsLoc = -1;
    GLint licStepSizeLoc = -1;
    GLint licClipStepSizeLoc = -1;
    GLint licNoiseFreqLoc = -1;
    GLint licClipNoiseFreqLoc = -1;
    GLint licUvwScaleLoc = -1;
    GLint licClipUvwScaleLoc = -1;
    GLint licUvwOffsetLoc = -1;
    GLint licClipUvwOffsetLoc = -1;
    GLint licEnhancedLoc = -1;
    GLint licClipEnhancedLoc = -1;
};
