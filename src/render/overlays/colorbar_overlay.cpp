#include "render/overlays/colorbar_overlay.h"

#include <glad/gl.h>
#include "render/foundation/shader_utils.h"
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <QOpenGLContext>
#include <cstring>

namespace {

const char* vsSrc = R"(
#version 460 core
layout(location = 0) in vec2 aPos;   // clip-space xy
layout(location = 1) in vec2 aUV;   // texture uv
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

const char* fsSrc = R"(
#version 460 core
in vec2 vUV;
uniform sampler2D uTex;
out vec4 frag;
void main() {
    vec4 c = texture(uTex, vUV);
    if (c.a < 0.02) discard;
    frag = c;
}
)";

} // namespace

bool ColorbarOverlay::init() {
    if (isInitialized()) return true;
    if (!buildProgram()) return false;

    const float verts[6][4] = {
        {-1, -1, 0, 0},
        { 1, -1, 1, 0},
        {-1,  1, 0, 1},
        {-1,  1, 0, 1},
        { 1, -1, 1, 0},
        { 1,  1, 1, 1},
    };
    setupVertexBuffer(vao_, vbo_, verts, sizeof(verts), 4 * sizeof(float),
                      { { 0, 2, 0 }, { 1, 2, 2 * sizeof(float) } }, GL_STATIC_DRAW);

    glCreateTextures(GL_TEXTURE_2D, 1, tex_.ptr());
    glTextureParameteri(tex_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(tex_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(tex_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(tex_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

bool ColorbarOverlay::buildProgram() {
    program_.reset(compileProgram(vsSrc, fsSrc, "ColorbarOverlay"));
    if (!program_.has()) return false;
    samplerLoc_ = glGetUniformLocation(program_, "uTex");
    return true;
}

void ColorbarOverlay::shutdown() {
    if (!QOpenGLContext::currentContext()) return;
    vao_.reset();
    vbo_.reset();
    tex_.reset();
    program_.reset();
    samplerLoc_ = -1;
    texW_ = texH_ = 0;
    imageCacheValid_ = false;
    textureCacheValid_ = false;
    cachedImage_ = QImage();
}

QImage ColorbarOverlay::buildImage(float dpr, int deviceW, int deviceH,
                                    const std::vector<ColorbarData>& bars) const {
    QImage img(deviceW, deviceH, QImage::Format_ARGB32);
    img.fill(Qt::transparent);

    if (bars.empty()) return img;

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const int margin = static_cast<int>(14 * dpr);
    const int barW   = static_cast<int>(220 * dpr);
    const int barH   = static_cast<int>(14 * dpr);
    const int gap    = static_cast<int>(4 * dpr);
    const int tickLen = static_cast<int>(6 * dpr);
    const int stackGap = static_cast<int>(8 * dpr);

    QFont labelFont;
    labelFont.setPixelSize(static_cast<int>(11 * dpr));
    const QFontMetrics labelFm(labelFont);
    const int labelH = labelFm.height();

    QFont subtitleFont;
    subtitleFont.setPixelSize(static_cast<int>(9 * dpr));
    const QFontMetrics subtitleFm(subtitleFont);
    const int subtitleH = subtitleFm.height();

    QFont tickFont;
    tickFont.setPixelSize(static_cast<int>(9 * dpr));
    const QFontMetrics tickFm(tickFont);
    const int tickH = tickFm.height();

    const int subtitleGap = 2;
    const int blockH = subtitleH + subtitleGap + labelH + gap + barH + gap + tickH;

    // Stack bars bottom-right, first bar at the bottom.
    int y = deviceH - margin;

    for (int bi = 0; bi < bars.size(); ++bi) {
        const auto& data = bars[bi];
        if (!data.visible || data.stops.isEmpty() || data.title.isEmpty()) continue;

        y -= blockH;
        const int blockX = deviceW - margin - barW;
        const int blockY = y;

        // ---- Subtitle (type annotation, centered above title) ----
        int titleY = blockY;
        if (!data.subtitle.isEmpty()) {
            p.setFont(subtitleFont);
            p.setPen(QColor("#999999"));
            const QRect subtitleRect(blockX, titleY, barW, subtitleH);
            p.drawText(subtitleRect, Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextSingleLine, data.subtitle);
            titleY += subtitleH + subtitleGap;
        }

        // ---- Title (centered above bar) ----
        p.setFont(labelFont);
        p.setPen(QColor("#e8e8e8"));
        const QRect titleRect(blockX, titleY, barW, labelH);
        p.drawText(titleRect, Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextSingleLine, data.title);

        const int barX = blockX;
        const int barY = titleY + labelH + gap;

        // ---- Horizontal gradient bar ----
        {
            QLinearGradient grad(static_cast<qreal>(barX), 0.0,
                                 static_cast<qreal>(barX + barW), 0.0);
            const int n = data.stops.size();
            for (int i = 0; i < n; ++i) {
                const QVariantList s = data.stops[i].toList();
                const float t = s[0].toFloat();
                grad.setColorAt(static_cast<qreal>(t), QColor::fromRgbF(
                    qBound(0.0, s[1].toDouble(), 1.0),
                    qBound(0.0, s[2].toDouble(), 1.0),
                    qBound(0.0, s[3].toDouble(), 1.0)));
            }
            p.fillRect(barX, barY, barW, barH, grad);
        }

        // ---- Bar outline ----
        p.setPen(QColor(0, 0, 0, 180));
        p.setBrush(Qt::NoBrush);
        p.drawRect(barX, barY, barW, barH);

        // ---- Tick marks + labels (below bar, evenly spaced left-to-right) ----
        p.setFont(tickFont);
        p.setPen(QColor("#e8e8e8"));

        const int tickCount = data.tickLabels.size();
        if (tickCount > 0) {
            const int tickY = barY + barH;
            for (int i = 0; i < tickCount; ++i) {
                const float frac = tickCount > 1
                    ? static_cast<float>(i) / static_cast<float>(tickCount - 1)
                    : 0.0f;
                const int tx = barX + static_cast<int>(frac * barW);

                // tick mark downward from bar bottom edge
                p.drawLine(tx, tickY, tx, tickY + tickLen);

                // label centered under tick
                const QRect lblRect(tx - static_cast<int>(tickFm.horizontalAdvance(data.tickLabels[i]) * 0.5),
                                    tickY + tickLen,
                                    tickFm.horizontalAdvance(data.tickLabels[i]),
                                    tickH);
                p.drawText(lblRect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextSingleLine,
                           data.tickLabels[i]);
            }
        }

        y -= stackGap;
    }

    return img;
}

void ColorbarOverlay::uploadAndDraw(const QImage& img, int deviceW, int deviceH) {
    QImage gl = img.convertToFormat(QImage::Format_RGBA8888);
    gl = gl.flipped(Qt::Vertical);

    if (gl.width() != texW_ || gl.height() != texH_) {
        tex_.reset();
        glCreateTextures(GL_TEXTURE_2D, 1, tex_.ptr());
        glTextureParameteri(tex_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(tex_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(tex_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(tex_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        texW_ = gl.width();
        texH_ = gl.height();
    }
    glBindTextureUnit(0, tex_);
    glTextureStorage2D(tex_, 1, GL_RGBA8, gl.width(), gl.height());
    glTextureSubImage2D(tex_, 0, 0, 0, gl.width(), gl.height(), GL_RGBA, GL_UNSIGNED_BYTE, gl.constBits());
    textureCacheValid_ = true;

    // Save engine state we mutate; restored automatically on scope exit.
    GLStateGuard guard;

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glViewport(0, 0, deviceW, deviceH);

    glUseProgram(program_);
    glUniform1i(samplerLoc_, 0);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);
}

void ColorbarOverlay::drawBars(float dpr, int deviceW, int deviceH,
                                const std::vector<ColorbarData>& bars) {
    if (!isInitialized() || deviceW <= 0 || deviceH <= 0) return;

    bool anyVisible = false;
    for (const auto& b : bars) { if (b.visible) { anyVisible = true; break; } }
    if (!anyVisible) return;

    bool paramsChanged = !imageCacheValid_ || cachedDpr_ != dpr ||
                         cachedW_ != deviceW || cachedH_ != deviceH ||
                         cachedBars_.size() != bars.size();
    if (!paramsChanged) {
        for (size_t i = 0; i < bars.size(); ++i) {
            if (cachedBars_[i].title != bars[i].title ||
                cachedBars_[i].subtitle != bars[i].subtitle ||
                cachedBars_[i].stops != bars[i].stops ||
                cachedBars_[i].tickLabels != bars[i].tickLabels ||
                cachedBars_[i].visible != bars[i].visible) {
                paramsChanged = true;
                break;
            }
        }
    }

    if (paramsChanged) {
        cachedImage_ = buildImage(dpr, deviceW, deviceH, bars);
        cachedBars_ = bars;
        cachedDpr_ = dpr;
        cachedW_ = deviceW;
        cachedH_ = deviceH;
        imageCacheValid_ = true;
        textureCacheValid_ = false;
    }

    if (!textureCacheValid_) {
        uploadAndDraw(cachedImage_, deviceW, deviceH);
    } else {
        // Save engine state we mutate; restored automatically on scope exit.
        GLStateGuard guard;

        glBindTextureUnit(0, tex_);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glViewport(0, 0, deviceW, deviceH);
        glUseProgram(program_);
        glUniform1i(samplerLoc_, 0);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
        glUseProgram(0);
    }
}

