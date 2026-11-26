#pragma once

#include "render/foundation/gl_raii.h"
#include <glm/glm.hpp>
#include <vector>
#include <utility>
#include <string>

struct RenderRenderState;
struct ShaderSources;
class MeshGLManager;
class ColormapManager;

class SurfaceLicPass {
public:
    struct Resources {
        GLuint vectorTex = 0;
        glm::vec3 boxMin{0.0f};
        glm::vec3 boxMax{0.0f};
        int texDimX = 0;
        int texDimY = 0;
        int texDimZ = 0;
        float maxMagnitude = 0.0f;
        float robustFloor = 0.0f;
        bool valid() const { return vectorTex != 0; }
    };

    void init(const ShaderSources& sources);
    void shutdown();
    bool hasProgram() const { return m_flowProgram.has() && m_convolveProgram.has(); }

    // Runs flow+convolution and returns LIC intensity texture (R16F, negative=invalid) or 0.
    GLuint render(const RenderRenderState& state,
                  const std::vector<std::pair<GLuint, int>>& drawList,
                  const MeshGLManager& meshManager,
                  GLuint meshUbo,
                  GLuint defaultFbo,
                  const Resources& res);

private:
    void ensureTargets(int w, int h);
    void ensureNoise(int grain);
    void drawFullscreen();

    GlProgram m_flowProgram;
    GlProgram m_convolveProgram;
    GlFramebuffer m_flowFbo;
    GlTexture m_flowTex;      // RGBA16F
    GlTexture m_flowDepth;    // depth24
    GlFramebuffer m_licFbo;
    GlTexture m_licTex;       // R16F
    GlTexture m_noiseTex;     // R8 2D
    GlVao m_dummyVao;

    int m_w = 0, m_h = 0;
    int m_noiseGrain = 0;

    // flow uniforms
    GLint m_flowUboIndex = -1;
    GLint m_flowVectorTexLoc = -1;
    GLint m_flowBoxMinLoc = -1;
    GLint m_flowBoxMaxLoc = -1;
    GLint m_flowUvwScaleLoc = -1;
    GLint m_flowUvwOffsetLoc = -1;
    GLint m_flowMagThreshLoc = -1;
    GLint m_flowViewportLoc = -1;

    // convolve uniforms
    GLint m_convFlowTexLoc = -1;
    GLint m_convNoiseTexLoc = -1;
    GLint m_convTexelSizeLoc = -1;
    GLint m_convStepsLoc = -1;
    GLint m_convStepSizeLoc = -1;
    GLint m_convNoiseFreqLoc = -1;
    GLint m_convEnhancedLoc = -1;
};
