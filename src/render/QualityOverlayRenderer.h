#pragma once

#include <glad/gl.h>

struct RenderRenderState;

class QualityOverlayRenderer {
public:
    void draw(const RenderRenderState& state, GLuint meshShaderProgram);
    void markDirty() { m_dirty = true; }
    void shutdown();

private:
    void rebuild(const RenderRenderState& state);

    GLuint m_openEdgesVao = 0, m_openEdgesVbo = 0;
    GLuint m_nonManifoldVao = 0, m_nonManifoldVbo = 0;
    GLuint m_degenerateVao = 0, m_degenerateVbo = 0;
    bool m_dirty = true;
};
