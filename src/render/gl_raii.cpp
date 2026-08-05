#include "render/gl_raii.h"
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
