#pragma once
#include <QWidget>
#include <QString>
#include <QVector>
#include <vector>

// A time/iteration-series plot for monitor data (e.g. CFD residuals).
// Unlike LinePlotWidget, the X axis is the sample index/iteration and the
// Y axis is the monitored value (log scale by default).
class MonitorPlotWidget : public QWidget {
public:
    explicit MonitorPlotWidget(QWidget* parent = nullptr);

    void clear();
    void appendPoint(const QString& series, float x, float y);
    void setSeries(const QString& series, const std::vector<float>& xs, const std::vector<float>& ys);
    void removeSeries(const QString& series);
    void setLogScale(bool on);
    void setLegendVisible(bool on);
    void setSeriesVisible(const QString& series, bool on);
    void setManualYRange(bool autoRange, float ymin, float ymax);
    bool logScale() const { return m_logScale; }
    bool legendVisible() const { return m_legendVisible; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    struct Series {
        QString name;
        QColor color;
        std::vector<float> xs;
        std::vector<float> ys;
        bool visible = true;
    };

    int seriesIndex(const QString& name) const;
    QColor colorFor(int index) const;

    QVector<Series> m_series;
    bool m_logScale = true;
    bool m_legendVisible = true;
    bool m_autoY = true;
    float m_yMinManual = 0.0f;
    float m_yMaxManual = 1.0f;
    int m_hoverSeries = -1;
    int m_hoverIndex = -1;
};
