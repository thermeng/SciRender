#include "render/QualityOverlayRenderer.h"
#include "render/renderer.h"
#include <vector>

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

void QualityOverlayRenderer::draw(const RenderRenderState& state, GLuint meshShaderProgram) {
    if (!state.showQualityOverlay || meshShaderProgram == 0) return;

    GLboolean depthWas = glIsEnabled(GL_DEPTH_TEST);
    GLboolean cullWas  = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    if (m_dirty) rebuild(state);
    auto drawCached = [&](GLuint vao, GLsizei count) {
        if (vao == 0 || count <= 0) return;
        glBindVertexArray(vao);
        glDrawArrays(GL_LINES, 0, count);
        glBindVertexArray(0);
    };
    drawCached(m_openEdgesVao, state.qualityOpenEdges ? static_cast<GLsizei>(state.qualityOpenEdges->size() / 3) : 0);
    drawCached(m_nonManifoldVao, state.qualityNonManifoldEdges ? static_cast<GLsizei>(state.qualityNonManifoldEdges->size() / 3) : 0);
    drawCached(m_degenerateVao, state.qualityDegenerateTris ? static_cast<GLsizei>(state.qualityDegenerateTris->size() / 3) : 0);
    if (depthWas) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (cullWas)  glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);
}

void QualityOverlayRenderer::shutdown() {
    auto del = [](GLuint& vao, GLuint& vbo) {
        if (vao) { glDeleteVertexArrays(1, &vao); vao = 0; }
        if (vbo) { glDeleteBuffers(1, &vbo); vbo = 0; }
    };
    del(m_openEdgesVao, m_openEdgesVbo);
    del(m_nonManifoldVao, m_nonManifoldVbo);
    del(m_degenerateVao, m_degenerateVbo);
}
