#include "render/overlays/gizmo.h"

#include <glad/gl.h>
#include "render/foundation/shader_utils.h"
#include <vector>
#include <algorithm>
#include <iostream>
#include <QImage>
#include <QPainter>
#include <QFont>
#include <QString>
#include <QOpenGLContext>

// Shaders

// Screen-space AA line: quad vertices carry clip-space pos + color + signed distance.
// Fragment shader uses fwidth/smoothstep on the distance for crisp AA at any DPI.
static const char* aaLineVS = R"(
#version 460 core
layout(location = 0) in vec2 aPos;    // clip-space xy
layout(location = 1) in vec3 aColor;
layout(location = 2) in float aDist;  // signed distance from centerline (pixels)
uniform mat4 uMVP;
out vec3 vColor;
out float vDist;
void main() {
    vColor = aColor;
    vDist = aDist;
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
}
)";
static const char* aaLineFS = R"(
#version 460 core
in vec3 vColor;
in float vDist;
uniform float uHalfWidth;
out vec4 frag;
void main() {
    float d = abs(vDist);
    float aa = fwidth(d);
    float alpha = 1.0 - smoothstep(uHalfWidth - aa, uHalfWidth + aa, d);
    if (alpha < 0.005) discard;
    frag = vec4(vColor, alpha);
}
)";

// Flat BASE_COLOR shader for axis tip cones — always fully opaque, never dark
// on the back side (matches FreeCAD's SoAxisCrossKit lightModel BASE_COLOR).
static const char* capVS = R"(
#version 460 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
uniform vec3 uColor;
out vec3 vColor;
void main() {
    vColor = uColor;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";
static const char* capFS = R"(
#version 460 core
in vec3 vColor;
out vec4 frag;
void main() {
    frag = vec4(vColor, 1.0);
}
)";

// Flat BASE_COLOR shader for the center sphere — always fully opaque, never dark
// on the back side (matches FreeCAD's SoAxisCrossKit lightModel BASE_COLOR).
static const char* sphereVS = R"(
#version 460 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
uniform vec3 uColor;
out vec3 vColor;
void main() {
    vColor = uColor;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";
static const char* sphereFS = R"(
#version 460 core
in vec3 vColor;
out vec4 frag;
void main() {
    frag = vec4(vColor, 1.0);
}
)";

static const char* textVS = R"(
#version 460 core
layout(location = 0) in vec4 aPos;   // xy = local px, zw = uv
layout(location = 1) in vec3 aColor;
uniform mat4 uMVP;
out vec2 vUV;
out vec3 vColor;
void main() {
    vUV = aPos.zw;
    vColor = aColor;
    gl_Position = uMVP * vec4(aPos.xy, 0.0, 1.0);
}
)";
static const char* textFS = R"(
#version 460 core
in vec2 vUV;
in vec3 vColor;
uniform sampler2D uTex;
out vec4 frag;
void main() {
    float a = texture(uTex, vUV).r;   // atlas is white-on-transparent; .r = coverage
    if (a < 0.02) discard;
    frag = vec4(vColor, a);
}
)";

Gizmo::Gizmo() = default;
Gizmo::~Gizmo() { shutdown(); }

