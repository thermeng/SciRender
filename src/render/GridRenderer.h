#pragma once

#include "render/gl_raii.h"
#include <glm/glm.hpp>

struct RenderRenderState;
struct ShaderSources;

class GridRenderer {
public:
    void init(const ShaderSources& sources);
    void draw(const RenderRenderState& state, const glm::mat4& view, const glm::mat4& proj);
    void shutdown();

private:
    void updateUbo(const RenderRenderState& state, const glm::mat4& view, const glm::mat4& proj);

    GlVao m_vao;
    GlBuffer m_vbo;
    GlProgram m_program;
    GlBuffer m_ubo;
    GLuint m_uboIndex = ~0u;
    double m_planeY = 0.0;
};
