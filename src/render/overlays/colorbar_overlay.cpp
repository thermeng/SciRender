#include "render/overlays/colorbar_overlay.h"

#include <glad/gl.h>
#include "render/foundation/shader_utils.h"
#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <QFontMetrics>
#include <QOpenGLContext>
#include <QPalette>
#include <cstring>
#include <cmath>

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

enum class FontKind { Title, Tick };

QFont makeFont(const ColorbarStyle& st, FontKind kind, float dpr) {
    QFont f;
    if (!st.fontFamily.isEmpty()) f.setFamily(st.fontFamily);
    f.setBold(st.fontBold);
    f.setItalic(st.fontItalic);
    const float basePx = (kind == FontKind::Title) ? 11.0f : 9.0f;
    const float scale = (kind == FontKind::Title) ? st.fontScale : st.tickFontScale;
    f.setPixelSize(static_cast<int>(basePx * scale * dpr));
    return f;
}

QString displayNameOf(const ColorbarData& bar) {
    QString t = bar.title;
    if (!bar.units.isEmpty())
        t += QString(" (%1)").arg(bar.units);
    return t;
}

QString annotationOf(const ColorbarData& bar) {
    return bar.subtitle.isEmpty() ? QString() : QString("[%1]").arg(bar.subtitle);
}

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

// Greedy label selection along the reading direction (bottom→top when
// vertical, left→right otherwise). The end label (max) always wins: interior
// selections that block it are dropped.
void ColorbarOverlay::selectTickLabels(std::vector<TickItem>& ticks,
                                       const QStringList& labels, int minGap, bool vertical) {
    const int n = static_cast<int>(ticks.size());
    if (n <= 1) return;
    auto fitsAfter = [&](int cand, int prev) {
        if (labels[cand] == labels[prev]) return false;
        return vertical
            ? ticks[cand].label.bottom() <= ticks[prev].label.top() - minGap
            : ticks[cand].label.left() >= ticks[prev].label.right() + minGap;
    };
    std::vector<int> selected{0};
    for (int i = 1; i < n; ++i) {
        if (i == n - 1) {
            while (!selected.empty()) {
                if (fitsAfter(i, selected.back())) { selected.push_back(i); break; }
                if (selected.size() == 1) break; // min label keeps priority
                selected.pop_back();
            }
        } else if (fitsAfter(i, selected.back())) {
            selected.push_back(i);
        }
    }
    std::vector<char> keep(n, 0);
    for (int idx : selected) keep[idx] = 1;
    for (int i = 0; i < n; ++i) {
        if (!keep[i]) ticks[i].label = QRect();
    }
}

