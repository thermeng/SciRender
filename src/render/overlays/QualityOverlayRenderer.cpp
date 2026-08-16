#include "render/overlays/QualityOverlayRenderer.h"
#include "render/foundation/renderer.h"
#include "render/foundation/shader_utils.h"
#include <glm/gtc/type_ptr.hpp>
#include <vector>

void QualityOverlayRenderer::init(const ShaderSources& sources) {
    if (sources.qualityOverlayVert.empty() || sources.qualityOverlayFrag.empty()) return;

    m_program.reset(compileProgram(sources.qualityOverlayVert.c_str(),
                               sources.qualityOverlayFrag.c_str(), "QualityOverlay"));
    if (m_program.has()) {
        m_mvpLoc   = glGetUniformLocation(m_program, "uMVP");
        m_colorLoc = glGetUniformLocation(m_program, "uColor");
    }
}

void QualityOverlayRenderer::rebuild(const RenderRenderState& state) {
    auto buildOne = [&](GlVao& vao, GlBuffer& vbo, const std::shared_ptr<const std::vector<float>>& vertsPtr) {
        vao.reset(); vbo.reset();
        if (!vertsPtr || vertsPtr->empty()) return;
        glCreateVertexArrays(1, vao.ptr());
        glCreateBuffers(1, vbo.ptr());
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
    if (!state.showQualityOverlay || !m_program.has()) return;

    if (m_dirty) rebuild(state);

    auto hasData = [](const std::shared_ptr<const std::vector<float>>& p) {
        return p && !p->empty();
    };
    if (!hasData(state.qualityDegenerateTris) &&
        !hasData(state.qualityOpenEdges) &&
        !hasData(state.qualityNonManifoldEdges)) return;

    GLboolean depthWas = glIsEnabled(GL_DEPTH_TEST);
    GLint depthFuncWas = 0;
    glGetIntegerv(GL_DEPTH_FUNC, &depthFuncWas);
    GLboolean cullWas = glIsEnabled(GL_CULL_FACE);
    GLint lineWidthWas = 0;
    glGetIntegerv(GL_LINE_WIDTH, &lineWidthWas);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);
    glLineWidth(2.0f);

    glm::mat4 mvp = glm::make_mat4(projPtr) * glm::make_mat4(viewPtr);

    glUseProgram(m_program);
    glUniformMatrix4fv(m_mvpLoc, 1, GL_FALSE, glm::value_ptr(mvp));

    auto drawOne = [&](const GlVao& vao, GLsizei count, const float* color) {
        if (!vao.has() || count <= 0) return;
        glUniform4f(m_colorLoc, color[0], color[1], color[2], color[3]);
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

    drawOne(m_degenerateVao,
            hasData(state.qualityDegenerateTris)
                ? static_cast<GLsizei>(state.qualityDegenerateTris->size() / 2) : 0,
            kRed);
    drawOne(m_openEdgesVao,
            hasData(state.qualityOpenEdges)
                ? static_cast<GLsizei>(state.qualityOpenEdges->size() / 2) : 0,
            kOrange);
    drawOne(m_nonManifoldVao,
            hasData(state.qualityNonManifoldEdges)
                ? static_cast<GLsizei>(state.qualityNonManifoldEdges->size() / 2) : 0,
            kPurple);

    glUseProgram(0);
    glLineWidth(static_cast<GLfloat>(lineWidthWas));
    if (depthWas) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthFunc(static_cast<GLenum>(depthFuncWas));
    if (cullWas) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
}

void QualityOverlayRenderer::shutdown() {
    m_program.reset();
    m_openEdgesVao.reset(); m_openEdgesVbo.reset();
    m_nonManifoldVao.reset(); m_nonManifoldVbo.reset();
    m_degenerateVao.reset(); m_degenerateVbo.reset();
}


