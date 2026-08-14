#include "render/GridRenderer.h"
#include "render/shader_utils.h"
#include "render/renderer.h"
#include <glm/gtc/type_ptr.hpp>

void GridRenderer::init(const ShaderSources& sources) {
    if (sources.gridVert.empty() || sources.gridFrag.empty()) return;

    m_program.reset(compileProgram(sources.gridVert.c_str(), sources.gridFrag.c_str(), "Grid"));
    if (m_program.has()) {
        m_shadowMapLoc = glGetUniformLocation(m_program, "uShadowMap");
    }

    const float q[8] = { -1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f, 1.0f, 1.0f };
    glCreateVertexArrays(1, m_vao.ptr());
    glCreateBuffers(1, m_vbo.ptr());
    glEnableVertexArrayAttrib(m_vao, 0);
    glVertexArrayAttribFormat(m_vao, 0, 2, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(m_vao, 0, 0);
    glNamedBufferData(m_vbo, sizeof(q), q, GL_STATIC_DRAW);
    glVertexArrayVertexBuffer(m_vao, 0, m_vbo, 0, 2 * sizeof(float));
}

void GridRenderer::updateUbo(const RenderRenderState& state, const glm::mat4& view, const glm::mat4& proj) {
    if (!m_program.has()) return;
    if (!m_ubo.has()) {
        m_uboIndex = glGetUniformBlockIndex(m_program, "GridUBO");
        if (m_uboIndex != ~0u) {
            glUniformBlockBinding(m_program, m_uboIndex, 2);
            glCreateBuffers(1, m_ubo.ptr());
            glNamedBufferData(m_ubo, sizeof(GridUBOData), nullptr, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_UNIFORM_BUFFER, 2, m_ubo);
        }
    }
    if (!m_ubo.has()) return;
    GridUBOData ubo{};
    ubo.invView = glm::inverse(view);
    ubo.invProj = glm::inverse(proj);
    ubo.view = view;
    ubo.proj = proj;
    ubo.camPos_colorR = glm::vec4(glm::vec3(state.camera.position), 0.0f);
    float bgLum = 0.299f * state.bgColor[0] + 0.587f * state.bgColor[1] + 0.114f * state.bgColor[2];
    glm::vec3 gridCol = (bgLum > 0.5f) ? glm::vec3(0.18f, 0.18f, 0.20f) : glm::vec3(0.78f, 0.78f, 0.82f);
    ubo.colorBG_falloff = glm::vec4(gridCol.r, gridCol.g, gridCol.b, 0.02f);
    double planePos = 0.0;
    if (state.hasMeshLoaded) {
        switch (state.gridAxis) {
            case 0: planePos = state.worldMinX; break;
            case 1: planePos = state.worldMaxX; break;
            case 2: planePos = state.worldMinY; break;
            case 3: planePos = state.worldMaxY; break;
            case 4: planePos = state.worldMinZ; break;
            case 5: planePos = state.worldMaxZ; break;
        }
    }
    ubo.gridAxis_planePos = glm::vec4(static_cast<float>(state.gridAxis / 2), static_cast<float>(planePos), 0.0f, 0.0f);
    ubo.flags = glm::vec4(m_useZeroToOne ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
    ubo.lightMVP = state.lightMVP;
    ubo.shadowParams = state.gridShadows ? glm::vec4(1.0f, 0.003f, 0.0f, 0.0f) : glm::vec4(0.0f, 0.003f, 0.0f, 0.0f);
    glNamedBufferSubData(m_ubo, 0, sizeof(GridUBOData), &ubo);
}

void GridRenderer::draw(const RenderRenderState& state, const glm::mat4& view, const glm::mat4& proj, GLuint shadowTex) {
    if (!state.showGrid || !m_program.has()) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glUseProgram(m_program);

    updateUbo(state, view, proj);

    if (shadowTex != 0 && m_shadowMapLoc >= 0) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, shadowTex);
        glUniform1i(m_shadowMapLoc, 0);
    }

    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glUseProgram(0);
}

void GridRenderer::shutdown() {
    m_ubo.reset();
    m_program.reset();
    m_vao.reset();
    m_vbo.reset();
}
