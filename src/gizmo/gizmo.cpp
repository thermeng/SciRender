#include "gizmo/gizmo.h"

#include <vector>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <QImage>
#include <QPainter>
#include <QFont>
#include <QString>

// ----------------------------------------------------------------------------
// Shaders
// ----------------------------------------------------------------------------
static const char* lineVS = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
uniform mat4 uMVP;
out vec3 vColor;
void main() {
    vColor = aColor;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";
static const char* lineFS = R"(
#version 330 core
in vec3 vColor;
out vec4 frag;
void main() { frag = vec4(vColor, 1.0); }
)";

static const char* textVS = R"(
#version 330 core
layout(location = 0) in vec4 aPos;   // xy = local px (in 0..viewport px space), zw = uv
uniform mat4 uMVP;
out vec2 vUV;
void main() {
    vUV = aPos.zw;
    gl_Position = uMVP * vec4(aPos.xy, 0.0, 1.0);
}
)";
static const char* textFS = R"(
#version 330 core
in vec2 vUV;
uniform sampler2D uTex;
uniform vec3 uColor;
out vec4 frag;
void main() {
    float a = texture(uTex, vUV).r;   // atlas is white-on-transparent; .r = coverage
    if (a < 0.02) discard;
    frag = vec4(uColor, a);
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
// Uploaded mirrored (GL origin is bottom-left) so UV (0,0) maps to glyph bottom-left.
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
        f.setPointSize(44);
        f.setBold(true);
        p.setFont(f);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);
        for (int i = 0; i < 3; ++i) {
            p.drawText(QRect(i * cell, 0, cell, cell), Qt::AlignCenter, QString(chars[i]));
        }
    }
    QImage gl = img.convertToFormat(QImage::Format_RGBA8888);
    // NOTE: no vertical mirror here — the overlay's coordinate space already matches
    // GL's bottom-left origin, so flipping would invert the glyphs (classic Y-inversion bug).
    for (int y = 0; y < gl.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(gl.scanLine(y));
        for (int x = 0; x < gl.width(); ++x) {
            if (qAlpha(row[x]) > 0) row[x] = qRgba(255, 255, 255, qAlpha(row[x]));
        }
    }

    glGenTextures(1, &glyphTex);
    glBindTexture(GL_TEXTURE_2D, glyphTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, glyphAtlasW, glyphAtlasH, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, gl.constBits());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return glyphTex != 0;
}

bool Gizmo::buildLineProgram() {
    lineProgram = linkProgram(lineVS, lineFS);
    if (!lineProgram) return false;
    lineMvpLoc   = glGetUniformLocation(lineProgram, "uMVP");
    linePosLoc   = glGetAttribLocation(lineProgram, "aPos");
    lineColLoc   = glGetAttribLocation(lineProgram, "aColor");
    return lineMvpLoc >= 0 && linePosLoc >= 0 && lineColLoc >= 0;
}

bool Gizmo::buildTextProgram() {
    textProgram = linkProgram(textVS, textFS);
    if (!textProgram) return false;
    textMvpLoc   = glGetUniformLocation(textProgram, "uMVP");
    textColorLoc = glGetUniformLocation(textProgram, "uColor");
    textTexLoc   = glGetUniformLocation(textProgram, "uTex");
    textPosLoc   = glGetAttribLocation(textProgram, "aPos");
    return textMvpLoc >= 0 && textColorLoc >= 0 && textTexLoc >= 0 && textPosLoc >= 0;
}

