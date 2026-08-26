#include "render/overlays/gizmo.h"

#include <glad/gl.h>
#include "render/foundation/shader_utils.h"
#include <vector>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <QImage>
#include <QPainter>
#include <QFont>
#include <QString>
#include <QOpenGLContext>

// ----------------------------------------------------------------------------
// Shaders
// ----------------------------------------------------------------------------
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

// Simple solid-color shader for the origin disc (the light-marker discs live in
// LightMarkerOverlay; this tiny program is triad-local).
static const char* discVS = R"(
#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
uniform mat4 uMVP;
out vec3 vColor;
void main() {
    vColor = aColor;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";
static const char* discFS = R"(
#version 460 core
in vec3 vColor;
out vec4 frag;
void main() { frag = vec4(vColor, 1.0); }
)";

// Simple solid-color shader for axis tip cones.
static const char* capVS = R"(
#version 460 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";
static const char* capFS = R"(
#version 460 core
uniform vec3 uColor;
out vec4 frag;
void main() { frag = vec4(uColor, 1.0); }
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
    const char chars[3] = { 'X', 'Y', 'Z' };
    const int cell = 64;
    glyphAtlasW = cell * 3;
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
        for (int i = 0; i < 3; ++i) {
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
    capMvpLoc   = glGetUniformLocation(capProgram, "uMVP");
    capColorLoc = glGetUniformLocation(capProgram, "uColor");
    return capMvpLoc >= 0 && capColorLoc >= 0;
}