// ----------------------------------------------------------------------------
// Atlas: rasterize 'X','Y','Z' into a 3-cell horizontal strip, white-on-clear.
// Uploaded AS-IS: QImage row 0 (glyph top) lands at texture v=0. The V flip that
// makes glyphs upright happens in draw(), not here.
// ----------------------------------------------------------------------------
bool Gizmo::buildAtlas() {
    const char chars[4] = { 'X', 'Y', 'Z', '-' };
    const int cell = 64;
    glyphAtlasW = cell * 4;
    glyphAtlasH = cell;

    QImage img(glyphAtlasW, glyphAtlasH, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        p.setPen(Qt::white);
        QFont f;
        f.setPointSize(33);
        p.setFont(f);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);
        for (int i = 0; i < 4; ++i) {
            p.drawText(QRect(i * cell, 0, cell, cell), Qt::AlignCenter, QString(chars[i]));
        }
    }
    QImage gl = img.convertToFormat(QImage::Format_RGBA8888);
    // NOTE: no vertical mirror here — the atlas already has glyph-top at v=0, and
    // draw() samples v=0 at the quad's top edge, so the glyphs are upright as-is.
    for (int y = 0; y < gl.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(gl.scanLine(y));
        for (int x = 0; x < gl.width(); ++x) {
            if (qAlpha(row[x]) > 0) row[x] = qRgba(255, 255, 255, qAlpha(row[x]));
        }
    }

    glCreateTextures(GL_TEXTURE_2D, 1, glyphTex.ptr());
    glTextureStorage2D(glyphTex, 1, GL_RGBA8, glyphAtlasW, glyphAtlasH);
    glTextureSubImage2D(glyphTex, 0, 0, 0, glyphAtlasW, glyphAtlasH, GL_RGBA, GL_UNSIGNED_BYTE, gl.constBits());
    glTextureParameteri(glyphTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(glyphTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(glyphTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(glyphTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return glyphTex.has();
}

bool Gizmo::buildAALineProgram() {
    aaLineProgram.reset(compileProgram(aaLineVS, aaLineFS, "Gizmo/aaLine"));
    if (!aaLineProgram.has()) return false;
    aaLineMvpLoc       = glGetUniformLocation(aaLineProgram, "uMVP");
    aaLineHalfWidthLoc = glGetUniformLocation(aaLineProgram, "uHalfWidth");
    aaLinePosLoc       = glGetAttribLocation(aaLineProgram, "aPos");
    aaLineColLoc       = glGetAttribLocation(aaLineProgram, "aColor");
    aaLineDistLoc      = glGetAttribLocation(aaLineProgram, "aDist");
    return aaLineMvpLoc >= 0 && aaLineHalfWidthLoc >= 0
        && aaLinePosLoc >= 0 && aaLineColLoc >= 0 && aaLineDistLoc >= 0;
}

bool Gizmo::buildCapProgram() {
    capProgram.reset(compileProgram(capVS, capFS, "Gizmo/cap"));
    if (!capProgram.has()) return false;
    capMvpLoc      = glGetUniformLocation(capProgram, "uMVP");
    capColorLoc    = glGetUniformLocation(capProgram, "uColor");
    return capMvpLoc >= 0 && capColorLoc >= 0;
}

bool Gizmo::buildSphereProgram() {
    sphereProgram.reset(compileProgram(sphereVS, sphereFS, "Gizmo/sphere"));
    if (!sphereProgram.has()) return false;
    sphereMvpLoc     = glGetUniformLocation(sphereProgram, "uMVP");
    sphereColorLoc   = glGetUniformLocation(sphereProgram, "uColor");
    return sphereMvpLoc >= 0 && sphereColorLoc >= 0;
}

bool Gizmo::buildTextProgram() {
    textProgram.reset(compileProgram(textVS, textFS, "Gizmo/text"));
    if (!textProgram.has()) return false;
    textMvpLoc   = glGetUniformLocation(textProgram, "uMVP");
    textTexLoc   = glGetUniformLocation(textProgram, "uTex");
    textPosLoc   = glGetAttribLocation(textProgram, "aPos");
    textColLoc   = glGetAttribLocation(textProgram, "aColor");
    return textMvpLoc >= 0 && textTexLoc >= 0 && textPosLoc >= 0 && textColLoc >= 0;
}

bool Gizmo::init() {
    if (isInitialized()) return true;

    if (!buildAALineProgram() || !buildCapProgram() || !buildSphereProgram()
        || !buildTextProgram() || !buildAtlas())
        return false;

    // ---- AA line VBO (dynamic): 3 axes × 6 verts × (vec2 pos + vec3 col + float dist) ----
    setupVertexBuffer(aaLineVAO, aaLineVBO, nullptr, 3 * 6 * 6 * sizeof(float), 6 * sizeof(float),
                      { { aaLinePosLoc, 2, 0 }, { aaLineColLoc, 3, 2 * sizeof(float) }, { aaLineDistLoc, 1, 5 * sizeof(float) } },
                      GL_DYNAMIC_DRAW);

    // ---- Cap cones (static): 3 axes × 8-sided cone × (vec3 pos + vec3 normal) ----
    {
        const int segments = 8;
        const float coneH = 0.25f;   // height along axis
        const float coneR = 0.15f;   // base radius (matches shaft base width for obvious tip)
        // Each cone: 1 cap triangle fan (segments tris) + side triangles (segments tris)
        // = 2*segments triangles = 6*segments vertices
        capVertCount = 3 * 6 * segments;
        std::vector<float> verts;
        verts.reserve(capVertCount * 6);

        const glm::vec3 axes[3] = { {1,0,0}, {0,1,0}, {0,0,1} };
        const glm::vec3 colors[3] = { {1,0.2f,0.2f}, {0.2f,1,0.2f}, {0.3f,0.5f,1} };

        for (int a = 0; a < 3; ++a) {
            glm::vec3 tip = axes[a] * (1.0f + coneH);  // tip extends beyond axis length 1
            glm::vec3 base = axes[a];                    // base at axis tip
            // Find two perpendicular vectors to the axis
            glm::vec3 perp1, perp2;
            if (std::abs(axes[a].x) < 0.9f)
                perp1 = glm::normalize(glm::cross(axes[a], glm::vec3(1,0,0)));
            else
                perp1 = glm::normalize(glm::cross(axes[a], glm::vec3(0,1,0)));
            perp2 = glm::cross(axes[a], perp1);

            // Side normal: average of (tip-normal) and (side direction) — approximate
            glm::vec3 sideNormal = glm::normalize(tip - base + (perp1 * coneR));

            // Side triangles: each is (tip, base+i, base+i+1) with per-vertex normals
            for (int i = 0; i < segments; ++i) {
                float a0 = (float)i / segments * 6.28318f;
                float a1 = (float)(i + 1) / segments * 6.28318f;
                glm::vec3 p0 = base + (perp1 * std::cos(a0) + perp2 * std::sin(a0)) * coneR;
                glm::vec3 p1 = base + (perp1 * std::cos(a1) + perp2 * std::sin(a1)) * coneR;
                // Tip vertex (normal points outward along axis)
                glm::vec3 nTip = axes[a];
                // Base edge normals (point outward and slightly back)
                glm::vec3 nEdge = glm::normalize(tip - p0);
                glm::vec3 nEdge1 = glm::normalize(tip - p1);
                float v[] = {
                    tip.x, tip.y, tip.z, nTip.x, nTip.y, nTip.z,
                    p0.x, p0.y, p0.z, nEdge.x, nEdge.y, nEdge.z,
                    p1.x, p1.y, p1.z, nEdge1.x, nEdge1.y, nEdge1.z,
                };
                verts.insert(verts.end(), v, v + 18);
            }
            // Cap fan triangles (base face, normal points backward).
            // Exactly `segments` triangles — i from 0 wraps naturally at 2π, keeping
            // the emitted vertex count in lockstep with capVertCount (48 per axis).
            // An earlier [1, segments) loop emitted one triangle fewer than the
            // draw call assumed and walked off the end of this buffer.
            glm::vec3 capN = -axes[a];
            for (int i = 0; i < segments; ++i) {
                float a0 = (float)i / segments * 6.28318f;
                float a1 = (float)(i + 1) / segments * 6.28318f;
                glm::vec3 p0 = base + (perp1 * std::cos(a0) + perp2 * std::sin(a0)) * coneR;
                glm::vec3 p1 = base + (perp1 * std::cos(a1) + perp2 * std::sin(a1)) * coneR;
                float v[] = {
                    base.x, base.y, base.z, capN.x, capN.y, capN.z,
                    p0.x, p0.y, p0.z, capN.x, capN.y, capN.z,
                    p1.x, p1.y, p1.z, capN.x, capN.y, capN.z,
                };
                verts.insert(verts.end(), v, v + 18);
            }
        }

        setupVertexBuffer(capVAO, capVBO, verts.data(), verts.size() * sizeof(float), 6 * sizeof(float),
                          { { 0, 3, 0 }, { 1, 3, 3 * sizeof(float) } }, GL_STATIC_DRAW);
    }

    // ---- Center sphere (static): UV sphere with normals for metallic lighting ----
    {
        const int latSegments = 24;
        const int lonSegments = 36;
        const float radius = 0.12f;
        std::vector<float> verts;

        for (int lat = 0; lat < latSegments; ++lat) {
            float theta0 = (float)lat * 3.14159265f / latSegments;
            float theta1 = (float)(lat + 1) * 3.14159265f / latSegments;
            for (int lon = 0; lon < lonSegments; ++lon) {
                float phi0 = (float)lon * 2.0f * 3.14159265f / lonSegments;
                float phi1 = (float)(lon + 1) * 2.0f * 3.14159265f / lonSegments;

                auto vertex = [&](float theta, float phi) {
                    float x = std::sin(theta) * std::cos(phi);
                    float y = std::cos(theta);
                    float z = std::sin(theta) * std::sin(phi);
                    // position
                    verts.push_back(x * radius);
                    verts.push_back(y * radius);
                    verts.push_back(z * radius);
                    // normal
                    verts.push_back(x);
                    verts.push_back(y);
                    verts.push_back(z);
                };

                // Triangle 1: (lat,lon) -> (lat+1,lon) -> (lat,lon+1)
                vertex(theta0, phi0);
                vertex(theta1, phi0);
                vertex(theta0, phi1);
                // Triangle 2: (lat+1,lon) -> (lat+1,lon+1) -> (lat,lon+1)
                vertex(theta1, phi0);
                vertex(theta1, phi1);
                vertex(theta0, phi1);
            }
        }

        originVertCount = static_cast<int>(verts.size() / 6);
        GLint posLoc = glGetAttribLocation(sphereProgram, "aPos");
        GLint norLoc = glGetAttribLocation(sphereProgram, "aNormal");
        setupVertexBuffer(originVAO, originVBO, verts.data(), verts.size() * sizeof(float), 6 * sizeof(float),
                          { { posLoc, 3, 0 }, { norLoc, 3, 3 * sizeof(float) } }, GL_STATIC_DRAW);
    }

    // ---- Text quad VBO (dynamic): 3 chars × 6 verts × (vec4 px.xy+uv + vec3 color) ----
    setupVertexBuffer(textVAO, textVBO, nullptr, 3 * 6 * 7 * sizeof(float), 7 * sizeof(float),
                      { { textPosLoc, 4, 0 }, { textColLoc, 3, 4 * sizeof(float) } }, GL_DYNAMIC_DRAW);

    return true;
}

void Gizmo::shutdown() {
    if (!QOpenGLContext::currentContext()) return;
    aaLineVAO.reset(); aaLineVBO.reset(); aaLineProgram.reset();
    capVAO.reset(); capVBO.reset(); capProgram.reset();
    originVAO.reset(); originVBO.reset();
    sphereProgram.reset();
    textVAO.reset(); textVBO.reset(); textProgram.reset();
    glyphTex.reset();
}

glm::ivec2 Gizmo::rectOrigin(int deviceW, int deviceH, float dpr, int foot, int corner) {
    const int s = static_cast<int>(foot * dpr);
    const int m = static_cast<int>(kMarginPx * dpr);
    const bool right = (corner == BottomRight || corner == TopRight);
    const bool top   = (corner == TopLeft     || corner == TopRight);
    return { right ? deviceW - m - s : m,
             top   ? deviceH - m - s : m };  // GL convention: origin bottom-left
}

void Gizmo::draw(const glm::mat4& mainView, float dpr,
                  int deviceW, int deviceH,
                  int corner, int foot, int hoverAxis,
                  bool showNegativeAxes) {
    if (!isInitialized()) return;

    // Save engine state we mutate; restored automatically on scope exit.
    GLStateGuard guard;

    const int s = static_cast<int>(foot * dpr);
    const glm::ivec2 ro = rectOrigin(deviceW, deviceH, dpr, foot, corner);
    glViewport(ro.x, ro.y, s, s);

    const glm::mat4 gizmoView = glm::mat4(glm::mat3(mainView));
    const glm::mat4 gizmoProj = glm::ortho(-1.55f, 1.55f, -1.55f, 1.55f, -10.0f, 10.0f);
    const glm::mat4 lineMVP = gizmoProj * gizmoView;

    // Preserve the active clip origin, force the negative-one-to-one depth range
    // the gizmo's ortho projection expects; the guard restores both on exit.
    GLint curClipOrigin = GL_UPPER_LEFT;
    glGetIntegerv(GL_CLIP_ORIGIN, &curClipOrigin);
    glClipControl(static_cast<GLenum>(curClipOrigin), GL_NEGATIVE_ONE_TO_ONE);
    // Screen-space overlay: never depth-test against the main scene. The main
    // depth buffer holds scene geometry whose window-z beats the gizmo's fixed
    // mid-range depth (its ortho maps z=0 to 0.5) under linear orthographic
    // projection, so mesh fragments covering the corner erased the axes in
    // parallel camera. Sections below draw in painter order; no depth needed.
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // ---- axis triad: center sphere + thick arrow shafts + conical tips ----
    const float clipPerPx = 3.1f / static_cast<float>(foot);
    const float shaftHalfWidthOrigin = 0.5f;  // thinner at center/pivot
    const float shaftHalfWidthBase = 1.2f;    // wider at cone base
    const float hoverHalfWidthOrigin = 0.8f;  // hovered width at origin
    const float hoverHalfWidthBase = 1.8f;    // hovered width at cone
    const float sheenHalfWidth = 0.3f;        // thin center highlight strip
    const float sheenHalfWidthHover = 0.5f;   // thin center highlight strip (hovered)
    const float shaftRatio = 0.8f;       // shaft ends at 80%, cone covers last 20%

    const glm::vec3 axes[3] = { {1,0,0}, {0,1,0}, {0,0,1} };
    const float baseColors[3][3] = { {1,0.2f,0.2f}, {0.2f,1,0.2f}, {0.3f,0.5f,1.0f} };
    const glm::vec3 gold(1.0f, 0.84f, 0.0f);  

    auto axisColor = [&](int axisIdx, bool isHovered) -> glm::vec3 {
        if (isHovered) return gold;
        return glm::vec3(baseColors[axisIdx][0], baseColors[axisIdx][1], baseColors[axisIdx][2]);
    };

    bool hoveredPos[3] = {
        hoverAxis >= 0 && hoverAxis == 0,
        hoverAxis >= 0 && hoverAxis == 2,
        hoverAxis >= 0 && hoverAxis == 4 };
    bool hoveredNeg[3] = {
        hoverAxis >= 0 && hoverAxis == 1,
        hoverAxis >= 0 && hoverAxis == 3,
        hoverAxis >= 0 && hoverAxis == 5 };

    // === Arrow shafts: thick screen-space quads from origin to shaftRatio * axis ===
    glUseProgram(aaLineProgram);
    glUniformMatrix4fv(aaLineMvpLoc, 1, GL_FALSE, glm::value_ptr(glm::mat4(1.0f)));
    glBindVertexArray(aaLineVAO);

    bool axisVisible[3] = { true, true, true };
    for (int i = 0; i < 3; ++i) {
        // Positive axis shaft (tapered: thinner at center, wider at cone) + center sheen
        {
            bool hovered = hoveredPos[i];
            float hwOrigin = hovered ? hoverHalfWidthOrigin : shaftHalfWidthOrigin;
            float hwBase = hovered ? hoverHalfWidthBase : shaftHalfWidthBase;
            float hwClipOrigin = hwOrigin * clipPerPx;
            float hwClipBase = hwBase * clipPerPx;
            glm::vec3 color = axisColor(i, hovered);
            glm::vec3 sheenColor = glm::min(color * 0.5f + 0.5f, glm::vec3(1.0f));

            glm::vec4 cA = lineMVP * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
            glm::vec4 cB = lineMVP * glm::vec4(axes[i] * shaftRatio, 1.0f);
            glm::vec2 a(cA), b(cB);
            glm::vec2 dir = b - a;
            float len = glm::length(dir);

            if (len < 1e-4f) {
                axisVisible[i] = false;
                float q[] = {
                    a.x, a.y, color.r, color.g, color.b, 0.0f,
                    a.x, a.y, color.r, color.g, color.b, 0.0f,
                    a.x, a.y, color.r, color.g, color.b, 0.0f,
                    a.x, a.y, color.r, color.g, color.b, 0.0f,
                    a.x, a.y, color.r, color.g, color.b, 0.0f,
                    a.x, a.y, color.r, color.g, color.b, 0.0f,
                };
                glUniform1f(aaLineHalfWidthLoc, hwOrigin);
                glNamedBufferSubData(aaLineVBO, 0, sizeof(q), q);
                glDrawArrays(GL_TRIANGLES, 0, 6);
                continue;
            }

            glm::vec2 perp(-dir.y / len, dir.x / len);
            glm::vec2 aL = a - perp * hwClipOrigin;
            glm::vec2 aR = a + perp * hwClipOrigin;
            glm::vec2 bL = b - perp * hwClipBase;
            glm::vec2 bR = b + perp * hwClipBase;

            // Base tapered shaft quad
            float q[] = {
                aL.x, aL.y, color.r, color.g, color.b, -hwOrigin,
                bL.x, bL.y, color.r, color.g, color.b, -hwBase,
                bR.x, bR.y, color.r, color.g, color.b,  hwBase,
                aL.x, aL.y, color.r, color.g, color.b, -hwOrigin,
                bR.x, bR.y, color.r, color.g, color.b,  hwBase,
                aR.x, aR.y, color.r, color.g, color.b,  hwOrigin,
            };
            glUniform1f(aaLineHalfWidthLoc, hwBase);
            glNamedBufferSubData(aaLineVBO, 0, sizeof(q), q);
            glDrawArrays(GL_TRIANGLES, 0, 6);

            // Sheen highlight: thinner lighter strip down shaft center
            float sheenHw = hovered ? sheenHalfWidthHover : sheenHalfWidth;
            float sheenClip = sheenHw * clipPerPx;
            glm::vec2 aLs = a - perp * sheenClip;
            glm::vec2 aRs = a + perp * sheenClip;
            glm::vec2 bLs = b - perp * sheenClip;
            glm::vec2 bRs = b + perp * sheenClip;
            float qs[] = {
                aLs.x, aLs.y, sheenColor.r, sheenColor.g, sheenColor.b, -sheenHw,
                bLs.x, bLs.y, sheenColor.r, sheenColor.g, sheenColor.b, -sheenHw,
                bRs.x, bRs.y, sheenColor.r, sheenColor.g, sheenColor.b,  sheenHw,
                aLs.x, aLs.y, sheenColor.r, sheenColor.g, sheenColor.b, -sheenHw,
                bRs.x, bRs.y, sheenColor.r, sheenColor.g, sheenColor.b,  sheenHw,
                aRs.x, aRs.y, sheenColor.r, sheenColor.g, sheenColor.b,  sheenHw,
            };
            glUniform1f(aaLineHalfWidthLoc, sheenHw);
            glNamedBufferSubData(aaLineVBO, 0, sizeof(qs), qs);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        // Negative axis shaft (shown on hover, or always when enabled) — tapered + sheen
        if (showNegativeAxes || hoveredNeg[i]) {
            float hwOrigin = hoveredNeg[i] ? hoverHalfWidthOrigin : shaftHalfWidthOrigin;
            float hwBase = hoveredNeg[i] ? hoverHalfWidthBase : shaftHalfWidthBase;
            float hwClipOrigin = hwOrigin * clipPerPx;
            float hwClipBase = hwBase * clipPerPx;
            glm::vec3 color = axisColor(i, hoveredNeg[i]);
            glm::vec3 sheenColor = glm::min(color * 0.5f + 0.5f, glm::vec3(1.0f));

            glm::vec4 cA = lineMVP * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
            glm::vec4 cB = lineMVP * glm::vec4(-axes[i] * shaftRatio, 1.0f);
            glm::vec2 a(cA), b(cB);
            glm::vec2 dir = b - a;
            float len = glm::length(dir);

            if (len < 1e-4f) continue;

            glm::vec2 perp(-dir.y / len, dir.x / len);
            glm::vec2 aL = a - perp * hwClipOrigin;
            glm::vec2 aR = a + perp * hwClipOrigin;
            glm::vec2 bL = b - perp * hwClipBase;
            glm::vec2 bR = b + perp * hwClipBase;

            // Base tapered shaft quad
            float q[] = {
                aL.x, aL.y, color.r, color.g, color.b, -hwOrigin,
                bL.x, bL.y, color.r, color.g, color.b, -hwBase,
                bR.x, bR.y, color.r, color.g, color.b,  hwBase,
                aL.x, aL.y, color.r, color.g, color.b, -hwOrigin,
                bR.x, bR.y, color.r, color.g, color.b,  hwBase,
                aR.x, aR.y, color.r, color.g, color.b,  hwOrigin,
            };
            glUniform1f(aaLineHalfWidthLoc, hwBase);
            glNamedBufferSubData(aaLineVBO, 0, sizeof(q), q);
            glDrawArrays(GL_TRIANGLES, 0, 6);

            // Sheen highlight: thinner lighter strip down shaft center
            float sheenHw = hoveredNeg[i] ? sheenHalfWidthHover : sheenHalfWidth;
            float sheenClip = sheenHw * clipPerPx;
            glm::vec2 aLs = a - perp * sheenClip;
            glm::vec2 aRs = a + perp * sheenClip;
            glm::vec2 bLs = b - perp * sheenClip;
            glm::vec2 bRs = b + perp * sheenClip;
            float qs[] = {
                aLs.x, aLs.y, sheenColor.r, sheenColor.g, sheenColor.b, -sheenHw,
                bLs.x, bLs.y, sheenColor.r, sheenColor.g, sheenColor.b, -sheenHw,
                bRs.x, bRs.y, sheenColor.r, sheenColor.g, sheenColor.b,  sheenHw,
                aLs.x, aLs.y, sheenColor.r, sheenColor.g, sheenColor.b, -sheenHw,
                bRs.x, bRs.y, sheenColor.r, sheenColor.g, sheenColor.b,  sheenHw,
                aRs.x, aRs.y, sheenColor.r, sheenColor.g, sheenColor.b,  sheenHw,
            };
            glUniform1f(aaLineHalfWidthLoc, sheenHw);
            glNamedBufferSubData(aaLineVBO, 0, sizeof(qs), qs);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
    }
    glBindVertexArray(0);

    // === Cone arrowheads: static geometry scaled by shaftRatio
    //    base moves from axis*1.0 to axis*shaftRatio, tip from axis*1.25 to axis*1.0 (shaftRatio=0.8) ===
    glUseProgram(capProgram);
    glBindVertexArray(capVAO);
    const int conePerAxis = capVertCount / 3;
    for (int i = 0; i < 3; ++i) {
        // Positive cone
        {
            glm::vec3 color = axisColor(i, hoveredPos[i]);
            float coneScale = hoveredPos[i] ? shaftRatio * 1.25f : shaftRatio;
            // Pin cone base at shaft end so hover grows outward, not from origin
            glm::mat4 coneModel = glm::translate(glm::mat4(1.0f), axes[i] * (shaftRatio - coneScale))
                                * glm::scale(glm::mat4(1.0f), glm::vec3(coneScale));
            glm::mat4 coneMVP = lineMVP * coneModel;
            glUniformMatrix4fv(capMvpLoc, 1, GL_FALSE, glm::value_ptr(coneMVP));
            glUniform3fv(capColorLoc, 1, glm::value_ptr(color));
            glDrawArrays(GL_TRIANGLES, i * conePerAxis, conePerAxis);
        }
        // Negative cone (shown on hover, or always when enabled)
        if (showNegativeAxes || hoveredNeg[i]) {
            glm::vec3 color = axisColor(i, hoveredNeg[i]);
            float coneScale = hoveredNeg[i] ? shaftRatio * 1.25f : shaftRatio;
            glm::mat4 reflect(1.0f);
            reflect[i][i] = -1.0f;
            // Pin cone base at shaft end (negative direction) so hover grows outward
            glm::mat4 coneModel = reflect * glm::translate(glm::mat4(1.0f), axes[i] * (shaftRatio - coneScale))
                                * glm::scale(glm::mat4(1.0f), glm::vec3(coneScale));
            glm::mat4 coneMVP = lineMVP * coneModel;
            glUniformMatrix4fv(capMvpLoc, 1, GL_FALSE, glm::value_ptr(coneMVP));
            glUniform3fv(capColorLoc, 1, glm::value_ptr(color));
            glDrawArrays(GL_TRIANGLES, i * conePerAxis, conePerAxis);
        }
    }
    glBindVertexArray(0);

    // === Pole-view axis markers: small disc at origin for axes viewed end-on ===
    {
        const float poleMarkerRadius = 0.08f;
        constexpr int kSegs = 16;
        std::vector<float> markerVerts;
        for (int i = 0; i < 3; ++i) {
            if (axisVisible[i]) continue;
            const glm::vec3 col = axisColor(i, hoveredPos[i] || hoveredNeg[i]);
            for (int t = 0; t < kSegs; ++t) {
                float a0 = (float)t / kSegs * 6.28318f;
                float a1 = (float)(t + 1) / kSegs * 6.28318f;
                markerVerts.insert(markerVerts.end(), { 0.0f, 0.0f, col.r, col.g, col.b, 0.0f });
                markerVerts.insert(markerVerts.end(), { std::cos(a0) * poleMarkerRadius, std::sin(a0) * poleMarkerRadius, col.r, col.g, col.b, 0.0f });
                markerVerts.insert(markerVerts.end(), { std::cos(a1) * poleMarkerRadius, std::sin(a1) * poleMarkerRadius, col.r, col.g, col.b, 0.0f });
            }
        }
        if (!markerVerts.empty()) {
            glUseProgram(aaLineProgram);
            glUniformMatrix4fv(aaLineMvpLoc, 1, GL_FALSE, glm::value_ptr(glm::mat4(1.0f)));
            glUniform1f(aaLineHalfWidthLoc, shaftHalfWidthOrigin);
            glBindVertexArray(aaLineVAO);
            glNamedBufferSubData(aaLineVBO, 0, static_cast<GLsizeiptr>(markerVerts.size() * sizeof(float)), markerVerts.data());
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(markerVerts.size() / 6));
            glBindVertexArray(0);
        }
    }

    // ---- Text labels (per-vertex color, batched) ----
    const glm::mat4 textMVP = glm::ortho(0.0f, (float)foot, 0.0f, (float)foot, -1.0f, 1.0f);
    const float cellU = 1.0f / 4.0f;  // 4 cells: X, Y, Z, -
    const float glyph = 20.0f; 
    const float half = glyph * 0.5f;
    const float gap = -10.0f;  // minus sign attached directly to letter (no gap)

    // Positive labels: single quad per axis, cell i (X/Y/Z)
    float quads[3][6][7];
    for (int i = 0; i < 3; ++i) {
        glm::vec4 clip = gizmoProj * gizmoView * glm::vec4(axes[i], 1.0f);
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        float f = 1.2f;
        ndc.x *= f; ndc.y *= f;
        float cx = (ndc.x * 0.5f + 0.5f) * foot;
        float cy = (ndc.y * 0.5f + 0.5f) * foot;
        cx = std::max(half, std::min(foot - half, cx));
        cy = std::max(half, std::min(foot - half, cy));

        bool isPole = std::abs(ndc.z) > 0.99f;
        if (isPole) {
            float offsetDist = glyph * 1.5f;
            if (i == 2) {
                cx += offsetDist * 0.707f;
                cy += offsetDist * 0.707f;
            } else if (i == 0) {
                cy += (ndc.y >= 0 ? offsetDist : -offsetDist);
            } else {
                cx += (ndc.x >= 0 ? offsetDist : -offsetDist);
            }
            cx = std::max(half, std::min(foot - half, cx));
            cy = std::max(half, std::min(foot - half, cy));
        }

        float u0 = i * cellU, u1 = (i + 1) * cellU;
        const float vTop = 0.0f, vBot = 1.0f;
        const float hx = glyph * 0.5f, hy = glyph * 0.5f;
        const glm::vec3 labCol = axisColor(i, hoveredPos[i]);
        float tri[6][7] = {
            { cx - hx, cy + hy, u0, vTop, labCol.r, labCol.g, labCol.b },
            { cx + hx, cy + hy, u1, vTop, labCol.r, labCol.g, labCol.b },
            { cx - hx, cy - hy, u0, vBot, labCol.r, labCol.g, labCol.b },
            { cx + hx, cy + hy, u1, vTop, labCol.r, labCol.g, labCol.b },
            { cx + hx, cy - hy, u1, vBot, labCol.r, labCol.g, labCol.b },
            { cx - hx, cy - hy, u0, vBot, labCol.r, labCol.g, labCol.b },
        };
        std::memcpy(quads[i], tri, sizeof(tri));
    }

    glUseProgram(textProgram);
    glUniformMatrix4fv(textMvpLoc, 1, GL_FALSE, glm::value_ptr(textMVP));
    glUniform1i(textTexLoc, 0);
    glBindTextureUnit(0, glyphTex);
    glBindVertexArray(textVAO);
    glNamedBufferSubData(textVBO, 0, sizeof(quads), quads);
    glDrawArrays(GL_TRIANGLES, 0, 3 * 6);  // 3 label quads

    // Negative labels: two quads per axis (minus sign cell 3 + letter cell a)
    for (int a = 0; a < 3; ++a) {
        if (!(showNegativeAxes || hoveredNeg[a])) continue;

        glm::vec3 unit = (a == 0) ? glm::vec3(1,0,0) : (a == 1) ? glm::vec3(0,1,0) : glm::vec3(0,0,1);
        glm::vec4 clip = gizmoProj * gizmoView * glm::vec4(-unit, 1.0f);
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        float f = 1.20f;
        ndc.x *= f; ndc.y *= f;
        float cx = (ndc.x * 0.5f + 0.5f) * foot;
        float cy = (ndc.y * 0.5f + 0.5f) * foot;
        const float halfg = glyph * 0.5f;
        cx = std::max(halfg, std::min(foot - halfg, cx));
        cy = std::max(halfg, std::min(foot - halfg, cy));

        bool isPole = std::abs(ndc.z) > 0.99f;
        if (isPole) {
            float offsetDist = glyph * 1.5f;
            if (a == 2) { cx += offsetDist * 0.707f; cy += offsetDist * 0.707f; }
            else if (a == 0) { cy += (ndc.y >= 0 ? offsetDist : -offsetDist); }
            else { cx += (ndc.x >= 0 ? offsetDist : -offsetDist); }
            cx = std::max(halfg, std::min(foot - halfg, cx));
            cy = std::max(halfg, std::min(foot - halfg, cy));
        }

        const glm::vec3 labCol = axisColor(a, hoveredNeg[a]);

        // Minus-sign quad: cell 3, placed left of the letter center
        {
            float u0 = 3 * cellU, u1 = 4 * cellU;
            float mx = cx - glyph - gap;
            float tri[6][7] = {
                { mx - halfg, cy + halfg, u0, 0.0f, labCol.r, labCol.g, labCol.b },
                { mx + halfg, cy + halfg, u1, 0.0f, labCol.r, labCol.g, labCol.b },
                { mx - halfg, cy - halfg, u0, 1.0f, labCol.r, labCol.g, labCol.b },
                { mx + halfg, cy + halfg, u1, 0.0f, labCol.r, labCol.g, labCol.b },
                { mx + halfg, cy - halfg, u1, 1.0f, labCol.r, labCol.g, labCol.b },
                { mx - halfg, cy - halfg, u0, 1.0f, labCol.r, labCol.g, labCol.b },
            };
            glNamedBufferSubData(textVBO, 0, sizeof(tri), tri);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        // Letter quad: cell a, at the label center
        {
            float u0 = a * cellU, u1 = (a + 1) * cellU;
            float tri[6][7] = {
                { cx - halfg, cy + halfg, u0, 0.0f, labCol.r, labCol.g, labCol.b },
                { cx + halfg, cy + halfg, u1, 0.0f, labCol.r, labCol.g, labCol.b },
                { cx - halfg, cy - halfg, u0, 1.0f, labCol.r, labCol.g, labCol.b },
                { cx + halfg, cy + halfg, u1, 0.0f, labCol.r, labCol.g, labCol.b },
                { cx + halfg, cy - halfg, u1, 1.0f, labCol.r, labCol.g, labCol.b },
                { cx - halfg, cy - halfg, u0, 1.0f, labCol.r, labCol.g, labCol.b },
            };
            glNamedBufferSubData(textVBO, 0, sizeof(tri), tri);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
    }

    // === Center sphere (flat yellow) - drawn last, on top of all arrows ===
    {
        glm::mat4 model(1.0f);  // sphere is at origin, no extra transform
        glm::vec3 sphereColor(1.0f, 0.95f, 0.2f);  // Brighter gold yellow

        glUseProgram(sphereProgram);
        glUniformMatrix4fv(sphereMvpLoc, 1, GL_FALSE, glm::value_ptr(lineMVP));
        glUniform3fv(sphereColorLoc, 1, glm::value_ptr(sphereColor));
        glBindVertexArray(originVAO);
        glDrawArrays(GL_TRIANGLES, 0, originVertCount);
        glBindVertexArray(0);
    }

    glBindVertexArray(0);
    glUseProgram(0);
}

int Gizmo::hitTestAxis(const glm::mat4& mainView, float dpr,
                       int deviceW, int deviceH,
                       float xDevPx, float yDevPx,
                       int corner, int foot) {
    const glm::ivec2 ro = rectOrigin(deviceW, deviceH, dpr, foot, corner);
    const int s = static_cast<int>(foot * dpr);
    // Click arrives top-left (Qt); the square lives bottom-left (GL). Convert.
    const float gx = xDevPx - static_cast<float>(ro.x);
    const float gy = static_cast<float>(deviceH) - yDevPx - static_cast<float>(ro.y);
    if (gx < 0.0f || gy < 0.0f || gx > static_cast<float>(s) || gy > static_cast<float>(s))
        return -1;

    // Local clip coords — mirrors glm::ortho(-1.55,1.55,...) over [0,s] px,
    // i.e. the exact inverse of the viewport transform draw() relies on.
    constexpr float kExtent = 1.55f;
    const float inv = (2.0f * kExtent) / static_cast<float>(s);
    const glm::vec2 click(gx * inv - kExtent, gy * inv - kExtent);

    // Dead zone around the pivot: avoid unstable selection near the origin.
    // The center sphere is larger than the old origin disc, so widen the dead zone.
    const float deadZoneClip = 10.0f * (2.0f * kExtent) / static_cast<float>(foot);
    if (glm::length(click) < deadZoneClip) return -1;

    // Same matrices as draw(): rotation-only view + fixed ortho.
    const glm::mat4 mvp = glm::ortho(-kExtent, kExtent, -kExtent, kExtent, -10.0f, 10.0f)
                        * glm::mat4(glm::mat3(mainView));

    // ~14 logical px of slack around each shaft/cone/label zone.
    const float thresh = 14.0f * (2.0f * kExtent) / static_cast<float>(foot);

    const glm::vec3 tips[6] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
    // Label geometry (logical px) mirrors draw(): glyph 24, f=1.40, clamp.
    const float glyph = 24.0f;
    const float halfG = glyph * 0.5f;
    const float fLabel = 1.40f;

    // Logical click coords for label tests.
    const float lx = gx / dpr;
    const float ly = gy / dpr;

    // Label hit test first (point-in-quad), for all 6 directions.
    for (int i = 0; i < 6; ++i) {
        int a = i / 2;
        glm::vec3 unit = (a == 0) ? glm::vec3(1,0,0) : (a == 1) ? glm::vec3(0,1,0) : glm::vec3(0,0,1);
        glm::vec3 tip = (i % 2 == 0) ? unit : -unit;
        glm::vec4 clip = mvp * glm::vec4(tip, 1.0f);
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        ndc.x *= fLabel; ndc.y *= fLabel;
        float cx = (ndc.x * 0.5f + 0.5f) * foot;
        float cy = (ndc.y * 0.5f + 0.5f) * foot;
        cx = std::max(halfG, std::min(foot - halfG, cx));
        cy = std::max(halfG, std::min(foot - halfG, cy));
        bool isPole = std::abs(ndc.z) > 0.99f;
        if (isPole) {
            float offsetDist = glyph * 1.5f;
            if (a == 2) { cx += offsetDist * 0.707f; cy += offsetDist * 0.707f; }
            else if (a == 0) { cy += (ndc.y >= 0 ? offsetDist : -offsetDist); }
            else { cx += (ndc.x >= 0 ? offsetDist : -offsetDist); }
            cx = std::max(halfG, std::min(foot - halfG, cx));
            cy = std::max(halfG, std::min(foot - halfG, cy));
        }
        // Label hit test: negative labels (-X/-Y/-Z) extend left with a minus-sign quad.
        const float labelLeft = (i % 2 == 1) ? cx - glyph - 4.0f - halfG : cx - halfG;
        if (lx >= labelLeft && lx <= cx + halfG && ly >= cy - halfG && ly <= cy + halfG) {
            return i;
        }
    }

    // Shaft hit test with depth tie-break.
    int best = -1;
    float bestD = thresh;
    float bestZ = 0.0f;
    bool have = false;
    for (int i = 0; i < 6; ++i) {
        glm::vec4 oClip = mvp * glm::vec4(0.0f,0.0f,0.0f,1.0f);
        glm::vec4 tClip = mvp * glm::vec4(tips[i], 1.0f);
        glm::vec2 t(tClip.x, tClip.y); // w=1
        const float len2 = glm::dot(t, t);
        float u = (len2 > 1e-12f) ? glm::dot(click, t) / len2 : -1.0f;
        u = glm::clamp(u, 0.0f, 1.45f);
        const float d = glm::length(click - t * u);
        if (d >= thresh) continue;
        float originZ = oClip.z; // w=1
        float tipZ = tClip.z;
        float pointZ = originZ + u * (tipZ - originZ);
        if (!have || d < bestD - 1e-5f || (std::fabs(d - bestD) <= 1e-5f && pointZ < bestZ)) {
            best = i;
            bestD = d;
            bestZ = pointZ;
            have = true;
        }
    }
    return best;
}

