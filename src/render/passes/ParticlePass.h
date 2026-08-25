#pragma once

#include "render/foundation/gl_raii.h"

#include <glm/glm.hpp>

#include <vector>

struct RenderRenderState;
struct ShaderSources;
class StreamlineSet;
class ColormapManager;

class ParticlePass {
public:
    void init(const ShaderSources& sources);
    void draw(const RenderRenderState& state,
              float frameDt,
              const glm::mat4& mvp,
              StreamlineSet& streamlines,
              const ColormapManager& colormap);
    void shutdown();
    void resetCount() { m_particleVertexCount = 0; }

private:
    GlProgram particleProgram;
    GlVao particleVao;
    GlBuffer particleVbo;
    int m_particleVertexCount = 0;
    GLint particleColorLoc = -1;
    GLint particleMvpLoc = -1;
    GLint particleSizeRefWLoc = -1;
    GLint particleLutLoc = -1;
    GLint particlePointSizeLoc = -1;
    GLint particleUseColormapLoc = -1;
    GLint particleMagRangeLoc = -1;
};


