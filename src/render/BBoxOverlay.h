#pragma once

#include "render/gl_raii.h"
#include <glm/glm.hpp>

struct RenderRenderState;
struct ShaderSources;

class BBoxOverlay {
public:
    void init(const ShaderSources& sources);
    void draw(const RenderRenderState& state, const glm::mat4& view, const glm::mat4& proj, bool hasMeshes);
    void shutdown();

private:
    GlProgram m_program;
    GLint m_mvpLoc = -1;
    GLint m_colorLoc = -1;
    GlVao m_vao;
    GlBuffer m_vbo;
};
