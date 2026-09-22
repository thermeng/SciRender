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
    static constexpr int kMaxPeelLayers = 64;

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

    bool hasProgram() const { return (m_peelProgram.has() || m_peelClipProgram.has()) && (m_compositeProgram.has() || m_blitProgram.has()); }

private:
    void ensurePeelFbos(int w, int h, int samples, int needLayers);
    void destroyPeelFbos();

    GlProgram m_peelProgram;
    GlProgram m_peelClipProgram;
    GlProgram m_compositeProgram;
    GlProgram m_blitProgram; // single-layer iterative composite for >32 layers to avoid sampler limit
    GLint m_peelPrevDepthLoc = -1;
    GLint m_peelClipPrevDepthLoc = -1;
    GLint m_peelLayerLoc = -1;
    GLint m_peelClipLayerLoc = -1;
    GLint m_peelLutLoc = -1;
    GLint m_peelClipLutLoc = -1;
    GLint m_peelNumBandsLoc = -1;
    GLint m_peelClipNumBandsLoc = -1;
    GLint m_peelUboIndex = -1;
    GLint m_peelClipUboIndex = -1;
    GLint m_peelNumLayersLoc = -1;
    GLint m_compositeLayersLoc = -1;
    GLint m_blitLayerLoc = -1;
    int m_maxTextureUnits = 0; // queried at init

    GlFramebuffer m_peelFbo[kMaxPeelLayers];
    GlTexture m_peelColorTex[kMaxPeelLayers];
    GlTexture m_peelDepthTex[kMaxPeelLayers];
    GlTexture m_peelMainDepth;
    GlVao m_peelDummyVao;
    int m_peelFboW = 0, m_peelFboH = 0, m_peelSamples = 0;
    int m_peelAllocated = 0;
};
