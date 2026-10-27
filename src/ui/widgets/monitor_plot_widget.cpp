#include "monitor_plot_widget.h"
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QToolTip>
#include <QFontMetrics>
#include <algorithm>
#include <cmath>

namespace {
constexpr QRgb kPalette[] = {
    0x4a90e2, 0xB0543A, 0x3AA054, 0xD09030,
    0x8A54B0, 0x30A0A0, 0xC23A3A, 0x7070A0
};
constexpr int kPaletteSize = int(sizeof(kPalette) / sizeof(kPalette[0]));
}

MonitorPlotWidget::MonitorPlotWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumHeight(200);
}

int MonitorPlotWidget::seriesIndex(const QString& name) const {
    for (int i = 0; i < m_series.size(); ++i)
        if (m_series[i].name == name) return i;
    return -1;
}

QColor MonitorPlotWidget::colorFor(int index) const {
    return QColor(kPalette[index % kPaletteSize]);
}

void MonitorPlotWidget::clear() {
    m_series.clear();
    m_hoverSeries = -1;
    m_hoverIndex = -1;
    update();
}

void MonitorPlotWidget::setSeries(const QString& name, const std::vector<float>& xs, const std::vector<float>& ys) {
    int idx = seriesIndex(name);
    if (idx < 0) {
        Series s;
        s.name = name;
        s.color = colorFor(m_series.size());
        s.xs = xs;
        s.ys = ys;
        m_series.push_back(s);
    } else {
        m_series[idx].xs = xs;
        m_series[idx].ys = ys;
    }
    update();
}

void MonitorPlotWidget::appendPoint(const QString& name, float x, float y) {
    int idx = seriesIndex(name);
    if (idx < 0) {
        Series s;
        s.name = name;
        s.color = colorFor(m_series.size());
        s.xs.push_back(x);
        s.ys.push_back(y);
        m_series.push_back(s);
    } else {
        m_series[idx].xs.push_back(x);
        m_series[idx].ys.push_back(y);
    }
    update();
}

void MonitorPlotWidget::removeSeries(const QString& name) {
    int idx = seriesIndex(name);
    if (idx >= 0) m_series.remove(idx);
    update();
}

void MonitorPlotWidget::setLogScale(bool on) {
    m_logScale = on;
    update();
}

void MonitorPlotWidget::setLegendVisible(bool on) {
    m_legendVisible = on;
    update();
}

void MonitorPlotWidget::setSeriesVisible(const QString& series, bool on) {
    int idx = seriesIndex(series);
    if (idx >= 0) { m_series[idx].visible = on; update(); }
}

void MonitorPlotWidget::setManualYRange(bool autoRange, float ymin, float ymax) {
    m_autoY = autoRange;
    m_yMinManual = ymin;
    m_yMaxManual = ymax;
    update();
}

void MonitorPlotWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), palette().color(QPalette::Window));

    // Collect data bounds.
    bool hasData = false;
    float xMin = 0, xMax = 1;
    float yMin = 0, yMax = 1;
    for (const auto& s : m_series) {
        if (!s.visible || s.xs.empty() || s.ys.empty()) continue;
        for (size_t i = 0; i < s.xs.size(); ++i) {
            float x = s.xs[i];
            float y = s.ys[i];
            if (!std::isfinite(x) || !std::isfinite(y)) continue;
            float yy = m_logScale ? std::log10(std::max(y, 1e-30f)) : y;
            if (!hasData) { xMin = xMax = x; yMin = yMax = yy; hasData = true; }
            else { xMin = std::min(xMin, x); xMax = std::max(xMax, x);
                   yMin = std::min(yMin, yy); yMax = std::max(yMax, yy); }
        }
    }
    if (!hasData) {
        p.setPen(palette().color(QPalette::WindowText));
        QFont f = p.font(); f.setItalic(true); p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter, tr("No monitor data"));
        return;
    }
    if (!hasData) {
        // still allow manual range? no data
    }
    if (!m_autoY) {
        float lo = m_yMinManual, hi = m_yMaxManual;
        if (m_logScale) { lo = std::log10(std::max(lo, 1e-30f)); hi = std::log10(std::max(hi, 1e-30f)); }
        yMin = lo; yMax = hi;
        if (std::abs(yMax - yMin) < 1e-12f) { yMin -= 0.5f; yMax += 0.5f; }
    }
    if (std::abs(xMax - xMin) < 1e-12f) xMax = xMin + 1;
    if (std::abs(yMax - yMin) < 1e-12f) { yMin -= 0.5f; yMax += 0.5f; }

    const int titleH = m_legendVisible ? 18 : 0; // title always? we'll reserve
    const int legendH = m_legendVisible ? 18 : 0;
    const int top = 20 + legendH;
    const int left = 60, right = 12, bottom = 40;
    QRect frame(left, top, width() - left - right, height() - top - bottom);
    if (frame.width() <= 0 || frame.height() <= 0) return;

    // Grid
    QColor grid = palette().color(QPalette::WindowText); grid.setAlpha(60);
    p.setPen(QPen(grid, 1));
    for (int i = 0; i <= 4; ++i) {
        int y = frame.top() + i * frame.height() / 4;
        p.drawLine(frame.left(), y, frame.right(), y);
        int x = frame.left() + i * frame.width() / 4;
        p.drawLine(x, frame.top(), x, frame.bottom());
    }
    p.setPen(QPen(palette().color(QPalette::WindowText), 1));
    p.drawRect(frame);

    // Y ticks
    QFont small = p.font(); small.setPointSize(small.pointSize() - 1); p.setFont(small);
    QFontMetrics fm(small);
    for (int i = 0; i <= 4; ++i) {
        float v = yMin + (yMax - yMin) * (1.0f - i / 4.0f);
        int y = frame.top() + i * frame.height() / 4;
        QString txt = m_logScale ? QString::number(std::pow(10.0, v), 'g', 3) : QString::number(v, 'g', 4);
        p.drawText(QRect(0, y - 8, left - 6, 16), Qt::AlignRight | Qt::AlignVCenter, txt);
    }
    // X ticks
    for (int i = 0; i <= 4; ++i) {
        float v = xMin + (xMax - xMin) * (i / 4.0f);
        int x = frame.left() + i * frame.width() / 4;
        QString txt = QString::number(v, 'g', 4);
        QRect r(x - 30, frame.bottom() + 2, 60, bottom - 4);
        p.drawText(r, Qt::AlignHCenter | Qt::AlignTop, txt);
    }

    // Series lines
    auto mapX = [&](float x) { return frame.left() + int((x - xMin) / (xMax - xMin) * frame.width()); };
    auto mapY = [&](float y) {
        float yy = m_logScale ? std::log10(std::max(y, 1e-30f)) : y;
        return frame.bottom() - int((yy - yMin) / (yMax - yMin) * frame.height());
    };
    for (const auto& s : m_series) {
        if (!s.visible || s.xs.size() < 2) continue;
        QPainterPath path; bool pen = false;
        for (size_t i = 0; i < s.xs.size() && i < s.ys.size(); ++i) {
            float x = s.xs[i], y = s.ys[i];
            if (!std::isfinite(x) || !std::isfinite(y)) { pen = false; continue; }
            int px = mapX(x), py = mapY(y);
            if (!pen) { path.moveTo(px, py); pen = true; } else path.lineTo(px, py);
        }
        p.setPen(QPen(s.color, 2));
        p.drawPath(path);
    }

    if (m_legendVisible) {
        int lx = frame.left();
        int ly = 20;
        p.setFont(small);
        for (const auto& s : m_series) {
            if (!s.visible) continue;
            int w = fm.horizontalAdvance(s.name) + 22;
            p.setPen(QPen(s.color, 2));
            p.drawLine(lx, ly + 6, lx + 14, ly + 6);
            p.setPen(palette().color(QPalette::WindowText));
            p.drawText(QRect(lx + 18, ly, w, 14), Qt::AlignLeft | Qt::AlignVCenter, s.name);
            lx += w + 6;
            if (lx > frame.right()) { lx = frame.left(); ly += 16; }
        }
    }

    // Title
    QFont tf = p.font(); tf.setBold(true); p.setFont(tf);
    p.drawText(QRect(frame.left(), 0, frame.width(), 18), Qt::AlignCenter,
               m_logScale ? tr("Monitor (log scale)") : tr("Monitor"));
}

void MonitorPlotWidget::mouseMoveEvent(QMouseEvent* e) {
    // Simple hover: find nearest point across all series.
    m_hoverSeries = -1; m_hoverIndex = -1;
    // Recompute bounds quickly? Reuse paint bounds by storing? For simplicity, just tooltip text.
    for (int si = 0; si < m_series.size(); ++si) {
        const auto& s = m_series[si];
        if (!s.visible) continue;
        for (int i = 0; i < (int)s.xs.size(); ++i) {
            // approximate: not exact without bounds, skip detailed hover
        }
    }
    QWidget::mouseMoveEvent(e);
}

void MonitorPlotWidget::leaveEvent(QEvent* e) {
    m_hoverSeries = -1; m_hoverIndex = -1;
    QWidget::leaveEvent(e);
}
