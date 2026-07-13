#pragma once
// Low-level modern-OpenGL 3D coordinate triad overlay with billboarded text labels.
// Pure GL (lines for axes, texture-mapped quads for X/Y/Z glyphs). No Qt layout.
// Designed to be drawn inside a corner viewport that tracks the camera's rotation.

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glad/glad.h>

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
    void draw(const glm::mat4& mainView, float dpr, int fbHeight);

    bool isInitialized() const { return lineProgram != 0 && textProgram != 0; }

private:
    // Axis lines (origin -> tip, per-vertex color)
    GLuint lineVAO = 0, lineVBO = 0, lineProgram = 0;
    GLint  lineMvpLoc = -1, lineColorLoc = -1, linePosLoc = -1, lineColLoc = -1;

    // Billboard text quads (6 verts/char, vec4 = px.xy + uv.uv)
    GLuint textVAO = 0, textVBO = 0, textProgram = 0;
    GLint  textMvpLoc = -1, textColorLoc = -1, textTexLoc = -1, textPosLoc = -1;
    GLuint glyphTex = 0;
    int    glyphAtlasW = 0, glyphAtlasH = 0;

    bool buildAtlas();      // rasterize X/Y/Z into a horizontal strip atlas via Qt
    bool buildLineProgram();
    bool buildTextProgram();
};
