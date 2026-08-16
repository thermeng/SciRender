#pragma once

#include "render/foundation/gl_raii.h"

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

    GlProgram m_program;
    GLint m_mvpLoc = -1;
    GLint m_colorLoc = -1;

    GlVao m_openEdgesVao;
    GlBuffer m_openEdgesVbo;
    GlVao m_nonManifoldVao;
    GlBuffer m_nonManifoldVbo;
    GlVao m_degenerateVao;
    GlBuffer m_degenerateVbo;
    bool m_dirty = true;
};


