#include "render/QualityOverlayRenderer.h"
#include "render/renderer.h"
#include "render/shader_utils.h"
#include <glm/gtc/type_ptr.hpp>
#include <vector>

void QualityOverlayRenderer::init(const ShaderSources& sources) {
    if (sources.qualityOverlayVert.empty() || sources.qualityOverlayFrag.empty()) return;

    m_program = compileProgram(sources.qualityOverlayVert.c_str(),
                               sources.qualityOverlayFrag.c_str(), "QualityOverlay");
    if (m_program != 0) {
        m_mvpLoc       = glGetUniformLocation(m_program, "uMVP");
        m_colorLoc     = glGetUniformLocation(m_program, "uColor");
        m_depthBiasLoc = glGetUniformLocation(m_program, "uDepthBias");
    }
}

void QualityOverlayRenderer::rebuild(const RenderRenderState& state) {
    auto buildOne = [&](GLuint& vao, GLuint& vbo, const std::shared_ptr<const std::vector<float>>& vertsPtr) {
        if (vao) { glDeleteVertexArrays(1, &vao); vao = 0; }
        if (vbo) { glDeleteBuffers(1, &vbo); vbo = 0; }
        if (!vertsPtr || vertsPtr->empty()) return;
        glCreateVertexArrays(1, &vao);
        glCreateBuffers(1, &vbo);
        glEnableVertexArrayAttrib(vao, 0);
        glVertexArrayAttribFormat(vao, 0, 3, GL_FLOAT, GL_FALSE, 0);
        glVertexArrayAttribBinding(vao, 0, 0);
        glNamedBufferData(vbo, vertsPtr->size() * sizeof(float), vertsPtr->data(), GL_STATIC_DRAW);
        glVertexArrayVertexBuffer(vao, 0, vbo, 0, 3 * sizeof(float));
    };
    buildOne(m_openEdgesVao, m_openEdgesVbo, state.qualityOpenEdges);
    buildOne(m_nonManifoldVao, m_nonManifoldVbo, state.qualityNonManifoldEdges);
    buildOne(m_degenerateVao, m_degenerateVbo, state.qualityDegenerateTris);
    m_dirty = false;
}

void QualityOverlayRenderer::draw(const RenderRenderState& state, const float* viewPtr, const float* projPtr) {
    if (!state.showQualityOverlay || m_program == 0) return;

    if (m_dirty) rebuild(state);

    auto hasData = [](const std::shared_ptr<const std::vector<float>>& p) {
        return p && !p->empty();
    };
    if (!hasData(state.qualityDegenerateTris) &&
        !hasData(state.qualityOpenEdges) &&
        !hasData(state.qualityNonManifoldEdges)) return;

    GLboolean depthWas = glIsEnabled(GL_DEPTH_TEST);
    GLenum depthFuncWas = glGetError(); // dummy, we'll save real state below
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);
    glLineWidth(2.0f);

    glm::mat4 mvp = glm::make_mat4(projPtr) * glm::make_mat4(viewPtr);

    glUseProgram(m_program);
    glUniformMatrix4fv(m_mvpLoc, 1, GL_FALSE, glm::value_ptr(mvp));

    auto drawOne = [&](GLuint vao, GLsizei count, const float* color, float bias) {
        if (vao == 0 || count <= 0) return;
        glUniform4f(m_colorLoc, color[0], color[1], color[2], color[3]);
        glUniform1f(m_depthBiasLoc, bias);
        glBindVertexArray(vao);
        glDrawArrays(GL_LINES, 0, count);
        glBindVertexArray(0);
    };

    // degenerate triangles: red
    static const float kRed[]     = { 1.0f, 0.4f,  0.4f,  1.0f };
    // open edges: orange
    static const float kOrange[]  = { 1.0f, 0.667f, 0.267f, 1.0f };
    // non-manifold: purple (edges + verts share color)
    static const float kPurple[]  = { 1.0f, 0.267f, 1.0f,  1.0f };

    constexpr float kBias = -0.001f;

    drawOne(m_degenerateVao,
            hasData(state.qualityDegenerateTris)
                ? static_cast<GLsizei>(state.qualityDegenerateTris->size() / 3) : 0,
            kRed, kBias);
    drawOne(m_openEdgesVao,
            hasData(state.qualityOpenEdges)
                ? static_cast<GLsizei>(state.qualityOpenEdges->size() / 3) : 0,
            kOrange, kBias);
    drawOne(m_nonManifoldVao,
            hasData(state.qualityNonManifoldEdges)
                ? static_cast<GLsizei>(state.qualityNonManifoldEdges->size() / 3) : 0,
            kPurple, kBias);

    glUseProgram(0);
    glLineWidth(1.0f);
    if (depthWas) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    if (glIsEnabled(GL_CULL_FACE)) {} else glDisable(GL_CULL_FACE);
}

void QualityOverlayRenderer::shutdown() {
    if (m_program) { glDeleteProgram(m_program); m_program = 0; }
    auto del = [](GLuint& vao, GLuint& vbo) {
        if (vao) { glDeleteVertexArrays(1, &vao); vao = 0; }
        if (vbo) { glDeleteBuffers(1, &vbo); vbo = 0; }
    };
    del(m_openEdgesVao, m_openEdgesVbo);
    del(m_nonManifoldVao, m_nonManifoldVbo);
    del(m_degenerateVao, m_degenerateVbo);
}
