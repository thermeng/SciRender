#pragma once
#include "render/foundation/gl_raii.h"
#include <glm/glm.hpp>
#include <vector>
#include <utility>

struct RenderRenderState;
struct ShaderSources;
struct MeshUBOData;

// Deep module for wireframe. Owns wire Program + thickness, auto-prefers
// cell edges when edgeDrawList present, else triangle edges. Hides GS vs
// glLineWidth fallback and crinkle-clip guard.
class WireframePass {
public:
    void init(const ShaderSources& sources);
    void shutdown();

    // Draw wireframe (cell or triangle) using provided UBO handle and mvp.
    // Caller owns UBO data; this pass binds and updates meshColor_wire flag.
    void draw(const RenderRenderState& state,
              MeshUBOData& ubo,
              GLuint meshUbo,
              const std::vector<std::pair<GLuint,int>>& drawList,
              const std::vector<std::pair<GLuint,int>>& edgeDrawList);

private:
    GlProgram wireProgram;
    GLint wireViewportLoc = -1;
    GLint wireHalfWidthLoc = -1;
    GLint wireMvpLoc = -1;
    GLint wireColorLoc = -1;
};
