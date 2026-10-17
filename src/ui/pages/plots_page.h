#pragma once

#include "sidebar_page.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QToolButton;
class PlotWindow;

namespace sidebar {

// Plots page (mode tiles, per-mode field/placement selectors, P0/P1 endpoints,
// export buttons). PlotWindow is injected: the page drives the canvas directly
// (setMode, scheduleRecompute, exports) but dock visibility stays on MainWindow,
// which relays visibility ticks via setViewportVisible() and connects
// viewportToggleRequested → setHistogramVisible.
class PlotsPage : public SidebarPage {
    Q_OBJECT
public:
    explicit PlotsPage(RenderSettings* settings, PlotWindow* plotWindow,
                       bool viewportVisible, QWidget* parent = nullptr);

public slots:
    void setViewportVisible(bool visible);

signals:
    void viewportToggleRequested(bool visible);

private:
    void refreshFieldCombos();
    void updatePlacementCombo(QComboBox* combo, bool lineMode);
    void initLineProbeFromMeshBounds();
    void rebuildPinList();
    void syncFromSettings();
    void applyViewportState();

    PlotWindow* m_plotWindow = nullptr;
    QPushButton* m_openBtn = nullptr;
    QToolButton* m_modeHistBtn = nullptr;
    QToolButton* m_modeLineBtn = nullptr;
    QStackedWidget* m_paramStack = nullptr;
    QComboBox* m_histFieldCombo = nullptr;
    QComboBox* m_histPlacementCombo = nullptr;
    QSpinBox* m_histBinsSpin = nullptr;
    QComboBox* m_lineFieldCombo = nullptr;
    QComboBox* m_linePlacementCombo = nullptr;
    QSpinBox* m_samplesSpin = nullptr;
    QDoubleSpinBox* m_p0x = nullptr;
    QDoubleSpinBox* m_p0y = nullptr;
    QDoubleSpinBox* m_p0z = nullptr;
    QDoubleSpinBox* m_p1x = nullptr;
    QDoubleSpinBox* m_p1y = nullptr;
    QDoubleSpinBox* m_p1z = nullptr;
    QCheckBox* m_showLineCb = nullptr;
    QCheckBox* m_compareCb = nullptr;
    QWidget* m_pinGroup = nullptr;      // "Pinned Lines" block, hidden while empty
    QListWidget* m_pinList = nullptr;   // checkable rows: check=visible, edit/remove buttons below
    bool m_lineAutoSeeded = true;       // mesh-load re-seeds P0/P1 to bbox until the user moves them
};

} // namespace sidebar
