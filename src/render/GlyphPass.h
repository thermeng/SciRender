#pragma once

#include "render/gl_raii.h"

#include <glm/glm.hpp>

#include <string>

struct RenderRenderState;
struct ShaderSources;
class VectorGlyphSet;
class ColormapManager;

class GlyphPass {
public:
    void init(const ShaderSources& sources);
    void draw(const RenderRenderState& state,
              const glm::mat4& view,
              const glm::mat4& proj,
              const VectorGlyphSet& glyphs,
              const ColormapManager& colormap);
    void shutdown();

private:
    GlProgram glyphProgram;
    GlBuffer glyphUbo;
    GLuint glyphUboIndex = ~0u;
    GLint glyphLutLoc = -1;
    GLint glyphViewPosLoc = -1;
};
