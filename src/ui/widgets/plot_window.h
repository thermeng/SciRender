#pragma once
#include <QWidget>
#include <QLabel>
#include <QTimer>
#include <vector>
#include <QStringList>
#include <glm/glm.hpp>
#include "plot_widget.h"
#include "line_plot_widget.h"
#include "monitor_plot_widget.h"

class RenderSettings;
class QStackedWidget;
namespace FieldStore { struct Context; }

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
    void setMode(int mode);            // 0 = histogram, 1 = line plot, 2 = residuals

    void refreshData();                // recompute both canvases from settings
    void clearResiduals();
    void appendResidual(int iteration, int dim, double continuity,
                       double momU, double momV, double momW,
                       double k, double omega, bool hasTurb);
    void setMonitorLogScale(bool on);
    void setMonitorLegendVisible(bool on);
    void setMonitorHistoryWindow(int n);
    void setMonitorYRange(bool autoRange, float ymin, float ymax);
    void setMonitorSeriesVisible(const QString& name, bool on);
    MonitorPlotWidget* monitorPlot() const { return m_monitorPlot; }
    QStringList monitorAvailableSeries() const { return m_monitorAvailableSeries; }
    void scheduleRecompute();          // debounced recompute (shared timer)

public:
    // Mode-aware export entry points for the sidebar export buttons.
    void exportCsv();
    void exportPng();
    void exportMonitorCsv();
    void exportMonitorPng();

Q_SIGNALS:
signals:
    void requestCloseHistogram();
    void requestOpenControls();
    void pinnedListChanged();
    void monitorSeriesChanged();

public slots:
    void togglePinnedLine(int index);
    void removePinnedLine(int index);

private slots:
    void onPlotParamsChanged();
    void onSettingsMeshDataUpdated();

private:
    void recomputeHistogram();
    void recomputeLine();
    void updateResidualPlot();
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
    MonitorPlotWidget* m_monitorPlot = nullptr;
    QLabel* m_residualStats = nullptr;
    QLabel* m_residualStatus = nullptr;
    std::vector<float> m_residualIters;
    std::vector<float> m_residualCont;
    std::vector<float> m_residualMomU;
    std::vector<float> m_residualMomV;
    std::vector<float> m_residualMomW;
    std::vector<float> m_residualK;
    std::vector<float> m_residualOmega;
    int m_residualDim = 3;
    bool m_residualHasTurb = false;
    int m_monitorHistoryWindow = 0; // 0 = all
    QStringList m_monitorAvailableSeries;
    QTimer* m_recomputeTimer = nullptr;
    int m_mode = 0;
    // cached line data for export (primary = user-selected placement;
    // secondary = the other placement when compare mode is on)
    std::vector<float> m_lineDists, m_lineVals;
    std::vector<float> m_lineVals2;
    QString m_lineLabel2;
    float m_lineMin=0,m_lineMax=1;
    QString m_lineField;
    int m_linePlacement=0;

public:
    // Pinned lines (session-scoped, not persisted): snapshots of the sampling
    // definition at pin time. Recomputed against the current mesh on every
    // refreshData so a mesh/meshDataUpdated never leaves stale curves.
    struct PinnedLineDef {
        glm::vec3 p0, p1;
        QString field;
        int placement = 0;   // 0 = Vertex, 1 = CellCenter
        int samples = 256;
        QString label;       // "L1", "L2", ...
        QString tipLabel;    // "L1 Velocity [Magnitude] [Cell]"
        bool hidden = false; // toggled by clicking the legend chip
    };
    void pinCurrentLine();
    void clearPinnedLines();

    // Sidebar management-list support: the Plots page renders a checkable
    // list (isosurface-values idiom) of pinned lines and routes edits back.
    struct PinInfo { QString label; QString tip; bool hidden; };
    std::vector<PinInfo> pinnedLineInfos() const;
    void setPinnedHiddenAt(int index, bool hidden);
    // Load a pinned line's definition back into the sidebar (P0/P1, field,
    // placement, samples); the next "Pin line" replaces it in place.
    void editPinnedLine(int index);
    int editingPin() const { return m_editingPin; }

private:
    // Samples one line into dists/vals; returns false if the field does not
    // resolve on the current mesh/snapshot. Shared by active + pinned sampling.
    bool sampleLine(const glm::vec3& p0, const glm::vec3& p1, const QString& field,
                    int placement, int N, const FieldStore::Context& ctx,
                    std::vector<float>& dists, std::vector<float>& vals, QString* err = nullptr);

    // Last rendered pinned series (cache for CSV export without resampling).
    std::vector<LineSeries> m_lastPinned;
    // Pinned line definitions (session-scoped).
    std::vector<PinnedLineDef> m_pinned;
    // Index of the pinned line loaded back into the sidebar for editing;
    // -1 when the "Pin line" button appends a new line instead.
    int m_editingPin = -1;
};
