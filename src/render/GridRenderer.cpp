#include "render/GridRenderer.h"
#include "render/shader_utils.h"
#include "render/renderer.h"
#include <glm/gtc/type_ptr.hpp>

void GridRenderer::init(const ShaderSources& sources) {
    if (sources.gridVert.empty() || sources.gridFrag.empty()) return;

    m_program = compileProgram(sources.gridVert.c_str(), sources.gridFrag.c_str(), "Grid");

    const float q[8] = { -1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f, 1.0f, 1.0f };
    glCreateVertexArrays(1, &m_vao);
    glCreateBuffers(1, &m_vbo);
    glEnableVertexArrayAttrib(m_vao, 0);
    glVertexArrayAttribFormat(m_vao, 0, 2, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(m_vao, 0, 0);
    glNamedBufferData(m_vbo, sizeof(q), q, GL_STATIC_DRAW);
    glVertexArrayVertexBuffer(m_vao, 0, m_vbo, 0, 2 * sizeof(float));
}

void GridRenderer::updateUbo(const RenderRenderState& state, const glm::mat4& view, const glm::mat4& proj) {
    if (m_program == 0) return;
    if (m_ubo == 0) {
        m_uboIndex = glGetUniformBlockIndex(m_program, "GridUBO");
        if (m_uboIndex != GL_INVALID_INDEX) {
            glUniformBlockBinding(m_program, m_uboIndex, 2);
            glCreateBuffers(1, &m_ubo);
            glNamedBufferData(m_ubo, sizeof(GridUBOData), nullptr, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_UNIFORM_BUFFER, 2, m_ubo);
        }
    }
    if (m_ubo == 0) return;
    GridUBOData ubo{};
    ubo.invView = glm::inverse(view);
    ubo.invProj = glm::inverse(proj);
    ubo.view = view;
    ubo.proj = proj;
    ubo.camPos_colorR = glm::vec4(glm::vec3(state.camera.position), 0.0f);
    float bgLum = 0.299f * state.bgColor[0] + 0.587f * state.bgColor[1] + 0.114f * state.bgColor[2];
    glm::vec3 gridCol = (bgLum > 0.5f) ? glm::vec3(0.18f, 0.18f, 0.20f) : glm::vec3(0.78f, 0.78f, 0.82f);
    ubo.colorBG_falloff = glm::vec4(gridCol.r, gridCol.g, gridCol.b, 0.02f);
    m_planeY = state.hasMeshLoaded ? state.worldMinY : 0.0;
    ubo.planeY_pad = glm::vec4(static_cast<float>(m_planeY), 0.0f, 0.0f, 0.0f);
    glNamedBufferSubData(m_ubo, 0, sizeof(GridUBOData), &ubo);
}

void GridRenderer::draw(const RenderRenderState& state, const glm::mat4& view, const glm::mat4& proj) {
    if (!state.showGrid || m_program == 0) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glUseProgram(m_program);

    updateUbo(state, view, proj);

    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glUseProgram(0);
}

void GridRenderer::shutdown() {
    if (m_ubo) { glDeleteBuffers(1, &m_ubo); m_ubo = 0; }
    if (m_program) { glDeleteProgram(m_program); m_program = 0; }
    if (m_vao) { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
    if (m_vbo) { glDeleteBuffers(1, &m_vbo); m_vbo = 0; }
}
