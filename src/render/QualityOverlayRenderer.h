#pragma once

#include <glad/gl.h>

struct RenderRenderState;
struct ShaderSources;

class QualityOverlayRenderer {
public:
    void init(const ShaderSources& sources);
    void draw(const RenderRenderState& state, const float* viewPtr, const float* projPtr);
    void markDirty() { m_dirty = true; }
    void shutdown();

private:
    void rebuild(const RenderRenderState& state);

    GLuint m_program = 0;
    GLint m_mvpLoc = -1;
    GLint m_colorLoc = -1;
    GLint m_depthBiasLoc = -1;

    GLuint m_openEdgesVao = 0, m_openEdgesVbo = 0;
    GLuint m_nonManifoldVao = 0, m_nonManifoldVbo = 0;
    GLuint m_degenerateVao = 0, m_degenerateVbo = 0;
    bool m_dirty = true;
};
