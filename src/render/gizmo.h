#pragma once
// Low-level modern-OpenGL 3D coordinate triad overlay with billboarded text labels.
// Pure GL (lines for axes, texture-mapped quads for X/Y/Z glyphs). No Qt layout.
// Designed to be drawn inside a corner viewport that tracks the camera's rotation.

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glad/gl.h>

class Gizmo {
public:
    Gizmo();
    ~Gizmo();

    // Builds GL programs, buffers, and the glyph atlas. Call with a live GL context.
    bool init();
    // Releases all owned GL resources.
    void shutdown();

    // Draws the triad into a fixed bottom-left corner viewport.
    //  mainView : the scene's full view matrix (used only for its rotation part)
    //  dpr      : device-pixel-ratio so the overlay footprint stays constant on HiDPI
    //  foot     : viewport size in pixels (default 120)
    void draw(const glm::mat4& mainView, float dpr, int foot = 120);

    // draws Light Kit direction markers in the corner viewport.
    //  dirs[]  : kit-local unit directions
    //  cols[]  : RGB tint per light.
    //  dpr     : device-pixel-ratio
    //  foot    : viewport size in pixels (default 120)
    void drawLights(const glm::vec3 dirs[5], const glm::vec3 cols[5], float dpr, int foot = 120);

    bool isInitialized() const { return aaLineProgram != 0 && textProgram != 0; }

private:
    // Screen-space AA axis lines (clip-space pos + color + signed dist)
    GLuint aaLineVAO = 0, aaLineVBO = 0, aaLineProgram = 0;
    GLint  aaLineMvpLoc = -1, aaLineHalfWidthLoc = -1;
    GLint  aaLinePosLoc = -1, aaLineColLoc = -1, aaLineDistLoc = -1;

    // Axis tip cones (simple diffuse-lit)
    GLuint capVAO = 0, capVBO = 0, capProgram = 0;
    GLint  capMvpLoc = -1, capLightDirLoc = -1, capColorLoc = -1;
    int    capVertCount = 0;

    // Origin disc at pivot
    GLuint originVAO = 0, originVBO = 0;
    int    originVertCount = 0;

    // Light-marker solid-color program (reuses the old simple line shader)
    GLuint lightMarkProgram = 0;
    GLint  lightMarkMvpLoc = -1;

    // Billboard text quads (6 verts/char, vec4 = px.xy + uv.uv)
    GLuint textVAO = 0, textVBO = 0, textProgram = 0;
    GLint  textMvpLoc = -1, textColorLoc = -1, textTexLoc = -1, textPosLoc = -1;
    GLuint glyphTex = 0;
    int    glyphAtlasW = 0, glyphAtlasH = 0;

    // light-marker disc geometry (5 lights * 6 verts * vec5 = px.xy + rgb)
    GLuint lightMarkVAO = 0, lightMarkVBO = 0;

    bool buildAtlas();      // rasterize X/Y/Z into a horizontal strip atlas via Qt
    bool buildAALineProgram();
    bool buildCapProgram();
    bool buildLightMarkProgram();
    bool buildTextProgram();
};
