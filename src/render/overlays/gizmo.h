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
    // Screen corner the triad square is pinned to. Stored as a plain int in
    // RenderRenderState::gizmoCorner (value-type snapshot carries no enums).
    enum Corner { BottomLeft = 0, BottomRight = 1, TopLeft = 2, TopRight = 3 };

    // Footprint presets indexed by RenderRenderState::gizmoSizeChoice (S/M/L).
    static constexpr int kNumSizes = 3;
    static constexpr int kMarginPx = 10;   // gap between triad square and viewport edges
    static int footprintFor(int sizeChoice) {
        static constexpr int kFootprints[kNumSizes] = { 96, 128, 160 };
        return (sizeChoice >= 0 && sizeChoice < kNumSizes) ? kFootprints[sizeChoice] : 128;
    }

    // Device-pixel rect (GL bottom-left convention) of the triad square for a
    // given corner/footprint. Shared by draw(), hit-test, and LightMarkerOverlay
    // so every consumer agrees on placement by construction.
    static glm::ivec2 rectOrigin(int deviceW, int deviceH, float dpr, int foot, int corner);

    Gizmo();
    ~Gizmo();

    bool init();
    void shutdown();

    // Renders the triad into the corner sub-viewport. hoverAxis (-1 = none) is
    // brightened + thickened for pointer feedback.
    void draw(const glm::mat4& mainView, float dpr,
              int deviceW, int deviceH,
              int corner = BottomLeft, int foot = 128, int hoverAxis = -1);

    bool isInitialized() const { return aaLineProgram.has() && textProgram.has(); }

    // Maps a click (device px, top-left origin) to the axis (0=X, 1=Y, 2=Z)
    // whose shaft/cone/label zone contains it, or -1 if none. Uses exactly the
    // same projection and rect math as draw(), so hits agree with what is seen.
    static int hitTestAxis(const glm::mat4& mainView, float dpr,
                           int deviceW, int deviceH,
                           float xDevPx, float yDevPx,
                           int corner = BottomLeft, int foot = 128);

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

    GlProgram discProgram;
    GLint  discMvpLoc = -1;

    GlProgram textProgram;
    GlVao textVAO;
    GlBuffer textVBO;
    GLint  textMvpLoc = -1, textTexLoc = -1, textPosLoc = -1, textColLoc = -1;
    GlTexture glyphTex;
    int    glyphAtlasW = 0, glyphAtlasH = 0;

    bool buildAtlas();
    bool buildAALineProgram();
    bool buildCapProgram();
    bool buildDiscProgram();
    bool buildTextProgram();
};
