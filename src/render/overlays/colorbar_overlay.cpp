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
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
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
                      { { 0, 2, 0 }, { 1, 2, 2 * sizeof(float) } }, GL_DYNAMIC_DRAW);
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
    for (auto& tc : cachedTextures_) {
        if (tc.texId) glDeleteTextures(1, &tc.texId);
    }
    cachedTextures_.clear();
    vao_.reset();
    vbo_.reset();
    program_.reset();
    samplerLoc_ = -1;
}

void ColorbarOverlay::markDirty() {
    for (auto& tc : cachedTextures_) tc.valid = false;
}

QSize ColorbarOverlay::singleBarSize(float dpr) const {
    const int barW = 200;
    const int barH = 12;
    const int gap = 4;
    const int tickLen = 5;
    const int pad = 8;

    QFont labelFont;
    labelFont.setPixelSize(11);
    const int labelH = QFontMetrics(labelFont).height();
    QFont tickFont;
    tickFont.setPixelSize(9);
    const int tickH = QFontMetrics(tickFont).height();

    const int w = static_cast<int>((barW + pad * 2) * dpr);
    const int h = static_cast<int>((labelH + gap + barH + tickLen + tickH + pad * 2) * dpr);
    return QSize(w, h);
}

QImage ColorbarOverlay::buildSingleBarImage(float dpr, const ColorbarData& bar) const {
    QSize sz = singleBarSize(dpr);
    QImage img(sz, QImage::Format_ARGB32);
    img.fill(Qt::transparent);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const int barW = static_cast<int>(200 * dpr);
    const int barH = static_cast<int>(12 * dpr);
    const int gap = static_cast<int>(4 * dpr);
    const int tickLen = static_cast<int>(5 * dpr);
    const int pad = static_cast<int>(8 * dpr);

    QFont labelFont;
    labelFont.setPixelSize(static_cast<int>(11 * dpr));
    const QFontMetrics labelFm(labelFont);
    const int labelH = labelFm.height();

    QFont tickFont;
    tickFont.setPixelSize(static_cast<int>(9 * dpr));
    const QFontMetrics tickFm(tickFont);
    const int tickH = tickFm.height();

    const int blockX = pad;
    const int blockY = pad;

    // Title with annotation prefix
    p.setFont(labelFont);
    p.setPen(QColor("#e8e8e8"));
    QString displayTitle = bar.title;
    if (!bar.subtitle.isEmpty())
        displayTitle = QString("[%1] %2").arg(bar.subtitle, bar.title);
    const QRect titleRect(blockX, blockY, barW, labelH);
    p.drawText(titleRect, Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextSingleLine, displayTitle);

    const int barX = blockX;
    const int barY = blockY + labelH + gap;

    // Gradient bar
    {
        QLinearGradient grad(static_cast<qreal>(barX), 0.0,
                             static_cast<qreal>(barX + barW), 0.0);
        const int n = bar.stops.size();
        for (int i = 0; i < n; ++i) {
            const QVariantList s = bar.stops[i].toList();
            const float t = s[0].toFloat();
            grad.setColorAt(static_cast<qreal>(t), QColor::fromRgbF(
                qBound(0.0, s[1].toDouble(), 1.0),
                qBound(0.0, s[2].toDouble(), 1.0),
                qBound(0.0, s[3].toDouble(), 1.0)));
        }
        QPainterPath barPath;
        const int radius = static_cast<int>(3 * dpr);
        barPath.addRoundedRect(QRectF(barX, barY, barW, barH), radius, radius);
        p.save();
        p.setClipPath(barPath);
        p.fillRect(barX, barY, barW, barH, grad);
        p.restore();
        p.setPen(QColor(0, 0, 0, 160));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(barX, barY, barW, barH), radius, radius);
    }

    // Ticks
    p.setFont(tickFont);
    p.setPen(QColor("#cccccc"));
    const int tickCount = bar.tickLabels.size();
    if (tickCount > 0) {
        const int tickY = barY + barH;
        for (int i = 0; i < tickCount; ++i) {
            const float frac = tickCount > 1
                ? static_cast<float>(i) / static_cast<float>(tickCount - 1)
                : 0.0f;
            const int tx = barX + static_cast<int>(frac * barW);
            p.drawLine(tx, tickY, tx, tickY + tickLen);
            const QString& lbl = bar.tickLabels[i];
            const int lw = tickFm.horizontalAdvance(lbl);
            const QRect lblRect(tx - lw / 2, tickY + tickLen, lw, tickH);
            p.drawText(lblRect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextSingleLine, lbl);
        }
    }

    return img;
}

