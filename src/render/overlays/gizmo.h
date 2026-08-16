#pragma once
// Low-level modern-OpenGL 3D coordinate triad overlay with billboarded text labels.
// Pure GL (lines for axes, texture-mapped quads for X/Y/Z glyphs). No Qt layout.
// Designed to be drawn inside a corner viewport that tracks the camera's rotation.

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "render/foundation/gl_raii.h"

class Gizmo {
public:
    Gizmo();
    ~Gizmo();

    bool init();
    void shutdown();

    void draw(const glm::mat4& mainView, float dpr, int foot = 120);
    void drawLights(const glm::vec3 dirs[5], const glm::vec3 cols[5], float dpr, int foot = 120);

    bool isInitialized() const { return aaLineProgram.has() && textProgram.has(); }

private:
    GlProgram aaLineProgram;
    GlVao aaLineVAO;
    GlBuffer aaLineVBO;
    GLint  aaLineMvpLoc = -1, aaLineHalfWidthLoc = -1;
    GLint  aaLinePosLoc = -1, aaLineColLoc = -1, aaLineDistLoc = -1;

    GlProgram capProgram;
    GlVao capVAO;
    GlBuffer capVBO;
    GLint  capMvpLoc = -1, capColorLoc = -1;
    int    capVertCount = 0;

    GlVao originVAO;
    GlBuffer originVBO;
    int    originVertCount = 0;

    GlProgram lightMarkProgram;
    GLint  lightMarkMvpLoc = -1;

    GlProgram textProgram;
    GlVao textVAO;
    GlBuffer textVBO;
    GLint  textMvpLoc = -1, textTexLoc = -1, textPosLoc = -1, textColLoc = -1;
    GlTexture glyphTex;
    int    glyphAtlasW = 0, glyphAtlasH = 0;

    GlVao lightMarkVAO;
    GlBuffer lightMarkVBO;

    bool buildAtlas();
    bool buildAALineProgram();
    bool buildCapProgram();
    bool buildLightMarkProgram();
    bool buildTextProgram();
};


