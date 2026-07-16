#include "render/colorbar_overlay.h"

#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <QOpenGLContext>
#include <cstring>

namespace {

const char* vsSrc = R"(
#version 330 core
layout(location = 0) in vec2 aPos;   // clip-space xy
layout(location = 1) in vec2 aUV;   // texture uv
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

const char* fsSrc = R"(
#version 330 core
in vec2 vUV;
uniform sampler2D uTex;
out vec4 frag;
void main() {
    vec4 c = texture(uTex, vUV);
    if (c.a < 0.02) discard;
    frag = c;
}
)";

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(s); return 0; }
    return s;
}

GLuint link(const char* vs, const char* fs) {
    GLuint v = compile(GL_VERTEX_SHADER, vs);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(p); return 0; }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

} // namespace

bool ColorbarOverlay::init() {
    if (isInitialized()) return true;
    if (!buildProgram()) return false;

    // Fullscreen quad (two triangles) in clip space, with UVs.
    // Quad covers the whole viewport; the QImage already has the colorbar
    // drawn at the correct screen location, so we just blit it.
    const float verts[6][4] = {
        // x,   y,    u, v
        {-1, -1, 0, 0},
        { 1, -1, 1, 0},
        {-1,  1, 0, 1},
        {-1,  1, 0, 1},
        { 1, -1, 1, 0},
        { 1,  1, 1, 1},
    };
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    glGenTextures(1, &tex_);
    return true;
}

bool ColorbarOverlay::buildProgram() {
    program_ = link(vsSrc, fsSrc);
    if (!program_) return false;
    mvpLoc_ = glGetUniformLocation(program_, "uTex"); // only sampler used
    texLoc_ = glGetUniformLocation(program_, "uTex");
    return true;
}

void ColorbarOverlay::shutdown() {
    if (!QOpenGLContext::currentContext()) return;
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (tex_) glDeleteTextures(1, &tex_);
    if (program_) glDeleteProgram(program_);
    vao_ = vbo_ = tex_ = program_ = 0;
}

