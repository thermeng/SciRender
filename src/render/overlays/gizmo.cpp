#include "render/overlays/gizmo.h"

#include <glad/gl.h>
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

// Simple solid-color shader for light-marker discs.
static const char* lightMarkVS = R"(
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
static const char* lightMarkFS = R"(
#version 460 core
in vec3 vColor;
out vec4 frag;
void main() { frag = vec4(vColor, 1.0); }
)";

// Simple diffuse-lit shader for axis tip cones.
static const char* capVS = R"(
#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
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

namespace {
GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLchar buf[512];
        glGetShaderInfoLog(s, 512, nullptr, buf);
        std::cerr << "[Gizmo] shader compile error: " << buf << std::endl;
        glDeleteShader(s);
        return 0;
    }
    return s;
}
GLuint linkProgram(const char* vs, const char* fs) {
    GLuint v = compileShader(GL_VERTEX_SHADER, vs);
    GLuint f = compileShader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLchar buf[512];
        glGetProgramInfoLog(p, 512, nullptr, buf);
        std::cerr << "[Gizmo] program link error: " << buf << std::endl;
        glDeleteProgram(p);
        return 0;
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}
} // namespace

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
    aaLineProgram.reset(linkProgram(aaLineVS, aaLineFS));
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
    capProgram.reset(linkProgram(capVS, capFS));
    if (!capProgram.has()) return false;
    capMvpLoc   = glGetUniformLocation(capProgram, "uMVP");
    capColorLoc = glGetUniformLocation(capProgram, "uColor");
    return capMvpLoc >= 0 && capColorLoc >= 0;
}

bool Gizmo::buildLightMarkProgram() {
    lightMarkProgram.reset(linkProgram(lightMarkVS, lightMarkFS));
    if (!lightMarkProgram.has()) return false;
    lightMarkMvpLoc = glGetUniformLocation(lightMarkProgram, "uMVP");
    GLint posLoc = glGetAttribLocation(lightMarkProgram, "aPos");
    GLint colLoc = glGetAttribLocation(lightMarkProgram, "aColor");
    return lightMarkMvpLoc >= 0 && posLoc >= 0 && colLoc >= 0;
}

bool Gizmo::buildTextProgram() {
    textProgram.reset(linkProgram(textVS, textFS));
    if (!textProgram.has()) return false;
    textMvpLoc   = glGetUniformLocation(textProgram, "uMVP");
    textTexLoc   = glGetUniformLocation(textProgram, "uTex");
    textPosLoc   = glGetAttribLocation(textProgram, "aPos");
    textColLoc   = glGetAttribLocation(textProgram, "aColor");
    return textMvpLoc >= 0 && textTexLoc >= 0 && textPosLoc >= 0 && textColLoc >= 0;
}

