#include "plot_widget.h"
#include <QPainter>
#include <QMouseEvent>
#include <QToolTip>
#include <QFontMetrics>
#include <algorithm>
#include <cmath>

PlotWidget::PlotWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumHeight(200);
}

void PlotWidget::setHistogram(const std::vector<uint32_t>& bins, float dataMin, float dataMax,
                               const QString& fieldName, size_t numValues) {
    m_bins = bins;
    m_dataMin = dataMin;
    m_dataMax = dataMax;
    m_fieldName = fieldName;
    m_numValues = numValues;
    m_maxBin = 0;
    for (auto v : m_bins) m_maxBin = std::max(m_maxBin, v);
    m_hoverBin = -1;
    update();
}

void PlotWidget::clear() {
    m_bins.clear();
    m_maxBin = 0;
    m_hoverBin = -1;
    update();
}

void PlotWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), palette().color(QPalette::Window));

    if (m_bins.empty()) {
        p.setPen(palette().color(QPalette::WindowText));
        QFont f = p.font(); f.setItalic(true); p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter, tr("No histogram data"));
        return;
    }

    // Layout
    m_plotRect = QRect(m_leftPad, m_topPad, width() - m_leftPad - m_rightPad, height() - m_topPad - m_bottomPad);
    if (m_plotRect.width() <= 0 || m_plotRect.height() <= 0) return;

    // Grid + axes — WindowText with moderate alpha for visibility in both themes (was palette(Mid) DotLine, too faint)
    QColor gridCol = palette().color(QPalette::WindowText);
    gridCol.setAlpha(72);
    p.setPen(QPen(gridCol, 1, Qt::SolidLine));
    for (int i = 0; i <= 4; ++i) {
        int y = m_plotRect.top() + i * m_plotRect.height() / 4;
        p.drawLine(m_plotRect.left(), y, m_plotRect.right(), y);
    }
    // Vertical grid as well for better readability
    for (int i = 1; i < 4; ++i) {
        int x = m_plotRect.left() + i * m_plotRect.width() / 4;
        p.drawLine(x, m_plotRect.top(), x, m_plotRect.bottom());
    }
    p.setPen(QPen(palette().color(QPalette::WindowText), 1));
    p.drawRect(m_plotRect);

    // Bars
    const int n = static_cast<int>(m_bins.size());
    if (n == 0) return;
    double barW = static_cast<double>(m_plotRect.width()) / n;
    double maxBinD = (m_maxBin == 0) ? 1.0 : static_cast<double>(m_maxBin);
    QColor barColor(0x4a, 0x90, 0xe2);
    QColor hoverColor(0xff, 0x8c, 0x00);
    for (int i = 0; i < n; ++i) {
        double h = (static_cast<double>(m_bins[i]) / maxBinD) * m_plotRect.height();
        QRectF bar(m_plotRect.left() + i * barW + 0.5, m_plotRect.bottom() - h, barW - 1.0, h);
        if (bar.height() < 0.5) continue;
        p.fillRect(bar, (i == m_hoverBin) ? hoverColor : barColor);
    }

    // Axes labels
    p.setPen(palette().color(QPalette::WindowText));
    QFont small = p.font(); small.setPointSize(small.pointSize() - 1); p.setFont(small);
    QFontMetrics fm(small);
    // Y ticks (count)
    for (int i = 0; i <= 4; ++i) {
        uint32_t val = static_cast<uint32_t>(std::round((1.0 - i / 4.0) * maxBinD));
        int y = m_plotRect.top() + i * m_plotRect.height() / 4;
        QString txt = QString::number(val);
        p.drawText(QRect(0, y - 8, m_leftPad - 4, 16), Qt::AlignRight | Qt::AlignVCenter, txt);
    }
    // X ticks (scalar value) — keep last/first label inside chart to avoid overflow
    for (int i = 0; i <= 4; ++i) {
        float v = m_dataMin + (m_dataMax - m_dataMin) * (i / 4.0f);
        int x = m_plotRect.left() + i * m_plotRect.width() / 4;
        QString txt = QString::number(v, 'g', 4);
        // Elide if too wide for the 60px slot
        txt = fm.elidedText(txt, Qt::ElideRight, 58);
        QRect r;
        int flags = Qt::AlignTop;
        if (i == 0) {
            r = QRect(x, m_plotRect.bottom() + 2, 60, m_bottomPad - 4);
            flags |= Qt::AlignLeft | Qt::AlignTop;
            // Keep strictly inside widget
            if (r.right() > width() - 2) r.moveRight(width() - 2);
        } else if (i == 4) {
            r = QRect(x - 60, m_plotRect.bottom() + 2, 60, m_bottomPad - 4);
            flags |= Qt::AlignRight | Qt::AlignTop;
            if (r.left() < m_leftPad) r.moveLeft(m_leftPad);
            // Ensure not spilling past right edge
            if (r.right() > width() - 2) r.moveRight(width() - 2);
        } else {
            r = QRect(x - 30, m_plotRect.bottom() + 2, 60, m_bottomPad - 4);
            flags |= Qt::AlignHCenter | Qt::AlignTop;
        }
        p.drawText(r, flags, txt);
    }
    // Title
    {
        QFont titleFont = p.font(); titleFont.setBold(true); p.setFont(titleFont);
        QString title = m_fieldName.isEmpty() ? tr("Histogram") : tr("Histogram: %1  (n=%2)").arg(m_fieldName).arg(m_numValues);
        p.drawText(QRect(m_plotRect.left(), 0, m_plotRect.width(), m_topPad), Qt::AlignCenter, title);
    }
}

void PlotWidget::mouseMoveEvent(QMouseEvent* event) {
    if (m_bins.empty() || m_plotRect.width() <= 0) { m_hoverBin = -1; update(); return; }
    QPoint pos = event->pos();
    if (!m_plotRect.contains(pos)) { if (m_hoverBin != -1) { m_hoverBin = -1; update(); } return; }
    double barW = static_cast<double>(m_plotRect.width()) / m_bins.size();
    int bin = static_cast<int>((pos.x() - m_plotRect.left()) / barW);
    bin = std::clamp(bin, 0, static_cast<int>(m_bins.size()) - 1);
    if (bin != m_hoverBin) { m_hoverBin = bin; update(); }
    float binLo = m_dataMin + (m_dataMax - m_dataMin) * (bin / static_cast<float>(m_bins.size()));
    float binHi = m_dataMin + (m_dataMax - m_dataMin) * ((bin + 1) / static_cast<float>(m_bins.size()));
    QString tip = tr("Bin %1 [%2 .. %3): %4").arg(bin).arg(binLo, 0, 'g', 4).arg(binHi, 0, 'g', 4).arg(m_bins[bin]);
    QToolTip::showText(event->globalPosition().toPoint(), tip, this);
}

void PlotWidget::leaveEvent(QEvent*) {
    if (m_hoverBin != -1) { m_hoverBin = -1; update(); }
}

void PlotWidget::resizeEvent(QResizeEvent*) { update(); }
