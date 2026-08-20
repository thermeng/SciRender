#include "render/overlays/BBoxOverlay.h"
#include "render/foundation/shader_utils.h"
#include "render/foundation/renderer.h"
#include <glm/gtc/type_ptr.hpp>

void BBoxOverlay::init(const ShaderSources& sources) {
    if (sources.bboxVert.empty() || sources.bboxFrag.empty()) return;

    m_program.reset(compileProgram(sources.bboxVert.c_str(), sources.bboxFrag.c_str(), "BBox"));
    if (m_program.has()) {
        m_mvpLoc = glGetUniformLocation(m_program, "uMVP");
        m_colorLoc = glGetUniformLocation(m_program, "uColor");
    }
}

void BBoxOverlay::draw(const RenderRenderState& state, const glm::mat4& view, const glm::mat4& proj, bool hasMeshes) {
    if (!state.showBounds || !m_program.has()) return;
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
    if (!m_vao.has()) {
        setupVertexBuffer(m_vao, m_vbo, c, sizeof(c), 3 * sizeof(float), { { 0, 3, 0 } }, GL_STATIC_DRAW);
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

    // Save engine state we mutate; restored automatically on scope exit.
    GLStateGuard guard;
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

    glUseProgram(0);
}

void BBoxOverlay::shutdown() {
    m_program.reset();
    m_vao.reset();
    m_vbo.reset();
}