ColorbarOverlay::Layout ColorbarOverlay::computeLayout(float dpr, const ColorbarData& bar) const {
    Layout lay;
    const ColorbarStyle& st = bar.style;

    const QFontMetrics titleFm(makeFont(st, FontKind::Title, dpr));
    const QFontMetrics tickFm(makeFont(st, FontKind::Tick, dpr));

    const int gap = static_cast<int>(4 * dpr);
    const int tickLen = static_cast<int>(5 * dpr);
    const int pad = static_cast<int>(8 * dpr);
    const int titleH = titleFm.height();
    const int tickH = tickFm.height();
    const int barMain = static_cast<int>(200 * st.lengthScale * dpr);
    const int barCross = static_cast<int>(12 * st.thicknessScale * dpr);
    const int labelGap = static_cast<int>(3 * dpr);
    const int minGap = static_cast<int>(2 * dpr);
    const int n = bar.tickLabels.size();

    // Annotation line ("[Scalar]") sits above the field name line.
    const bool hasAnnotation = st.showAnnotation && !bar.subtitle.isEmpty();
    const int annotationH = hasAnnotation ? titleH : 0;
    const int nameW = titleFm.horizontalAdvance(displayNameOf(bar));
    const int annW = hasAnnotation ? titleFm.horizontalAdvance(annotationOf(bar)) : 0;

    int lwMax = 0;
    for (const QString& lbl : bar.tickLabels)
        lwMax = qMax(lwMax, tickFm.horizontalAdvance(lbl));

    if (st.orientation == ColorbarStyle::Vertical) {
        // Max at top (viz convention): tick i (min + frac*range) sits at
        // barBottom - frac*barMain; labels run up the right side.
        const int barX = pad;
        const int barY = pad + annotationH + titleH + gap;
        const int titleW = qMax(nameW, annW);
        const int contentW = qMax(barCross + tickLen + labelGap + lwMax, titleW);
        const int w = pad + contentW + pad;
        const int h = barY + barMain + pad;
        lay.size = QSize(w, h);
        lay.panel = QRect(QPoint(0, 0), lay.size);
        lay.annotation = hasAnnotation ? QRect(pad, pad, w - 2 * pad, titleH) : QRect();
        lay.title = QRect(pad, pad + annotationH, w - 2 * pad, titleH);
        lay.bar = QRect(barX, barY, barCross, barMain);

        const int barTop = barY;
        const int barBottom = barY + barMain;
        const int tickX = barX + barCross;
        lay.ticks.reserve(n);
        for (int i = 0; i < n; ++i) {
            const float frac = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;
            const int ty = barBottom - static_cast<int>(frac * barMain);
            TickItem item;
            item.index = i;
            item.markFrom = QPointF(tickX, ty);
            item.markTo = QPointF(tickX + tickLen, ty);
            const QString& lbl = bar.tickLabels[i];
            const int lw = tickFm.horizontalAdvance(lbl);
            QRect lblRect(tickX + tickLen + labelGap, ty - tickH / 2, lw, tickH);
            if (i == 0) lblRect.moveBottom(barBottom);
            else if (i == n - 1) lblRect.moveTop(barTop);
            item.label = lblRect;
            lay.ticks.push_back(item);
        }
        selectTickLabels(lay.ticks, bar.tickLabels, minGap, true);
        return lay;
    }

    // Horizontal layout.
    const int barX = pad;
    const int barY = pad + annotationH + titleH + gap;
    lay.size = QSize(barX + barMain + pad, barY + barCross + tickLen + tickH + pad);
    lay.panel = QRect(QPoint(0, 0), lay.size);
    lay.annotation = hasAnnotation ? QRect(barX, pad, barMain, titleH) : QRect();
    lay.title = QRect(barX, pad + annotationH, barMain, titleH);
    lay.bar = QRect(barX, barY, barMain, barCross);

    // Tick marks at full density; labels thinned so neighbors never overlap.
    const int tickY = barY + barCross;
    lay.ticks.reserve(n);
    for (int i = 0; i < n; ++i) {
        const float frac = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;
        const int tx = barX + static_cast<int>(frac * barMain);
        TickItem item;
        item.index = i;
        item.markFrom = QPointF(tx, tickY);
        item.markTo = QPointF(tx, tickY + tickLen);
        const QString& lbl = bar.tickLabels[i];
        const int lw = tickFm.horizontalAdvance(lbl);
        QRect lblRect(tx - lw / 2, tickY + tickLen, lw, tickH);
        if (i == 0) lblRect.moveLeft(barX);
        else if (i == n - 1) lblRect.moveRight(barX + barMain);
        item.label = lblRect;
        lay.ticks.push_back(item);
    }
    selectTickLabels(lay.ticks, bar.tickLabels, minGap, false);
    return lay;
}

