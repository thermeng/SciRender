#include "render/BBoxOverlay.h"
#include "render/shader_utils.h"
#include "render/renderer.h"
#include <glm/gtc/type_ptr.hpp>

void BBoxOverlay::init(const ShaderSources& sources) {
    if (sources.bboxVert.empty() || sources.bboxFrag.empty()) return;

    m_program = compileProgram(sources.bboxVert.c_str(), sources.bboxFrag.c_str(), "BBox");
    if (m_program != 0) {
        m_mvpLoc = glGetUniformLocation(m_program, "uMVP");
        m_colorLoc = glGetUniformLocation(m_program, "uColor");
    }
}

void BBoxOverlay::draw(const RenderRenderState& state, const glm::mat4& view, const glm::mat4& proj, bool hasMeshes) {
    if (!state.showBounds || m_program == 0) return;
    if (!hasMeshes) return;

    // 12 edges of a unit cube centered at origin, coords -0.5..0.5
    static const float c[24 * 3] = {
        -0.5f,-0.5f,-0.5f,  0.5f,-0.5f,-0.5f,
         0.5f,-0.5f,-0.5f,  0.5f, 0.5f,-0.5f,
         0.5f, 0.5f,-0.5f, -0.5f, 0.5f,-0.5f,
        -0.5f, 0.5f,-0.5f, -0.5f,-0.5f,-0.5f,
        -0.5f,-0.5f, 0.5f,  0.5f,-0.5f, 0.5f,
         0.5f,-0.5f, 0.5f,  0.5f, 0.5f, 0.5f,
         0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f,
        -0.5f, 0.5f, 0.5f, -0.5f,-0.5f, 0.5f,
        -0.5f,-0.5f,-0.5f, -0.5f,-0.5f, 0.5f,
         0.5f,-0.5f,-0.5f,  0.5f,-0.5f, 0.5f,
         0.5f, 0.5f,-0.5f,  0.5f, 0.5f, 0.5f,
        -0.5f, 0.5f,-0.5f, -0.5f, 0.5f, 0.5f
    };
    if (m_vao == 0) {
        glCreateVertexArrays(1, &m_vao);
        glCreateBuffers(1, &m_vbo);
        glEnableVertexArrayAttrib(m_vao, 0);
        glVertexArrayAttribFormat(m_vao, 0, 3, GL_FLOAT, GL_FALSE, 0);
        glVertexArrayAttribBinding(m_vao, 0, 0);
        glNamedBufferData(m_vbo, sizeof(c), c, GL_STATIC_DRAW);
        glVertexArrayVertexBuffer(m_vao, 0, m_vbo, 0, 3 * sizeof(float));
    }

    glm::vec3 center(static_cast<float>(state.worldCenterX),
                     static_cast<float>(state.worldCenterY),
                     static_cast<float>(state.worldCenterZ));
    glm::vec3 diag(static_cast<float>(state.worldMaxX - state.worldMinX),
                   static_cast<float>(state.worldMaxY - state.worldMinY),
                   static_cast<float>(state.worldMaxZ - state.worldMinZ));
    glm::mat4 model = glm::translate(glm::mat4(1.0f), center) *
                      glm::scale(glm::mat4(1.0f), diag);
    glm::mat4 mvp = proj * view * model;

    GLboolean depthWas = glIsEnabled(GL_DEPTH_TEST);
    glDisable(GL_DEPTH_TEST);
    glLineWidth(1.0f);

    glUseProgram(m_program);
    glUniformMatrix4fv(m_mvpLoc, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniform4f(m_colorLoc,
                state.meshColor[0],
                state.meshColor[1],
                state.meshColor[2],
                1.0f);

    glBindVertexArray(m_vao);
    glDrawArrays(GL_LINES, 0, 24);
    glBindVertexArray(0);

    if (depthWas) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glUseProgram(0);
}

void BBoxOverlay::shutdown() {
    if (m_program) { glDeleteProgram(m_program); m_program = 0; }
    if (m_vao) { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
    if (m_vbo) { glDeleteBuffers(1, &m_vbo); m_vbo = 0; }
}
