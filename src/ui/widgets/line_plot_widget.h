#pragma once
#include <QWidget>
#include <vector>
#include "plot_tick_format.h"

class LinePlotWidget : public QWidget {
    Q_OBJECT
public:
    explicit LinePlotWidget(QWidget* parent=nullptr);
    void setData(const std::vector<float>& dists, const std::vector<float>& values,
                 float vMin, float vMax, const QString& field, int placement);
    // Optional second curve sampled at the other placement at the SAME dists
    // points (guaranteed by the caller). Empty vals clears the compare curve.
    void setCompareSeries(const std::vector<float>& vals, const QString& label);
    void setTickFormat(int fmt) { m_tickFormat = (fmt >= 0 && fmt <= 2) ? fmt : 1; }
    void clear();
protected:
    void paintEvent(QPaintEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
private:
    std::vector<float> m_dists, m_vals;
    std::vector<float> m_cVals; QString m_cLabel;
    float m_vMin=0,m_vMax=1;
    QString m_field;
    int m_placement=0;
    int m_tickFormat=1;
    int m_hover=-1;
    QRect m_rect;
    int m_left=84,m_right=16,m_top=22,m_bottom=44;
};
