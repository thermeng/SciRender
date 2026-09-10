#pragma once
#include "render/foundation/gl_raii.h"
#include <glm/glm.hpp>
struct RenderRenderState;
struct ShaderSources;
class ProbeOverlay{
public:
    void init(const ShaderSources& s);
    void draw(const RenderRenderState& st, const glm::mat4& view, const glm::mat4& proj);
    void shutdown();
private:
    GlProgram m_prog;
    GLint m_mvpLoc=-1, m_colorLoc=-1;
    GlVao m_vao;
    GlBuffer m_vbo;
    bool m_init=false;
};
