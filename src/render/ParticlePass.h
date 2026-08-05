#pragma once

#include <glad/gl.h>
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
              StreamlineSet& streamlines,
              const ColormapManager& colormap);
    void shutdown();
    void resetCount() { m_particleVertexCount = 0; }

    bool hasProgram() const { return particleProgram != 0; }

private:
    GLuint particleProgram = 0;
    GLuint particleVao = 0;
    GLuint particleVbo = 0;
    int m_particleVertexCount = 0;
    GLint particleColorLoc = -1;
    GLint particleLutLoc = -1;
    GLint particlePointSizeLoc = -1;
    GLint particleUseColormapLoc = -1;
    GLint particleMagRangeLoc = -1;
};
