#pragma once

#include <utility>

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