bool Gizmo::init() {
    if (isInitialized()) return true;

    if (!buildAALineProgram() || !buildCapProgram() || !buildLightMarkProgram()
        || !buildTextProgram() || !buildAtlas())
        return false;

    // ---- AA line VBO (dynamic): 3 axes × 6 verts × (vec2 pos + vec3 col + float dist) ----
    {
        glCreateVertexArrays(1, aaLineVAO.ptr());
        glCreateBuffers(1, aaLineVBO.ptr());
        glEnableVertexArrayAttrib(aaLineVAO, aaLinePosLoc);
        glVertexArrayAttribFormat(aaLineVAO, aaLinePosLoc, 2, GL_FLOAT, GL_FALSE, 0);
        glVertexArrayAttribBinding(aaLineVAO, aaLinePosLoc, 0);
        glEnableVertexArrayAttrib(aaLineVAO, aaLineColLoc);
        glVertexArrayAttribFormat(aaLineVAO, aaLineColLoc, 3, GL_FLOAT, GL_FALSE, 2 * sizeof(float));
        glVertexArrayAttribBinding(aaLineVAO, aaLineColLoc, 0);
        glEnableVertexArrayAttrib(aaLineVAO, aaLineDistLoc);
        glVertexArrayAttribFormat(aaLineVAO, aaLineDistLoc, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(float));
        glVertexArrayAttribBinding(aaLineVAO, aaLineDistLoc, 0);
        glNamedBufferData(aaLineVBO, 3 * 6 * 6 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        glVertexArrayVertexBuffer(aaLineVAO, 0, aaLineVBO, 0, 6 * sizeof(float));
    }

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
            // Cap fan triangles (base face, normal points backward)
            glm::vec3 capN = -axes[a];
            glm::vec3 c0 = base + (perp1 * std::cos(0.0f) + perp2 * std::sin(0.0f)) * coneR;
            for (int i = 1; i < segments; ++i) {
                float a0 = (float)i / segments * 6.28318f;
                float a1 = (float)(i + 1) / segments * 6.28318f;
                if (i + 1 == segments) a1 = 0.0f;
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

        glCreateVertexArrays(1, capVAO.ptr());
        glCreateBuffers(1, capVBO.ptr());
        GLint capPosLoc = 0, capNormLoc = 1;
        glEnableVertexArrayAttrib(capVAO, capPosLoc);
        glVertexArrayAttribFormat(capVAO, capPosLoc, 3, GL_FLOAT, GL_FALSE, 0);
        glVertexArrayAttribBinding(capVAO, capPosLoc, 0);
        glEnableVertexArrayAttrib(capVAO, capNormLoc);
        glVertexArrayAttribFormat(capVAO, capNormLoc, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float));
        glVertexArrayAttribBinding(capVAO, capNormLoc, 0);
        glNamedBufferData(capVBO, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
        glVertexArrayVertexBuffer(capVAO, 0, capVBO, 0, 6 * sizeof(float));
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
        glCreateVertexArrays(1, originVAO.ptr());
        glCreateBuffers(1, originVBO.ptr());
        GLint lmPosLoc2 = glGetAttribLocation(lightMarkProgram, "aPos");
        GLint lmColLoc2 = glGetAttribLocation(lightMarkProgram, "aColor");
        glEnableVertexArrayAttrib(originVAO, lmPosLoc2);
        glVertexArrayAttribFormat(originVAO, lmPosLoc2, 3, GL_FLOAT, GL_FALSE, 0);
        glVertexArrayAttribBinding(originVAO, lmPosLoc2, 0);
        glEnableVertexArrayAttrib(originVAO, lmColLoc2);
        glVertexArrayAttribFormat(originVAO, lmColLoc2, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float));
        glVertexArrayAttribBinding(originVAO, lmColLoc2, 0);
        glNamedBufferData(originVBO, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
        glVertexArrayVertexBuffer(originVAO, 0, originVBO, 0, 6 * sizeof(float));
    }

    // ---- Text quad VBO (dynamic): 3 chars × 6 verts × (vec4 px.xy+uv + vec3 color) ----
    glCreateVertexArrays(1, textVAO.ptr());
    glCreateBuffers(1, textVBO.ptr());
    glEnableVertexArrayAttrib(textVAO, textPosLoc);
    glVertexArrayAttribFormat(textVAO, textPosLoc, 4, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(textVAO, textPosLoc, 0);
    glEnableVertexArrayAttrib(textVAO, textColLoc);
    glVertexArrayAttribFormat(textVAO, textColLoc, 3, GL_FLOAT, GL_FALSE, 4 * sizeof(float));
    glVertexArrayAttribBinding(textVAO, textColLoc, 0);
    glNamedBufferData(textVBO, 3 * 6 * 7 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glVertexArrayVertexBuffer(textVAO, 0, textVBO, 0, 7 * sizeof(float));

    // ---- Light-marker disc VBO (dynamic): 5 lights × 6 verts × vec6(px.xy + rgb) ----
    {
        glCreateVertexArrays(1, lightMarkVAO.ptr());
        glCreateBuffers(1, lightMarkVBO.ptr());
        GLint lmPosLoc = glGetAttribLocation(lightMarkProgram, "aPos");
        GLint lmColLoc = glGetAttribLocation(lightMarkProgram, "aColor");
        glEnableVertexArrayAttrib(lightMarkVAO, lmPosLoc);
        glVertexArrayAttribFormat(lightMarkVAO, lmPosLoc, 3, GL_FLOAT, GL_FALSE, 0);
        glVertexArrayAttribBinding(lightMarkVAO, lmPosLoc, 0);
        glEnableVertexArrayAttrib(lightMarkVAO, lmColLoc);
        glVertexArrayAttribFormat(lightMarkVAO, lmColLoc, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float));
        glVertexArrayAttribBinding(lightMarkVAO, lmColLoc, 0);
        glNamedBufferData(lightMarkVBO, 5 * 6 * 6 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        glVertexArrayVertexBuffer(lightMarkVAO, 0, lightMarkVBO, 0, 6 * sizeof(float));
    }

    return true;
}

void Gizmo::shutdown() {
    if (!QOpenGLContext::currentContext()) return;
    aaLineVAO.reset(); aaLineVBO.reset(); aaLineProgram.reset();
    capVAO.reset(); capVBO.reset(); capProgram.reset();
    originVAO.reset(); originVBO.reset();
    lightMarkVAO.reset(); lightMarkVBO.reset(); lightMarkProgram.reset();
    textVAO.reset(); textVBO.reset(); textProgram.reset();
    glyphTex.reset();
}

void Gizmo::draw(const glm::mat4& mainView, float dpr, int foot) {
    if (!isInitialized()) return;

    // Preserve engine state we mutate.
    GLint prevVP[4];
    glGetIntegerv(GL_VIEWPORT, prevVP);
    GLboolean depthWas = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blendWas = glIsEnabled(GL_BLEND);
    GLboolean cullWas = glIsEnabled(GL_CULL_FACE);
    GLint polyMode[2];
    glGetIntegerv(GL_POLYGON_MODE, polyMode);
    GLenum prevClipOrigin;
    GLenum prevClipDepthMode;
    glGetIntegerv(GL_CLIP_ORIGIN, (GLint*)&prevClipOrigin);
    glGetIntegerv(GL_CLIP_DEPTH_MODE, (GLint*)&prevClipDepthMode);

    const int margin = 10;
    const int s = static_cast<int>(foot * dpr);
    const int m = static_cast<int>(margin * dpr);
    glViewport(m, m, s, s);

    const glm::mat4 gizmoView = glm::mat4(glm::mat3(mainView));
    const glm::mat4 gizmoProj = glm::ortho(-1.55f, 1.55f, -1.55f, 1.55f, -10.0f, 10.0f);
    const glm::mat4 lineMVP = gizmoProj * gizmoView;

    glClipControl(prevClipOrigin, GL_NEGATIVE_ONE_TO_ONE);
    glDisable(GL_DEPTH_TEST);
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

    // Per-axis quad: 6 verts (2 triangles) × (vec2 pos + vec3 col + float dist) = 36 floats
    float quadData[3][36];

    for (int i = 0; i < 3; ++i) {
        glm::vec4 cA = lineMVP * glm::vec4(origins[i], 1.0f);
        glm::vec4 cB = lineMVP * glm::vec4(tips[i], 1.0f);
        // Ortho: w=1, just use xy
        glm::vec2 a(cA), b(cB);
        glm::vec2 dir = b - a;
        float len = glm::length(dir);
        if (len < 1e-6f) continue;
        glm::vec2 perp(-dir.y / len, dir.x / len);  // perpendicular in clip space

        glm::vec2 aL = a - perp * hwClip;
        glm::vec2 aR = a + perp * hwClip;
        glm::vec2 bL = b - perp * hwClip;
        glm::vec2 bR = b + perp * hwClip;

        float r = colors[i][0], g = colors[i][1], b2 = colors[i][2];
        // Two triangles: (aL,bL,bR) and (aL,bR,aR)
        // vDist: -halfWidth at left edge, +halfWidth at right edge
        float q[] = {
            aL.x, aL.y, r, g, b2, -halfWidth,
            bL.x, bL.y, r, g, b2, -halfWidth,
            bR.x, bR.y, r, g, b2,  halfWidth,
            aL.x, aL.y, r, g, b2, -halfWidth,
            bR.x, bR.y, r, g, b2,  halfWidth,
            aR.x, aR.y, r, g, b2,  halfWidth,
        };
        std::memcpy(quadData[i], q, sizeof(q));
    }

    glUseProgram(aaLineProgram);
    glUniformMatrix4fv(aaLineMvpLoc, 1, GL_FALSE, glm::value_ptr(glm::mat4(1.0f)));  // positions already in clip space
    glUniform1f(aaLineHalfWidthLoc, halfWidth);
    glBindVertexArray(aaLineVAO);
    glNamedBufferSubData(aaLineVBO, 0, sizeof(quadData), quadData);
    glDrawArrays(GL_TRIANGLES, 0, 3 * 6);  // 3 axis quads
    glBindVertexArray(0);

    // ---- Axis tip cones (diffuse-lit) ----
    glUseProgram(capProgram);
    glUniformMatrix4fv(capMvpLoc, 1, GL_FALSE, glm::value_ptr(lineMVP));
    // Simple directional light from upper-right-front
    glm::vec3 lightDir = glm::normalize(glm::vec3(0.4f, 0.6f, 0.7f));
    glUniform3fv(capLightDirLoc, 1, glm::value_ptr(lightDir));
    glBindVertexArray(capVAO);
    for (int i = 0; i < 3; ++i) {
        glUniform3fv(capColorLoc, 1, colors[i]);
        glDrawArrays(GL_TRIANGLES, i * (capVertCount / 3), capVertCount / 3);
    }
    glBindVertexArray(0);

    // ---- Origin disc (solid white at pivot) ----
    glUseProgram(lightMarkProgram);
    glUniformMatrix4fv(lightMarkMvpLoc, 1, GL_FALSE, glm::value_ptr(lineMVP));
    glBindVertexArray(originVAO);
    glDrawArrays(GL_TRIANGLES, 0, originVertCount);
    glBindVertexArray(0);

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

        float u0 = i * cellU, u1 = (i + 1) * cellU;
        const float vTop = 0.0f, vBot = 1.0f;
        const float hx = glyph * 0.5f, hy = glyph * 0.5f;
        const float r = colors[i][0], g = colors[i][1], b = colors[i][2];
        float tri[6][7] = {
            { cx - hx, cy + hy, u0, vTop, r, g, b },
            { cx + hx, cy + hy, u1, vTop, r, g, b },
            { cx - hx, cy - hy, u0, vBot, r, g, b },
            { cx + hx, cy + hy, u1, vTop, r, g, b },
            { cx + hx, cy - hy, u1, vBot, r, g, b },
            { cx - hx, cy - hy, u0, vBot, r, g, b },
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

    // ---- State handover ----
    glClipControl(prevClipOrigin, GL_NEGATIVE_ONE_TO_ONE);
    if (depthWas) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (blendWas) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (cullWas) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT, static_cast<GLenum>(polyMode[0]));
    glPolygonMode(GL_BACK, static_cast<GLenum>(polyMode[1]));
    glViewport(prevVP[0], prevVP[1], prevVP[2], prevVP[3]);
}

void Gizmo::drawLights(const glm::vec3 dirs[5], const glm::vec3 cols[5], float dpr, int foot) {
    if (!isInitialized()) return;

    // Preserve engine state we mutate (viewport + depth test + blend + poly mode + bindings + clip control).
    GLint prevVP[4];
    glGetIntegerv(GL_VIEWPORT, prevVP);
    GLboolean depthWas = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blendWas = glIsEnabled(GL_BLEND);
    GLboolean cullWas = glIsEnabled(GL_CULL_FACE);
    GLint polyMode[2];
    glGetIntegerv(GL_POLYGON_MODE, polyMode);
    GLenum prevClipOrigin;
    GLenum prevClipDepthMode;
    glGetIntegerv(GL_CLIP_ORIGIN, (GLint*)&prevClipOrigin);
    glGetIntegerv(GL_CLIP_DEPTH_MODE, (GLint*)&prevClipDepthMode);

    const int m = static_cast<int>(10 * dpr);
    const int s = static_cast<int>(foot * dpr);
    const int y0 = m;
    glViewport(m, y0, s, s);

    glClipControl(prevClipOrigin, GL_NEGATIVE_ONE_TO_ONE);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); // force filled discs (see draw())

    // px space [0..foot]x[0..foot] -> clip; markers are constant kit-local dirs.
    const glm::mat4 pxMVP = glm::ortho(0.0f, (float)foot, 0.0f, (float)foot, -1.0f, 1.0f);
    const float r = 6.0f; // disc radius in px

    glUseProgram(lightMarkProgram);
    glUniformMatrix4fv(lightMarkMvpLoc, 1, GL_FALSE, glm::value_ptr(pxMVP));
    glBindVertexArray(lightMarkVAO);
    // Build all 5 marker discs with per-vertex color, then single upload + single draw.
    float verts[5][6][6];
    for (int i = 0; i < 5; ++i) {
        float cx = (dirs[i].x * 0.5f + 0.5f) * foot;
        float cy = (dirs[i].y * 0.5f + 0.5f) * foot;
        const float q[6][6] = {
            { cx - r, cy - r, 0.0f, cols[i].r, cols[i].g, cols[i].b },
            { cx + r, cy - r, 0.0f, cols[i].r, cols[i].g, cols[i].b },
            { cx - r, cy + r, 0.0f, cols[i].r, cols[i].g, cols[i].b },
            { cx + r, cy - r, 0.0f, cols[i].r, cols[i].g, cols[i].b },
            { cx + r, cy + r, 0.0f, cols[i].r, cols[i].g, cols[i].b },
            { cx - r, cy + r, 0.0f, cols[i].r, cols[i].g, cols[i].b },
        };
        std::memcpy(verts[i], q, sizeof(q));
    }
    glNamedBufferSubData(lightMarkVBO, 0, sizeof(verts), verts);
    glDrawArrays(GL_TRIANGLES, 0, 5 * 6);  // 5 marker discs
    glBindVertexArray(0);
    glUseProgram(0);

    // ---- State handover: restore everything we touched ----
    glClipControl(prevClipOrigin, prevClipDepthMode);
    if (depthWas) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (blendWas) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (cullWas) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT, static_cast<GLenum>(polyMode[0]));
    glPolygonMode(GL_BACK, static_cast<GLenum>(polyMode[1]));
    glViewport(prevVP[0], prevVP[1], prevVP[2], prevVP[3]);
}