bool Gizmo::buildDiscProgram() {
    discProgram.reset(compileProgram(discVS, discFS, "Gizmo/disc"));
    if (!discProgram.has()) return false;
    discMvpLoc = glGetUniformLocation(discProgram, "uMVP");
    GLint posLoc = glGetAttribLocation(discProgram, "aPos");
    GLint colLoc = glGetAttribLocation(discProgram, "aColor");
    return discMvpLoc >= 0 && posLoc >= 0 && colLoc >= 0;
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

    if (!buildAALineProgram() || !buildCapProgram() || !buildDiscProgram()
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
        const float coneR = 0.08f;   // base radius
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
                          { { 0, 3, 0 } }, GL_STATIC_DRAW);
    }

    // ---- Origin disc (static): 12-sided disc at pivot, white per-vertex ----
    {
        const int segments = 12;
        const float radius = 0.07f;
        originVertCount = segments * 3;
        std::vector<float> verts;
        verts.reserve(originVertCount * 6);
        for (int i = 0; i < segments; ++i) {
            float a0 = (float)i / segments * 6.28318f;
            float a1 = (float)(i + 1) / segments * 6.28318f;
            if (i + 1 == segments) a1 = 0.0f;
            // vec3 pos + vec3 color (white)
            float v[] = {
                0.0f, 0.0f, 0.0f,    0.85f, 0.85f, 0.85f,
                std::cos(a0) * radius, std::sin(a0) * radius, 0.0f,  0.85f, 0.85f, 0.85f,
                std::cos(a1) * radius, std::sin(a1) * radius, 0.0f,  0.85f, 0.85f, 0.85f,
            };
            verts.insert(verts.end(), v, v + 18);
        }
        GLint lmPosLoc2 = glGetAttribLocation(discProgram, "aPos");
        GLint lmColLoc2 = glGetAttribLocation(discProgram, "aColor");
        setupVertexBuffer(originVAO, originVBO, verts.data(), verts.size() * sizeof(float), 6 * sizeof(float),
                          { { lmPosLoc2, 3, 0 }, { lmColLoc2, 3, 3 * sizeof(float) } }, GL_STATIC_DRAW);
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
    discProgram.reset();
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
                 int corner, int foot, int hoverAxis) {
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

    // ---- Screen-space AA axis lines ----
    // ponytail: clip-space expansion calibrated for ortho [-1.55,1.55] → [0,foot] px
    const float clipPerPx = 3.1f / static_cast<float>(foot);
    const float halfWidth = 0.6f;  // ~1.2px total
    const float hwClip = halfWidth * clipPerPx;

    const glm::vec3 origins[3] = { {0,0,0}, {0,0,0}, {0,0,0} };
    const glm::vec3 tips[3]    = { {1,0,0}, {0,1,0}, {0,0,1} };
    const float colors[3][3]   = { {1,0.2f,0.2f}, {0.2f,1,0.2f}, {0.3f,0.5f,1.0f} };

    // Hover feedback: pull the axis color toward white so the hovered shaft,
    // cone, and label read as one active element.
    auto axisColor = [&](int i) -> glm::vec3 {
        glm::vec3 c(colors[i][0], colors[i][1], colors[i][2]);
        if (i == hoverAxis) c = glm::min(c * 0.45f + 0.55f, glm::vec3(1.0f));
        return c;
    };

    // Per-axis quad: 6 verts (2 triangles) × (vec2 pos + vec3 col + float dist) = 36 floats
    float quadData[3][36];
    bool axisVisible[3] = { true, true, true };

    for (int i = 0; i < 3; ++i) {
        glm::vec4 cA = lineMVP * glm::vec4(origins[i], 1.0f);
        glm::vec4 cB = lineMVP * glm::vec4(tips[i], 1.0f);
        // Ortho: w=1, just use xy
        glm::vec2 a(cA), b(cB);
        glm::vec2 dir = b - a;
        float len = glm::length(dir);

        // Detect pole view: axis tip projects to near-origin (viewing down that axis)
        if (len < 1e-4f) {
            axisVisible[i] = false;
            // Fill with degenerate quad (will be no-op draw)
            glm::vec3 col = axisColor(i);
            float q[] = {
                a.x, a.y, col.r, col.g, col.b, 0.0f,
                a.x, a.y, col.r, col.g, col.b, 0.0f,
                a.x, a.y, col.r, col.g, col.b, 0.0f,
                a.x, a.y, col.r, col.g, col.b, 0.0f,
                a.x, a.y, col.r, col.g, col.b, 0.0f,
                a.x, a.y, col.r, col.g, col.b, 0.0f,
            };
            std::memcpy(quadData[i], q, sizeof(q));
            continue;
        }
        glm::vec2 perp(-dir.y / len, dir.x / len);  // perpendicular in clip space

        glm::vec2 aL = a - perp * hwClip;
        glm::vec2 aR = a + perp * hwClip;
        glm::vec2 bL = b - perp * hwClip;
        glm::vec2 bR = b + perp * hwClip;

        glm::vec3 col = axisColor(i);
        // Two triangles: (aL,bL,bR) and (aL,bR,aR)
        // vDist: -halfWidth at left edge, +halfWidth at right edge
        float q[] = {
            aL.x, aL.y, col.r, col.g, col.b, -halfWidth,
            bL.x, bL.y, col.r, col.g, col.b, -halfWidth,
            bR.x, bR.y, col.r, col.g, col.b,  halfWidth,
            aL.x, aL.y, col.r, col.g, col.b, -halfWidth,
            bR.x, bR.y, col.r, col.g, col.b,  halfWidth,
            aR.x, aR.y, col.r, col.g, col.b,  halfWidth,
        };
        std::memcpy(quadData[i], q, sizeof(q));
    }

    glUseProgram(aaLineProgram);
    glUniformMatrix4fv(aaLineMvpLoc, 1, GL_FALSE, glm::value_ptr(glm::mat4(1.0f)));  // positions already in clip space
    glBindVertexArray(aaLineVAO);
    glNamedBufferSubData(aaLineVBO, 0, sizeof(quadData), quadData);
    // One draw per axis: the hovered shaft uses a wider AA profile.
    for (int i = 0; i < 3; ++i) {
        glUniform1f(aaLineHalfWidthLoc, i == hoverAxis ? 1.0f : halfWidth);
        glDrawArrays(GL_TRIANGLES, i * 6, 6);
    }
    glBindVertexArray(0);

    // ---- Axis tip cones (solid color) ----
    glUseProgram(capProgram);
    glUniformMatrix4fv(capMvpLoc, 1, GL_FALSE, glm::value_ptr(lineMVP));
    glBindVertexArray(capVAO);
    for (int i = 0; i < 3; ++i) {
        const glm::vec3 col = axisColor(i);
        glUniform3fv(capColorLoc, 1, glm::value_ptr(col));
        glDrawArrays(GL_TRIANGLES, i * (capVertCount / 3), capVertCount / 3);
    }
    glBindVertexArray(0);

    // ---- Origin disc (solid white at pivot) ----
    glUseProgram(discProgram);
    glUniformMatrix4fv(discMvpLoc, 1, GL_FALSE, glm::value_ptr(lineMVP));
    glBindVertexArray(originVAO);
    glDrawArrays(GL_TRIANGLES, 0, originVertCount);
    glBindVertexArray(0);

    // ---- Pole-view axis markers (for axes viewed end-on) ----
    // Draw a small disc at origin for axes invisible due to pole view.
    // Batched: one upload + one draw regardless of how many axes are hidden.
    {
        const float poleMarkerRadius = 0.08f;
        constexpr int kSegs = 16;
        std::vector<float> markerVerts;
        markerVerts.reserve(3 * kSegs * 18);
        for (int i = 0; i < 3; ++i) {
            if (axisVisible[i]) continue;
            const glm::vec3 col = axisColor(i);
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
            glUniform1f(aaLineHalfWidthLoc, halfWidth);
            glBindVertexArray(aaLineVAO);
            glNamedBufferSubData(aaLineVBO, 0, static_cast<GLsizeiptr>(markerVerts.size() * sizeof(float)), markerVerts.data());
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(markerVerts.size() / 6));
            glBindVertexArray(0);
        }
    }

    // ---- Text labels (per-vertex color, batched: single upload + single draw) ----
    const glm::mat4 textMVP = glm::ortho(0.0f, (float)foot, 0.0f, (float)foot, -1.0f, 1.0f);
    const float cellU = 1.0f / 3.0f;
    const float glyph = 24.0f;
    const float half = glyph * 0.5f;

    float quads[3][6][7];  // 3 labels × 6 verts × (px.xy, uv, rgb)
    for (int i = 0; i < 3; ++i) {
        glm::vec4 clip = gizmoProj * gizmoView * glm::vec4(tips[i], 1.0f);
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        float f = 1.35f;
        ndc.x *= f; ndc.y *= f;
        float cx = (ndc.x * 0.5f + 0.5f) * foot;
        float cy = (ndc.y * 0.5f + 0.5f) * foot;
        cx = std::max(half, std::min(foot - half, cx));
        cy = std::max(half, std::min(foot - half, cy));

        // Detect pole view and offset label to avoid overlapping origin disc
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
        const glm::vec3 labCol = axisColor(i);
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

    // Same matrices as draw(): rotation-only view + fixed ortho.
    const glm::mat4 mvp = glm::ortho(-kExtent, kExtent, -kExtent, kExtent, -10.0f, 10.0f)
                        * glm::mat4(glm::mat3(mainView));

    // ~14 logical px of slack around each shaft/cone/label zone.
    const float thresh = 14.0f * (2.0f * kExtent) / static_cast<float>(foot);

    const glm::vec3 tips[3] = { {1,0,0}, {0,1,0}, {0,0,1} };
    int best = -1;
    float bestD = thresh;
    for (int i = 0; i < 3; ++i) {
        const glm::vec2 t(mvp * glm::vec4(tips[i], 1.0f));  // ortho: w == 1
        // Distance from click to segment [origin, tip]; allow overshoot past
        // the tip so the cone and billboarded label stay clickable.
        const float len2 = glm::dot(t, t);
        float u = (len2 > 1e-12f) ? glm::dot(click, t) / len2 : -1.0f;
        u = glm::clamp(u, 0.0f, 1.45f);
        const float d = glm::length(click - t * u);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

