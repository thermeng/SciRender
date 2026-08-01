#pragma once

#include <glad/gl.h>
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

    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    GLuint m_program = 0;
    GLuint m_ubo = 0;
    GLuint m_uboIndex = GL_INVALID_INDEX;
    double m_planeY = 0.0;
};
