#pragma once
#include <QWidget>
#include <QString>
#include <vector>
#include "plot_tick_format.h"

// One plotted curve. Active line = what the sidebar samples right now;
// pinned lines = previously sampled snapshots (see PlotWindow).
struct LineSeries {
    std::vector<float> dists;
    std::vector<float> vals;
    QString label;     // short legend text, e.g. "L2"
    QString tipLabel;  // fuller tooltip prefix, e.g. "L2 Velocity [Magnitude] [Cell]"
    bool hidden = false; // toggled via legend chip; still exported? no — hidden lines skip CSV
};

class LinePlotWidget : public QWidget {
    Q_OBJECT
public:
    explicit LinePlotWidget(QWidget* parent=nullptr);
    void setData(const std::vector<float>& dists, const std::vector<float>& values,
                 float vMin, float vMax, const QString& field, int placement);
    // Optional second curve sampled at the other placement at the SAME dists
    // points (guaranteed by the caller). Empty vals clears the compare curve.
    void setCompareSeries(const std::vector<float>& vals, const QString& label);
    // Replaces the full pinned-line set (each series maps its own dists to the
    // full plot width). Empty list clears all pins.
    void setPinnedLines(std::vector<LineSeries> lines);
    void setTickFormat(int fmt) { m_tickFormat = (fmt >= 0 && fmt <= 2) ? fmt : 1; }
    void clear();
Q_SIGNALS:
    // Legend chip gestures on pinned lines (index = position in the pinned list).
    void pinnedVisibilityToggled(int index);
    void pinnedRemoveRequested(int index);
protected:
    void paintEvent(QPaintEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
private:
    std::vector<float> m_dists, m_vals;
    std::vector<float> m_cVals; QString m_cLabel;
    std::vector<LineSeries> m_pinned;
    float m_vMin=0,m_vMax=1;
    QString m_field;
    int m_placement=0;
    int m_tickFormat=1;
    int m_hover=-1;      // index of hovered sample
    int m_hoverLine=-1;  // -1 = active line, else pinned index
    QRect m_rect;
    int m_left=84,m_right=16,m_top=22,m_bottom=44;

    // Legend chip hit rects, rebuilt every paint. pinned >= 0 → interactive.
    struct ChipHit { QRect r; int pinned = -1; };
    std::vector<ChipHit> m_chips;
};