QImage ColorbarOverlay::buildImage(float dpr, int deviceW, int deviceH,
                                     const ColorbarData& data, int corner) const {
    QImage img(deviceW, deviceH, QImage::Format_ARGB32);
    img.fill(Qt::transparent);

    if (!data.visible || data.stops.isEmpty() || data.title.isEmpty()) {
        return img;
    }

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    // ---- Layout (all scaled by device pixel ratio for crisp text) ----
    const int margin    = static_cast<int>(14 * dpr);
    const int barW      = static_cast<int>(18 * dpr);
    const int barH      = static_cast<int>(220 * dpr);
    const int spacing   = static_cast<int>(8 * dpr);
    const int tickLen   = static_cast<int>(6 * dpr);

    // Title font/metrics -> dynamic title height and width.
    QFont titleFont;
    titleFont.setPointSizeF(10 * dpr);
    const QFontMetrics titleFm(titleFont);
    const int titleH   = titleFm.height();
    const int titleW   = titleFm.horizontalAdvance(data.title);
    const int titleGap = static_cast<int>(8 * dpr); // gap between title and bar

    // Tick label font/metrics -> dynamic width from the longest label.
    QFont tickFont;
    tickFont.setPointSizeF(9 * dpr);
    const QFontMetrics tickFm(tickFont);
    int maxLabelW = 0;
    for (const QVariant& lbl : data.tickLabels) {
        maxLabelW = qMax(maxLabelW, tickFm.horizontalAdvance(lbl.toString()));
    }
    if (maxLabelW == 0) maxLabelW = tickFm.horizontalAdvance("0.0");

    // Content widths: bar + gap + label block. blockW takes the max of the
    // label block and the title so a long title never overflows the right edge.
    const int labelW = maxLabelW;
    const int labelBlockW = barW + spacing + labelW;
    const int blockW = qMax(labelBlockW, titleW);
    const int blockH = titleH + titleGap + barH;

    int x, y;
    if (corner == 0) {
        // bottom-right
        x = deviceW - margin - blockW;
        y = deviceH - margin - blockH;
    } else {
        // top-right
        x = deviceW - margin - blockW;
        y = margin;
    }

    // ---- Title (left-aligned, above the bar, with a gap) ----
    p.setFont(titleFont);
    p.setPen(QColor("#e8e8e8"));
    const QRect titleRect(x, y, blockW, titleH);
    p.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, data.title);

    const int barX = x;
    const int barY = y + titleH + titleGap;

    // ---- Gradient bar: smooth vertical fill from the stops ----
    // Stops are [t, r, g, b] with t ascending 0..1, and the legend top is t=1
    // (max). So a gradient stop at position p (0=top) maps to color(t = 1 - p).
    {
        QLinearGradient grad(0.0, barY, 0.0, barY + barH);
        const int n = data.stops.size();
        for (int i = 0; i < n; ++i) {
            const QVariantList s = data.stops[i].toList();
            const float t = s[0].toFloat();
            const qreal p = qreal(1.0) - qreal(t); // top -> max
            grad.setColorAt(p, QColor::fromRgbF(
                qBound(0.0, s[1].toDouble(), 1.0),
                qBound(0.0, s[2].toDouble(), 1.0),
                qBound(0.0, s[3].toDouble(), 1.0)));
        }
        p.fillRect(barX, barY, barW, barH, grad);
    }

    // ---- Bar outline (crisp 1px edge) ----
    p.setPen(QColor(0, 0, 0, 180));
    p.setBrush(Qt::NoBrush);
    p.drawRect(barX, barY, barW, barH);

    // ---- Tick marks + numeric labels ----
    p.setFont(tickFont);
    p.setPen(QColor("#e8e8e8"));

    const int tickCount = data.tickLabels.size();
    const int labX = barX + barW + spacing;
    for (int i = 0; i < tickCount; ++i) {
        // i = 0 -> top of bar (max); frac = i/(count-1) measures top->bottom.
        const float frac = tickCount > 1 ? static_cast<float>(i) / static_cast<float>(tickCount - 1) : 0.0f;
        const int ty = barY + static_cast<int>(frac * barH);

        // tick mark on the right edge of the bar, pointing outward
        p.drawLine(barX + barW, ty, barX + barW + tickLen, ty);

        // numeric label, left-aligned against the bar edge, vertically centered
        const QRect labRect(labX, ty - static_cast<int>(tickFm.height() * 0.5),
                            labelW, tickFm.height());
        p.drawText(labRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, data.tickLabels[i]);
    }

    return img;
}

void ColorbarOverlay::uploadAndDraw(const QImage& img, int deviceW, int deviceH) {
    QImage gl = img.convertToFormat(QImage::Format_RGBA8888);
    // QImage rows are top-to-bottom, but GL texture coordinate v=0 is the FIRST
    // uploaded row and is sampled at clip-space BOTTOM. Without flipping, the
    // fullscreen blit renders the legend upside-down in the FBO; captureFBO's
    // row-flip then bakes that inversion into the saved image. Mirror vertically
    // so the on-screen/window-space layout (top = max) is preserved in the PNG.
    gl = gl.flipped(Qt::Vertical);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, gl.width(), gl.height(), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, gl.constBits());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    // The colorbar is a full-viewport blit: always draw into the full FBO
    // viewport, since a prior pass (e.g. gizmo light markers) may have left a
    // smaller viewport bound. Without this the legend would be squashed.
    glViewport(0, 0, deviceW, deviceH);

    glUseProgram(program_);
    glUniform1i(texLoc_, 0);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

void ColorbarOverlay::draw(float dpr, int deviceW, int deviceH,
                           const ColorbarData& data, int corner) {
    if (!isInitialized() || deviceW <= 0 || deviceH <= 0) return;
    if (!data.visible) return;

    QImage img = buildImage(dpr, deviceW, deviceH, data, corner);
    uploadAndDraw(img, deviceW, deviceH);
}
