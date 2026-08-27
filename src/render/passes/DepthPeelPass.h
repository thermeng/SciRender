#pragma once
#include <glad/gl.h>
#include <glm/glm.hpp>
#include <vector>
#include <utility>
#include "render/foundation/gl_raii.h"

struct ShaderSources;
struct RenderRenderState;
class ColormapManager;

// Deep module for N-layer OIT. Owns peel FBOs/shaders, hides UBO/sampler coupling.
// Interface is viewport + maxPeelLayers + scalar LUT; no Renderer internals leak.
class DepthPeelPass {
public:
    static constexpr int kMaxPeelLayers = 8;

    void init(const ShaderSources& sources);
    void shutdown();
    void reinitForNewContext(); // zeros handles for lazy rebuild

    // Viewport override aware. Caller binds display FBO before call; this pass
    // composites back to that FBO. Uses meshUbo + colormap LUT internally.
    void renderTransparent(int vpW, int vpH,
                           const RenderRenderState& state,
                           GLuint meshUbo,
                           const ColormapManager& colormap,
                           const std::vector<std::pair<GLuint,int>>& transparentMeshes);

    bool hasProgram() const { return m_peelProgram.has() && m_compositeProgram.has(); }

private:
    void ensurePeelFbos(int w, int h, int samples);
    void destroyPeelFbos();

    GlProgram m_peelProgram;
    GlProgram m_compositeProgram;
    GLint m_peelPrevDepthLoc = -1;
    GLint m_peelLayerLoc = -1;
    GLint m_peelLutLoc = -1;
    GLint m_peelNumBandsLoc = -1;
    GLint m_peelNumLayersLoc = -1;

    GlFramebuffer m_peelFbo[kMaxPeelLayers];
    GlTexture m_peelColorTex[kMaxPeelLayers];
    GlTexture m_peelDepthTex[kMaxPeelLayers];
    GlTexture m_peelMainDepth;
    GlVao m_peelDummyVao;
    int m_peelFboW = 0, m_peelFboH = 0, m_peelSamples = 0;
};
