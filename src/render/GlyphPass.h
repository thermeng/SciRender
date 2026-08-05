#pragma once

#include <glad/gl.h>
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

    bool hasProgram() const { return glyphProgram != 0; }

private:
    GLuint glyphProgram = 0;
    GLuint glyphUbo = 0;
    GLuint glyphUboIndex = GL_INVALID_INDEX;
    GLint glyphLutLoc = -1;
    GLint glyphViewPosLoc = -1;
};
