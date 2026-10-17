#pragma once
#include <QWidget>
#include <QLabel>
#include <QTimer>
#include <vector>
#include "plot_widget.h"
#include "line_plot_widget.h"

class RenderSettings;
class QStackedWidget;

// Availability of the two placement options for a plot field, resolved against
// the uploaded mesh + post snapshot. Shared with the sidebar plots page, which
// owns the field/placement combos and uses it to enable/disable the Cell item.
struct PlotPlacementAvail { bool hasPoint = false; bool hasCell = false; };
PlotPlacementAvail plotPlacementAvailability(RenderSettings* settings, const QString& field);

// Canvas-only plots surface. All parameters (mode, field, placement, bins,
// samples, P0/P1) live in RenderSettings and are edited from the sidebar
// Plots page; PlotWindow just renders the active canvas and a status strip.
class PlotWindow : public QWidget {
    Q_OBJECT
public:
    explicit PlotWindow(RenderSettings* settings, QWidget* parent = nullptr);
    ~PlotWindow() override = default;

    int mode() const { return m_mode; }
    void setMode(int mode);            // 0 = histogram, 1 = line plot

    void refreshData();                // recompute both canvases from settings
    void scheduleRecompute();          // debounced recompute (shared timer)

public:
    // Mode-aware export entry points for the sidebar export buttons.
    void exportCsv();
    void exportPng();

Q_SIGNALS:
    void requestCloseHistogram();
    void requestOpenControls();

private slots:
    void onPlotParamsChanged();
    void onSettingsMeshDataUpdated();

private:
    void recomputeHistogram();
    void recomputeLine();
    void updateStatsLabel();
    void exportHistogramCsv();
    void exportHistogramPng();
    void exportLineCsv();
    void exportLinePng();

    RenderSettings* m_settings = nullptr;
    QStackedWidget* m_stack = nullptr;
    PlotWidget* m_plot = nullptr;
    LinePlotWidget* m_linePlot = nullptr;
    QLabel* m_statsLabel = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLabel* m_lineStats = nullptr;
    QLabel* m_lineStatus = nullptr;
    QTimer* m_recomputeTimer = nullptr;
    int m_mode = 0;
    // cached line data for export
    std::vector<float> m_lineDists, m_lineVals;
    float m_lineMin=0,m_lineMax=1;
    QString m_lineField;
    int m_linePlacement=0;
};