QImage ColorbarOverlay::buildSingleBarImage(float dpr, const ColorbarData& bar) const {
    const Layout lay = computeLayout(dpr, bar);
    QImage img(lay.size, QImage::Format_ARGB32);
    img.fill(Qt::transparent);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const ColorbarStyle& st = bar.style;

    // Background panel
    if (st.panelEnabled) {
        QColor panel = QApplication::palette().color(QPalette::Window);
        panel.setAlphaF(qBound(0.0, static_cast<double>(st.panelOpacity), 1.0));
        QPainterPath path;
        const int radius = static_cast<int>(4 * dpr);
        path.addRoundedRect(lay.panel, radius, radius);
        p.fillPath(path, panel);
    }

    // Annotation line above the field name line
    const QFont tFont = makeFont(st, FontKind::Title, dpr);
    p.setFont(tFont);
    if (!lay.annotation.isEmpty()) {
        p.setPen(QApplication::palette().color(QPalette::Text));
        const QString ann = annotationOf(bar);
        const QString annElided = QFontMetrics(tFont).elidedText(ann, Qt::ElideRight, lay.annotation.width());
        p.drawText(lay.annotation, Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextSingleLine, annElided);
    }

    // Field name line with optional unit suffix
    p.setFont(tFont);
    p.setPen(QApplication::palette().color(QPalette::Text));
    const QString displayTitle = displayNameOf(bar);
    const QString elided = QFontMetrics(tFont).elidedText(displayTitle, Qt::ElideRight, lay.title.width());
    p.drawText(lay.title, Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextSingleLine, elided);

    // Gradient bar (vertical bars read bottom→min, top→max)
    {
        const bool vertical = st.orientation == ColorbarStyle::Vertical;
        const int n = bar.stops.size();
        const int bands = bar.bandCount > 1 ? bar.bandCount : 0;
        if (bands > 1) {
            // Discrete bands: draw N rectangles, each filled with the quantized color.
            for (int b = 0; b < bands; ++b) {
                float t0 = static_cast<float>(b) / static_cast<float>(bands);
                float t1 = static_cast<float>(b + 1) / static_cast<float>(bands);
                // Sample color at band center for the fill.
                float tc = (static_cast<float>(b) + 0.5f) / static_cast<float>(bands);
                // Map tc to a stop color by interpolating between gradient stops.
                QColor col;
                for (int i = 0; i < n - 1; ++i) {
                    QVariantList s0 = bar.stops[i].toList();
                    QVariantList s1 = bar.stops[i + 1].toList();
                    float aT = s0[0].toFloat();
                    float bT = s1[0].toFloat();
                    if (tc >= aT && tc <= bT) {
                        float f = (bT - aT) > 1e-9f ? (tc - aT) / (bT - aT) : 0.0f;
                        col = QColor::fromRgbF(
                            qBound(0.0, s0[1].toDouble() + f * (s1[1].toDouble() - s0[1].toDouble()), 1.0),
                            qBound(0.0, s0[2].toDouble() + f * (s1[2].toDouble() - s0[2].toDouble()), 1.0),
                            qBound(0.0, s0[3].toDouble() + f * (s1[3].toDouble() - s0[3].toDouble()), 1.0));
                        break;
                    }
                }
                QRect bandRect;
                if (vertical) {
                    int y0 = lay.bar.top() + static_cast<int>(t0 * lay.bar.height());
                    int y1 = lay.bar.top() + static_cast<int>(t1 * lay.bar.height());
                    bandRect = QRect(lay.bar.left(), y0, lay.bar.width(), y1 - y0);
                } else {
                    int x0 = lay.bar.left() + static_cast<int>(t0 * lay.bar.width());
                    int x1 = lay.bar.left() + static_cast<int>(t1 * lay.bar.width());
                    bandRect = QRect(x0, lay.bar.top(), x1 - x0, lay.bar.height());
                }
                p.fillRect(bandRect, col);
            }
            // No border — sharp publication style (keep fill only).
        } else {
            QLinearGradient grad(lay.bar.left(), 0.0, lay.bar.right(), 0.0);
            if (vertical) {
                grad.setStart(0.0, lay.bar.top());
                grad.setFinalStop(0.0, lay.bar.bottom());
            }
            for (int i = 0; i < n; ++i) {
                const QVariantList s = bar.stops[i].toList();
                const float t = s[0].toFloat();
                grad.setColorAt(vertical ? (1.0 - static_cast<qreal>(t)) : static_cast<qreal>(t),
                    QColor::fromRgbF(
                        qBound(0.0, s[1].toDouble(), 1.0),
                        qBound(0.0, s[2].toDouble(), 1.0),
                        qBound(0.0, s[3].toDouble(), 1.0)));
            }
            QPainterPath barPath;
            barPath.addRect(lay.bar);
            p.save();
            p.setClipPath(barPath);
            p.fillRect(lay.bar, grad);
            p.restore();
            // No border — sharp publication style.
        }
    }

    // Tick marks + thinned labels
    p.setFont(makeFont(st, FontKind::Tick, dpr));
    p.setPen(QApplication::palette().color(QPalette::Text));
    for (const TickItem& item : lay.ticks) {
        p.drawLine(item.markFrom, item.markTo);
        if (item.label.isEmpty()) continue;
        p.drawText(item.label, Qt::AlignLeft | Qt::AlignTop | Qt::TextSingleLine,
                   bar.tickLabels[item.index]);
    }

    return img;
}

QString ColorbarOverlay::barKey(float dpr, const ColorbarData& bar) {
    // Content hash: dpr + title + subtitle + units + stops (t + rgb) + ticks + style.
    // Position intentionally excluded: it is applied via the quad transform, so
    // drags reuse the cached texture instead of forcing a re-upload per frame.
    const ColorbarStyle& st = bar.style;
    QString k = QString::number(dpr, 'f', 3) + "|" + bar.title + "|" + bar.subtitle
                + "|" + bar.units
                + "|" + QString::number(static_cast<int>(st.orientation))
                + "|" + QString::number(bar.bandCount)
                + "|" + st.fontFamily
                + "," + (st.fontBold ? "b1" : "b0")
                + "," + (st.fontItalic ? "i1" : "i0")
                + "," + QString::number(st.fontScale, 'f', 3)
                + "," + QString::number(st.tickFontScale, 'f', 3)
                + "," + QString::number(st.lengthScale, 'f', 3)
                + "," + QString::number(st.thicknessScale, 'f', 3)
                + "," + (st.panelEnabled ? "p1" : "p0")
                + "," + QString::number(st.panelOpacity, 'f', 3)
                + "," + (st.showAnnotation ? "a1" : "a0")
                + "|";
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
#if QT_VERSION >= QT_VERSION_CHECK(6, 11, 0)
        QImage gl = cache->image.convertToFormat(QImage::Format_RGBA8888).flipped(Qt::Vertical);
#else
        QImage gl = cache->image.convertToFormat(QImage::Format_RGBA8888).mirrored(false, true);
#endif
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
    const QSize sz = computeLayout(dpr, bar).size;
    const int px = static_cast<int>(bar.fracX * (deviceW - sz.width()));
    const int py = static_cast<int>(bar.fracY * (deviceH - sz.height()));
    return QRectF(px, py, sz.width(), sz.height());
}
