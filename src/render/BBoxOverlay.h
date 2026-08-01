#pragma once

#include <glad/gl.h>
#include <glm/glm.hpp>

struct RenderRenderState;
struct ShaderSources;

class BBoxOverlay {
public:
    void init(const ShaderSources& sources);
    void draw(const RenderRenderState& state, const glm::mat4& view, const glm::mat4& proj, bool hasMeshes);
    void shutdown();

private:
    GLuint m_program = 0;
    GLint m_mvpLoc = -1;
    GLint m_colorLoc = -1;
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
};
