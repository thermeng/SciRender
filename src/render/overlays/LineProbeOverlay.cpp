#include "render/overlays/LineProbeOverlay.h"
#include "render/foundation/shader_utils.h"
#include "render/foundation/renderer.h"
#include <glm/gtc/type_ptr.hpp>

void LineProbeOverlay::init(const ShaderSources& sources){
    if(sources.bboxVert.empty() || sources.bboxFrag.empty()) return;
    m_prog.reset(compileProgram(sources.bboxVert.c_str(), sources.bboxFrag.c_str(), "LineProbe"));
    if(m_prog.has()){
        m_mvpLoc = glGetUniformLocation(m_prog, "uMVP");
        m_colorLoc = glGetUniformLocation(m_prog, "uColor");
    }
    m_init = true;
}
void LineProbeOverlay::draw(const RenderRenderState& state, const glm::mat4& view, const glm::mat4& proj){
    if(!state.showLineProbe || !m_prog.has() || !m_init) return;
    glm::mat4 mvp = proj * view; // world space line already

    // line: P0->P1
    float lineVerts[6] = {
        state.lineP0.x, state.lineP0.y, state.lineP0.z,
        state.lineP1.x, state.lineP1.y, state.lineP1.z
    };
    float pointVerts[6] = {
        state.lineP0.x, state.lineP0.y, state.lineP0.z,
        state.lineP1.x, state.lineP1.y, state.lineP1.z
    };

    GLStateGuard guard;
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(m_prog);
    glUniformMatrix4fv(m_mvpLoc, 1, GL_FALSE, glm::value_ptr(mvp));

    // Draw line
    if(!m_vaoLine.has()){
        setupVertexBuffer(m_vaoLine, m_vboLine, lineVerts, sizeof(lineVerts), 3*sizeof(float), {{0,3,0}}, GL_DYNAMIC_DRAW);
    } else {
        glBindVertexArray(m_vaoLine);
        glNamedBufferSubData(m_vboLine, 0, sizeof(lineVerts), lineVerts);
        glBindVertexArray(0);
    }
    glLineWidth(3.0f);
    glUniform4f(m_colorLoc, 1.0f, 0.85f, 0.0f, 1.0f); // amber line
    glBindVertexArray(m_vaoLine);
    glDrawArrays(GL_LINES, 0, 2);
    glBindVertexArray(0);

    // Draw endpoints as points
    if(!m_vaoPoints.has()){
        setupVertexBuffer(m_vaoPoints, m_vboPoints, pointVerts, sizeof(pointVerts), 3*sizeof(float), {{0,3,0}}, GL_DYNAMIC_DRAW);
    } else {
        glBindVertexArray(m_vaoPoints);
        glNamedBufferSubData(m_vboPoints, 0, sizeof(pointVerts), pointVerts);
        glBindVertexArray(0);
    }
    glPointSize(10.0f);
    // P0 = cyan, P1 = magenta — but we draw both with same color for simplicity, use two draws with different uniforms
    glEnable(GL_PROGRAM_POINT_SIZE);
    glBindVertexArray(m_vaoPoints);
    glUniform4f(m_colorLoc, 0.0f, 0.95f, 0.95f, 1.0f);
    glDrawArrays(GL_POINTS, 0, 1);
    glUniform4f(m_colorLoc, 1.0f, 0.2f, 0.6f, 1.0f);
    glDrawArrays(GL_POINTS, 1, 1);
    glBindVertexArray(0);
    glDisable(GL_PROGRAM_POINT_SIZE);

    glUseProgram(0);
}
void LineProbeOverlay::shutdown(){
    m_prog.reset(); m_vaoLine.reset(); m_vboLine.reset(); m_vaoPoints.reset(); m_vboPoints.reset();
}
