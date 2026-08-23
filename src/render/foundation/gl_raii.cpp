#include "render/foundation/gl_raii.h"
#include <glad/gl.h>

GlTexture::~GlTexture() { if (m_id) glDeleteTextures(1, &m_id); }
void GlTexture::reset(GLuint id) { if (m_id) glDeleteTextures(1, &m_id); m_id = id; }

GlBuffer::~GlBuffer() { if (m_id) glDeleteBuffers(1, &m_id); }
void GlBuffer::reset(GLuint id) { if (m_id) glDeleteBuffers(1, &m_id); m_id = id; }

GlVao::~GlVao() { if (m_id) glDeleteVertexArrays(1, &m_id); }
void GlVao::reset(GLuint id) { if (m_id) glDeleteVertexArrays(1, &m_id); m_id = id; }

GlProgram::~GlProgram() { if (m_id) glDeleteProgram(m_id); }
void GlProgram::reset(GLuint id) { if (m_id) glDeleteProgram(m_id); m_id = id; }

GlFramebuffer::~GlFramebuffer() { if (m_id) glDeleteFramebuffers(1, &m_id); }
void GlFramebuffer::reset(GLuint id) { if (m_id) glDeleteFramebuffers(1, &m_id); m_id = id; }

GlRenderbuffer::~GlRenderbuffer() { if (m_id) glDeleteRenderbuffers(1, &m_id); }
void GlRenderbuffer::reset(GLuint id) { if (m_id) glDeleteRenderbuffers(1, &m_id); m_id = id; }

GLStateGuard::GLStateGuard() {
    glGetIntegerv(GL_VIEWPORT, m_viewport);
    glGetIntegerv(GL_DEPTH_FUNC, &m_depthFunc);
    glGetIntegerv(GL_DEPTH_WRITEMASK, reinterpret_cast<GLint*>(&m_depthMask));
    glGetIntegerv(GL_ACTIVE_TEXTURE, &m_activeTexture);
    glGetIntegerv(GL_POLYGON_MODE, m_polyMode);
    glGetIntegerv(GL_CLIP_ORIGIN, &m_clipOrigin);
    glGetIntegerv(GL_CLIP_DEPTH_MODE, &m_clipDepthMode);
    glGetIntegerv(GL_BLEND_SRC_RGB, &m_blendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &m_blendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &m_blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &m_blendDstAlpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &m_blendEquationRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &m_blendEquationAlpha);
    glGetIntegerv(GL_CURRENT_PROGRAM, &m_currentProgram);
    glGetFloatv(GL_LINE_WIDTH, &m_lineWidth);
    m_depthTest  = glIsEnabled(GL_DEPTH_TEST) != 0;
    m_depthClamp = glIsEnabled(GL_DEPTH_CLAMP) != 0;
    m_blend      = glIsEnabled(GL_BLEND) != 0;
    m_cullFace   = glIsEnabled(GL_CULL_FACE) != 0;
    m_scissorTest = glIsEnabled(GL_SCISSOR_TEST) != 0;
    m_programPointSize = glIsEnabled(GL_PROGRAM_POINT_SIZE) != 0;
}

GLStateGuard::~GLStateGuard() {
    glViewport(m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3]);
    if (m_depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (m_depthClamp) glEnable(GL_DEPTH_CLAMP); else glDisable(GL_DEPTH_CLAMP);
    if (m_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (m_cullFace) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (m_scissorTest) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (m_programPointSize) glEnable(GL_PROGRAM_POINT_SIZE); else glDisable(GL_PROGRAM_POINT_SIZE);
    glDepthFunc(static_cast<GLenum>(m_depthFunc));
    glDepthMask(m_depthMask ? GL_TRUE : GL_FALSE);
    glBlendFuncSeparate(static_cast<GLenum>(m_blendSrcRgb), static_cast<GLenum>(m_blendDstRgb),
                        static_cast<GLenum>(m_blendSrcAlpha), static_cast<GLenum>(m_blendDstAlpha));
    glBlendEquationSeparate(static_cast<GLenum>(m_blendEquationRgb), static_cast<GLenum>(m_blendEquationAlpha));
    glPolygonMode(GL_FRONT, static_cast<GLenum>(m_polyMode[0]));
    glPolygonMode(GL_BACK, static_cast<GLenum>(m_polyMode[1]));
    glClipControl(static_cast<GLenum>(m_clipOrigin), static_cast<GLenum>(m_clipDepthMode));
    glActiveTexture(static_cast<GLenum>(m_activeTexture));
    glLineWidth(m_lineWidth);
    glUseProgram(static_cast<GLuint>(m_currentProgram));
}

void setupVertexBuffer(GlVao& vao, GlBuffer& vbo,
                       const void* data, size_t sizeBytes,
                       size_t strideBytes,
                       std::initializer_list<VertexAttribSpec> attribs,
                       int usage) {
    glCreateVertexArrays(1, vao.ptr());
    glCreateBuffers(1, vbo.ptr());
    for (const VertexAttribSpec& a : attribs) {
        glEnableVertexArrayAttrib(vao, a.location);
        glVertexArrayAttribFormat(vao, a.location, a.size, GL_FLOAT, GL_FALSE, a.offsetBytes);
        glVertexArrayAttribBinding(vao, a.location, 0);
    }
    glNamedBufferData(vbo, static_cast<GLsizeiptr>(sizeBytes), data, static_cast<GLenum>(usage));
    glVertexArrayVertexBuffer(vao, 0, vbo, 0, static_cast<GLsizei>(strideBytes));
}


