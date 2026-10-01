#pragma once
#include <glad/gl.h>
#include <glm/glm.hpp>
#include <vector>
#include <utility>
#include "render/foundation/gl_raii.h"

struct ShaderSources;
struct RenderRenderState;
class ColormapManager;

// Deep module for order-independent transparency. Owns peel shaders and the
// ping-pong FBOs; hides UBO/sampler coupling. Interface is viewport +
// maxPeelIterations + scalar LUT; no Renderer internals leak.
//
// Algorithm: dual depth peeling. Two surfaces resolve per geometry pass (next
// front peel + next back peel), and depth ranges live in RG32F colour
// attachments holding (-nearest, farthest) per pixel, accumulated with GL_MAX
// blending on negated near depth. No depth attachment is used anywhere.
//
// Consequence: the target count is CONSTANT. Single-layer peeling needed N
// colour+depth pairs (64 layers = 128 textures); this needs 5 textures total
// regardless of iteration count, so there is no VRAM cliff and no
// GL_MAX_TEXTURE_IMAGE_UNITS ceiling.
class DepthPeelPass {
public:
    // Maximum peel iterations. Each resolves up to 2 surfaces, so the value is
    // "passes", not "layers".
    static constexpr int kMaxPeelIterations = 64;

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

    bool hasProgram() const { return m_peelProgram.has() || m_peelClipProgram.has(); }

private:
    // Ping-pong slot. Depth range and front accumulator each need two slots;
    // back and back-temp accumulation need one each; opaque depth is shared.
    enum Slot { kA = 0, kB = 1, kCount = 2 };

    void ensureTargets(int w, int h);
    void destroyTargets();

    // Resolve the peel program's uniform locations into the right member set
    // (plain vs crinkle-clip variant).
    void queryUniforms(bool isClip);

    // Binds the single scratch FBO to the requested colour attachments.
    void attachTargets(const GLuint* textures, int count);
    void clearTargets(const GLuint* textures, int count);
    void drawFullscreen();

    void drawTranslucent(GLuint vao, int indexCount);

    GlProgram m_peelProgram;
    GlProgram m_peelClipProgram;
    GlProgram m_blitProgram;

    GLint m_peelOpaqueDepthLoc = -1;
    GLint m_peelDepthSourceLoc = -1;
    GLint m_peelFrontSourceLoc = -1;
    GLint m_peelLutLoc = -1;
    GLint m_peelNumBandsLoc = -1;
    GLint m_peelStageLoc = -1;
    GLint m_peelClipOpaqueDepthLoc = -1;
    GLint m_peelClipDepthSourceLoc = -1;
    GLint m_peelClipFrontSourceLoc = -1;
    GLint m_peelClipLutLoc = -1;
    GLint m_peelClipNumBandsLoc = -1;
    GLint m_peelClipStageLoc = -1;
    GLint m_blitSrcLoc = -1;
    GLint m_blitBackLoc = -1;
    GLint m_blitModeLoc = -1;
    GLint m_blitClearValueLoc = -1;

    // Shader stages, passed per draw via the uStage uniform rather than
    // tracked as state.
    enum Stage { kInitialize = 0, kPeeling = 1, kAlpha = 2 };

    // Fixed target set (see class comment).
    GlFramebuffer m_fbo;
    GlTexture m_depthRange[2];  // RG32F: (-nearest, farthest), ping-pong
    GlTexture m_front[2];       // RGBA8 front accumulator, alpha stores (1-a)
    GlTexture m_back;           // RGBA8 back accumulator (premultiplied)
    GlTexture m_backTemp;       // RGBA8 per-iteration back peel
    GlTexture m_opaqueDepth;    // single-sample copy of the opaque scene depth
    GlVao m_dummyVao;

    int m_width = 0, m_height = 0;
    GLuint m_queryId = 0;   // GL_SAMPLES_PASSED, drives automatic termination
};