#pragma once

#include <utility>
#include <initializer_list>
#include <cstddef>

typedef unsigned int GLuint;
typedef int GLint;

class GlTexture {
    GLuint m_id = 0;
public:
    GlTexture() = default;
    explicit GlTexture(GLuint id) : m_id(id) {}
    ~GlTexture();
    GlTexture(GlTexture&& o) noexcept : m_id(o.m_id) { o.m_id = 0; }
    GlTexture& operator=(GlTexture&& o) noexcept { reset(); m_id = o.m_id; o.m_id = 0; return *this; }
    GlTexture(const GlTexture&) = delete;
    GlTexture& operator=(const GlTexture&) = delete;

    GLuint get() const { return m_id; }
    GLuint* ptr() { return &m_id; }
    GLuint release() { GLuint r = m_id; m_id = 0; return r; }
    void reset(GLuint id = 0);
    bool has() const { return m_id != 0; }
    operator GLuint() const { return m_id; }
};

class GlBuffer {
    GLuint m_id = 0;
public:
    GlBuffer() = default;
    explicit GlBuffer(GLuint id) : m_id(id) {}
    ~GlBuffer();
    GlBuffer(GlBuffer&& o) noexcept : m_id(o.m_id) { o.m_id = 0; }
    GlBuffer& operator=(GlBuffer&& o) noexcept { reset(); m_id = o.m_id; o.m_id = 0; return *this; }
    GlBuffer(const GlBuffer&) = delete;
    GlBuffer& operator=(const GlBuffer&) = delete;

    GLuint get() const { return m_id; }
    GLuint* ptr() { return &m_id; }
    GLuint release() { GLuint r = m_id; m_id = 0; return r; }
    void reset(GLuint id = 0);
    bool has() const { return m_id != 0; }
    operator GLuint() const { return m_id; }
};

class GlVao {
    GLuint m_id = 0;
public:
    GlVao() = default;
    explicit GlVao(GLuint id) : m_id(id) {}
    ~GlVao();
    GlVao(GlVao&& o) noexcept : m_id(o.m_id) { o.m_id = 0; }
    GlVao& operator=(GlVao&& o) noexcept { reset(); m_id = o.m_id; o.m_id = 0; return *this; }
    GlVao(const GlVao&) = delete;
    GlVao& operator=(const GlVao&) = delete;

    GLuint get() const { return m_id; }
    GLuint* ptr() { return &m_id; }
    GLuint release() { GLuint r = m_id; m_id = 0; return r; }
    void reset(GLuint id = 0);
    bool has() const { return m_id != 0; }
    operator GLuint() const { return m_id; }
};

class GlProgram {
    GLuint m_id = 0;
public:
    GlProgram() = default;
    explicit GlProgram(GLuint id) : m_id(id) {}
    ~GlProgram();
    GlProgram(GlProgram&& o) noexcept : m_id(o.m_id) { o.m_id = 0; }
    GlProgram& operator=(GlProgram&& o) noexcept { reset(); m_id = o.m_id; o.m_id = 0; return *this; }
    GlProgram(const GlProgram&) = delete;
    GlProgram& operator=(const GlProgram&) = delete;

    GLuint get() const { return m_id; }
    GLuint release() { GLuint r = m_id; m_id = 0; return r; }
    void reset(GLuint id = 0);
    bool has() const { return m_id != 0; }
    operator GLuint() const { return m_id; }
};

class GlFramebuffer {
    GLuint m_id = 0;
public:
    GlFramebuffer() = default;
    explicit GlFramebuffer(GLuint id) : m_id(id) {}
    ~GlFramebuffer();
    GlFramebuffer(GlFramebuffer&& o) noexcept : m_id(o.m_id) { o.m_id = 0; }
    GlFramebuffer& operator=(GlFramebuffer&& o) noexcept { reset(); m_id = o.m_id; o.m_id = 0; return *this; }
    GlFramebuffer(const GlFramebuffer&) = delete;
    GlFramebuffer& operator=(const GlFramebuffer&) = delete;

    GLuint get() const { return m_id; }
    GLuint* ptr() { return &m_id; }
    GLuint release() { GLuint r = m_id; m_id = 0; return r; }
    void reset(GLuint id = 0);
    bool has() const { return m_id != 0; }
    operator GLuint() const { return m_id; }
};

class GlRenderbuffer {
    GLuint m_id = 0;
public:
    GlRenderbuffer() = default;
    explicit GlRenderbuffer(GLuint id) : m_id(id) {}
    ~GlRenderbuffer();
    GlRenderbuffer(GlRenderbuffer&& o) noexcept : m_id(o.m_id) { o.m_id = 0; }
    GlRenderbuffer& operator=(GlRenderbuffer&& o) noexcept { reset(); m_id = o.m_id; o.m_id = 0; return *this; }
    GlRenderbuffer(const GlRenderbuffer&) = delete;
    GlRenderbuffer& operator=(const GlRenderbuffer&) = delete;

    GLuint get() const { return m_id; }
    GLuint* ptr() { return &m_id; }
    GLuint release() { GLuint r = m_id; m_id = 0; return r; }
    void reset(GLuint id = 0);
    bool has() const { return m_id != 0; }
    operator GLuint() const { return m_id; }
};

// RAII guard that saves the render state a pass/overlay mutates (viewport,
// depth/blend/cull/scissor/program-point-size enables, depth func/mask/clamp,
// blend func/equation, polygon mode, clip control, active texture, line width)
// and restores it in the destructor. Guarantees state handover even on early
// returns/exceptions — eliminates the classic "leaked GL state" bug class where
// a draw path forgets to restore depth/blend/etc.
class GLStateGuard {
public:
    GLStateGuard();
    ~GLStateGuard();
    GLStateGuard(const GLStateGuard&) = delete;
    GLStateGuard& operator=(const GLStateGuard&) = delete;

private:
    int m_viewport[4] = {};
    bool m_depthTest = false;
    bool m_depthClamp = false;
    bool m_blend = false;
    bool m_cullFace = false;
    bool m_scissorTest = false;
    bool m_programPointSize = false;
    int m_depthFunc = 0;
    bool m_depthMask = true;
    int m_blendSrcRgb = 0, m_blendDstRgb = 0;
    int m_blendSrcAlpha = 0, m_blendDstAlpha = 0;
    int m_blendEquationRgb = 0, m_blendEquationAlpha = 0;
    int m_polyMode[2] = {};
    int m_clipOrigin = 0, m_clipDepthMode = 0;
    int m_activeTexture = 0;
    float m_lineWidth = 1.0f;
    GLint m_currentProgram = 0;
};

// Describes one interleaved vertex attribute: shader location, component
// count, and byte offset from the start of each vertex.
struct VertexAttribSpec {
    int location;      // shader attribute location
    int size;          // components per attribute (1..4)
    int offsetBytes;   // byte offset of this attribute within the vertex
};

// Create a VAO + VBO pair and configure an interleaved attribute layout in one
// call. `data`/`sizeBytes` are uploaded to the buffer with `usage`
// (GL_STATIC_DRAW / GL_DYNAMIC_DRAW) and bound at vertex-buffer binding 0 with
// `strideBytes` per vertex. Replaces the boilerplate DSA idiom repeated across
// every pass/overlay.
void setupVertexBuffer(GlVao& vao, GlBuffer& vbo,
                       const void* data, size_t sizeBytes,
                       size_t strideBytes,
                       std::initializer_list<VertexAttribSpec> attribs,
                       int usage);
