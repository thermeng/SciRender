#pragma once

#include "sidebar_page.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QShowEvent;
class QTableWidget;
class QTimer;

namespace sidebar {

// Probe inspector page: world-position spinboxes + per-scalar value table
// sampled through the unified FieldStore/FieldSampler path. Self-subscribes to
// viewChanged (option sync) and meshDataUpdated (bounds, steps, table refresh).
class ProbePage : public SidebarPage {
    Q_OBJECT
public:
    explicit ProbePage(RenderSettings* settings, QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void refreshProbeTable();
    void scheduleProbeRefresh();
    void applyProbeSteps();

    QCheckBox* m_showCb = nullptr;
    QDoubleSpinBox* m_xSpin = nullptr;
    QDoubleSpinBox* m_ySpin = nullptr;
    QDoubleSpinBox* m_zSpin = nullptr;
    QComboBox* m_formatCombo = nullptr;
    QComboBox* m_placementCombo = nullptr;
    QTableWidget* m_table = nullptr;
    QTimer* m_refreshTimer = nullptr;
    bool m_probeDirty = false;
};

} // namespace sidebar
