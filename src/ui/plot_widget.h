#pragma once
#include <QWidget>
#include <QVector>
#include <cstdint>
#include <vector>

class PlotWidget : public QWidget {
    Q_OBJECT
public:
    explicit PlotWidget(QWidget* parent = nullptr);

    void setHistogram(const std::vector<uint32_t>& bins, float dataMin, float dataMax,
                      const QString& fieldName, size_t numValues);
    void clear();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    std::vector<uint32_t> m_bins;
    float m_dataMin = 0.0f, m_dataMax = 1.0f;
    QString m_fieldName;
    size_t m_numValues = 0;
    uint32_t m_maxBin = 0;
    int m_hoverBin = -1;

    QRect m_plotRect; // computed in paintEvent
    int m_leftPad = 56, m_rightPad = 12, m_topPad = 16, m_bottomPad = 28;
};
