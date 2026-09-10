#include "render/overlays/ProbeOverlay.h"
#include "render/foundation/shader_utils.h"
#include "render/foundation/renderer.h"
#include <glm/gtc/type_ptr.hpp>
void ProbeOverlay::init(const ShaderSources& s){
    if(s.bboxVert.empty()||s.bboxFrag.empty()) return;
    m_prog.reset(compileProgram(s.bboxVert.c_str(), s.bboxFrag.c_str(), "Probe"));
    if(m_prog.has()){ m_mvpLoc=glGetUniformLocation(m_prog,"uMVP"); m_colorLoc=glGetUniformLocation(m_prog,"uColor"); }
    m_init=true;
}
void ProbeOverlay::draw(const RenderRenderState& st, const glm::mat4& view, const glm::mat4& proj){
    if(!st.showProbe || !m_prog.has() || !m_init) return;
    glm::mat4 mvp = proj * view;
    float pt[3]={st.probePos.x, st.probePos.y, st.probePos.z};
    GLStateGuard guard;
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glPointSize(12.0f);
    if(!m_vao.has()){
        setupVertexBuffer(m_vao, m_vbo, pt, sizeof(pt), 3*sizeof(float), {{0,3,0}}, GL_DYNAMIC_DRAW);
    } else {
        glBindVertexArray(m_vao);
        glNamedBufferSubData(m_vbo,0,sizeof(pt),pt);
        glBindVertexArray(0);
    }
    glUseProgram(m_prog);
    glUniformMatrix4fv(m_mvpLoc,1,GL_FALSE, glm::value_ptr(mvp));
    // amber point with white border via two draws
    glUniform4f(m_colorLoc, 1.0f, 0.52f, 0.0f, 1.0f);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_POINTS,0,1);
    glBindVertexArray(0);
    glPointSize(8.0f);
    glUniform4f(m_colorLoc, 1.0f, 1.0f, 1.0f, 1.0f);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_POINTS,0,1);
    glBindVertexArray(0);
    glDisable(GL_PROGRAM_POINT_SIZE);
    glUseProgram(0);
}
void ProbeOverlay::shutdown(){ m_prog.reset(); m_vao.reset(); m_vbo.reset(); }