QString ColorbarOverlay::barKey(float dpr, const ColorbarData& bar) {
    // Content hash: dpr + title + subtitle + frac + stops (t + rgb) + ticks
    QString k = QString::number(dpr, 'f', 3) + "|" + bar.title + "|" + bar.subtitle
                + "|" + QString::number(bar.fracX, 'f', 4) + "," + QString::number(bar.fracY, 'f', 4) + "|";
    for (int i=0;i<bar.stops.size();++i) {
        auto s = bar.stops[i].toList();
        if (s.size()>=4) k += QString::number(s[0].toFloat(),'f',4) + ":" +
                             QString::number(s[1].toDouble(),'f',4) + "," +
                             QString::number(s[2].toDouble(),'f',4) + "," +
                             QString::number(s[3].toDouble(),'f',4) + ";";
        else k += bar.stops[i].toString() + ";";
    }
    k += "|";
    for (auto &lbl: bar.tickLabels) k += lbl + ";";
    return k;
}

void ColorbarOverlay::drawSingleBar(float dpr, int deviceW, int deviceH,
                                    const ColorbarData& bar) {
    QString key = barKey(dpr, bar);
    TextureCache* cache = nullptr;
    for (auto& tc : cachedTextures_) {
        if (tc.valid && tc.key == key && qFuzzyCompare(tc.dpr, dpr)) {
            cache = &tc;
            break;
        }
    }
    if (!cache) {
        for (auto& tc : cachedTextures_) {
            if (!tc.valid) { cache = &tc; break; }
        }
        if (!cache) {
            cachedTextures_.push_back({});
            cache = &cachedTextures_.back();
        }
        cache->image = buildSingleBarImage(dpr, bar);
        cache->key = key;
        cache->dpr = dpr;
        if (cache->texId) glDeleteTextures(1, &cache->texId);
        glCreateTextures(GL_TEXTURE_2D, 1, &cache->texId);
        glTextureParameteri(cache->texId, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(cache->texId, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(cache->texId, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(cache->texId, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        cache->w = cache->image.width();
        cache->h = cache->image.height();
        cache->valid = true;
        // Allocate immutable storage once
        QImage gl = cache->image.convertToFormat(QImage::Format_RGBA8888);
        gl = gl.flipped(Qt::Vertical);
        glTextureStorage2D(cache->texId, 1, GL_RGBA8, gl.width(), gl.height());
        glTextureSubImage2D(cache->texId, 0, 0, 0, gl.width(), gl.height(), GL_RGBA, GL_UNSIGNED_BYTE, gl.constBits());
        glBindTextureUnit(0, cache->texId);
    } else {
        glBindTextureUnit(0, cache->texId);
    }

    // Position in clip space
    const int px = static_cast<int>(bar.fracX * (deviceW - cache->w));
    const int py = static_cast<int>(bar.fracY * (deviceH - cache->h));
    const float left = (2.0f * px / deviceW) - 1.0f;
    const float right = (2.0f * (px + cache->w) / deviceW) - 1.0f;
    const float top = 1.0f - (2.0f * py / deviceH);
    const float bottom = 1.0f - (2.0f * (py + cache->h) / deviceH);

    const float verts[6][4] = {
        {left,  bottom, 0.0f, 0.0f},
        {right, bottom, 1.0f, 0.0f},
        {left,  top,    0.0f, 1.0f},
        {left,  top,    0.0f, 1.0f},
        {right, bottom, 1.0f, 0.0f},
        {right, top,    1.0f, 1.0f},
    };

    GLStateGuard guard;
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glViewport(0, 0, deviceW, deviceH);
    glUseProgram(program_);
    glUniform1i(samplerLoc_, 0);
    glBindVertexArray(vao_);
    glNamedBufferData(vbo_, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);
}

void ColorbarOverlay::drawBars(float dpr, int deviceW, int deviceH,
                                const std::vector<ColorbarData>& bars) {
    if (!isInitialized() || deviceW <= 0 || deviceH <= 0) return;
    cachedDpr_ = dpr;

    for (const auto& bar : bars) {
        if (!bar.visible || bar.stops.isEmpty() || bar.title.isEmpty()) continue;
        drawSingleBar(dpr, deviceW, deviceH, bar);
    }
}

int ColorbarOverlay::barIndexAt(float dpr, int deviceW, int deviceH,
                                 const std::vector<ColorbarData>& bars, int px, int py) const {
    for (int i = 0; i < static_cast<int>(bars.size()); ++i) {
        const auto& bar = bars[i];
        if (!bar.visible || bar.stops.isEmpty() || bar.title.isEmpty()) continue;
        QRectF rect = barRectAt(dpr, deviceW, deviceH, bar);
        if (rect.contains(static_cast<qreal>(px), static_cast<qreal>(py))) return i;
    }
    return -1;
}

QRectF ColorbarOverlay::barRectAt(float dpr, int deviceW, int deviceH,
                                   const ColorbarData& bar) const {
    QSize sz = singleBarSize(dpr);
    const int px = static_cast<int>(bar.fracX * (deviceW - sz.width()));
    const int py = static_cast<int>(bar.fracY * (deviceH - sz.height()));
    return QRectF(px, py, sz.width(), sz.height());
}