bool Gizmo::init() {
    if (isInitialized()) return true;

    if (!buildLineProgram() || !buildTextProgram() || !buildAtlas())
        return false;

    // Axis line geometry: origin -> tip, per-vertex color (R/G/B).
    const float lines[] = {
        //  pos                color
        0.0f, 0.0f, 0.0f,  1.0f, 0.2f, 0.2f,   // X base
        1.0f, 0.0f, 0.0f,  1.0f, 0.2f, 0.2f,   // X tip (red)
        0.0f, 0.0f, 0.0f,  0.2f, 1.0f, 0.2f,   // Y base
        0.0f, 1.0f, 0.0f,  0.2f, 1.0f, 0.2f,   // Y tip (green)
        0.0f, 0.0f, 0.0f,  0.3f, 0.5f, 1.0f,   // Z base
        0.0f, 0.0f, 1.0f,  0.3f, 0.5f, 1.0f,   // Z tip (blue)
    };
    glGenVertexArrays(1, &lineVAO);
    glGenBuffers(1, &lineVBO);
    glBindVertexArray(lineVAO);
    glBindBuffer(GL_ARRAY_BUFFER, lineVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(lines), lines, GL_STATIC_DRAW);
    glVertexAttribPointer(linePosLoc, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(linePosLoc);
    glVertexAttribPointer(lineColLoc, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(lineColLoc);
    glBindVertexArray(0);

    // Text quad VBO: 3 chars * 6 verts * vec4(px.xy, uv.zw). Allocated dynamic.
    glGenVertexArrays(1, &textVAO);
    glGenBuffers(1, &textVBO);
    glBindVertexArray(textVAO);
    glBindBuffer(GL_ARRAY_BUFFER, textVBO);
    glBufferData(GL_ARRAY_BUFFER, 3 * 6 * 4 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(textPosLoc, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(textPosLoc);
    glBindVertexArray(0);

    return true;
}

void Gizmo::shutdown() {
    if (lineVAO)   glDeleteVertexArrays(1, &lineVAO);
    if (lineVBO)   glDeleteBuffers(1, &lineVBO);
    if (textVAO)   glDeleteVertexArrays(1, &textVAO);
    if (textVBO)   glDeleteBuffers(1, &textVBO);
    if (glyphTex)  glDeleteTextures(1, &glyphTex);
    if (lineProgram)  glDeleteProgram(lineProgram);
    if (textProgram)  glDeleteProgram(textProgram);
    lineVAO = lineVBO = textVAO = textVBO = glyphTex = 0;
    lineProgram = textProgram = 0;
}

void Gizmo::draw(const glm::mat4& mainView, float dpr, int fbHeight) {
    if (!isInitialized()) return;

    // Preserve engine state we mutate (viewport + depth test + blend + bindings).
    GLint prevVP[4];
    glGetIntegerv(GL_VIEWPORT, prevVP);
    GLboolean depthWas = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blendWas = glIsEnabled(GL_BLEND);

    // 2. Isolated corner viewport footprint (scaled by dpr so it stays constant on HiDPI).
    const int foot = 120;
    const int margin = 10;
    const int s = static_cast<int>(foot * dpr);
    const int m = static_cast<int>(margin * dpr);
    // ponytail: context origin is top-left, so bottom edge = fbHeight - margin - size
    const int y0 = fbHeight - m - s;
    glViewport(m, y0, s, s);

    // ponytail: NO clear — scene already painted this corner with bgColor before
    // drawGizmo() runs, so blending on top keeps the gizmo transparent (model stays
    // visible behind it) instead of an opaque plate covering the mesh.

    // Rotation-only view matrix (strips camera translation).
    const glm::mat4 gizmoView = glm::mat4(glm::mat3(mainView));
    // Tight ortho but with margin so the triad + labels never reach the viewport edge.
    const glm::mat4 gizmoProj = glm::ortho(-1.55f, 1.55f, -1.55f, 1.55f, -10.0f, 10.0f);
    const glm::mat4 lineMVP = gizmoProj * gizmoView;

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // ---- Axis lines ----
    glUseProgram(lineProgram);
    glUniformMatrix4fv(lineMvpLoc, 1, GL_FALSE, glm::value_ptr(lineMVP));
    glBindVertexArray(lineVAO);
    glDrawArrays(GL_LINES, 0, 6);
    glBindVertexArray(0);

    // ---- Text placement: project each tip into the corner viewport's local px space ----
    // Text ortho maps local px [0..foot]x[0..foot] to clip; glyph coords live in that space.
    const glm::mat4 textMVP = glm::ortho(0.0f, (float)foot, 0.0f, (float)foot, -1.0f, 1.0f);
    const float colors[3][3] = { {1.0f,0.2f,0.2f}, {0.2f,1.0f,0.2f}, {0.3f,0.5f,1.0f} };
    const glm::vec3 tips[3] = { {1,0,0}, {0,1,0}, {0,0,1} };
    const float cellU = 1.0f / 3.0f;

    // Build dynamic quad geometry for all three glyphs, then draw per-axis for color override.
    float quads[3][6][4]; // [char][vert][px.x,px.y,uv.u,uv.v]
    const float glyph = 24.0f;
    for (int i = 0; i < 3; ++i) {
        glm::vec4 clip = gizmoProj * gizmoView * glm::vec4(tips[i], 1.0f);
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        // push the label slightly beyond the tip along its screen direction
        float f = 1.16f;
        ndc.x *= f; ndc.y *= f;
        float cx = (ndc.x * 0.5f + 0.5f) * foot;
        float cy = (ndc.y * 0.5f + 0.5f) * foot;
        // clamp so the entire glyph quad stays inside the viewport footprint (no clipping)
        const float half = glyph * 0.5f;
        cx = std::max(half, std::min(foot - half, cx));
        cy = std::max(half, std::min(foot - half, cy));

        float u0 = i * cellU, u1 = (i + 1) * cellU;
        // Atlas uploaded unflipped (QPainter top-row = glyph top = texture v=0).
        // Flip V here so the quad's bottom edge samples v=1 (glyph bottom).
        float v0 = 1.0f,     v1 = 0.0f;
        float hx = glyph * 0.5f, hy = glyph * 0.5f;
        float tri[6][4] = {
            { cx - hx, cy + hy, u0, v1 }, // TL
            { cx + hx, cy + hy, u1, v1 }, // TR
            { cx - hx, cy - hy, u0, v0 }, // BL
            { cx + hx, cy + hy, u1, v1 }, // TR
            { cx + hx, cy - hy, u1, v0 }, // BR
            { cx - hx, cy - hy, u0, v0 }, // BL
        };
        std::memcpy(quads[i], tri, sizeof(tri));
    }

    glUseProgram(textProgram);
    glUniformMatrix4fv(textMvpLoc, 1, GL_FALSE, glm::value_ptr(textMVP));
    glUniform1i(textTexLoc, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, glyphTex);
    glBindVertexArray(textVAO);
    glBindBuffer(GL_ARRAY_BUFFER, textVBO);
    for (int i = 0; i < 3; ++i) {
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quads[i]), quads[i]);
        glUniform3fv(textColorLoc, 1, colors[i]);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    // ---- State handover: restore everything we touched ----
    if (depthWas) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (blendWas) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glViewport(prevVP[0], prevVP[1], prevVP[2], prevVP[3]);
}
