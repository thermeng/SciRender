#pragma once
#include "render/foundation/gl_raii.h"
#include <glm/glm.hpp>

struct RenderRenderState;
struct ShaderSources;

class LineProbeOverlay {
public:
    void init(const ShaderSources& sources);
    void draw(const RenderRenderState& state, const glm::mat4& view, const glm::mat4& proj);
    void shutdown();
private:
    GlProgram m_prog;
    GLint m_mvpLoc = -1;
    GLint m_colorLoc = -1;
    GlVao m_vaoLine, m_vaoPoints;
    GlBuffer m_vboLine, m_vboPoints;
    bool m_init = false;
};
