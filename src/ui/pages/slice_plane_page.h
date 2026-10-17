#pragma once

#include "sidebar_page.h"

class QCheckBox;
class QComboBox;

namespace sidebar {

// Slice Plane section page — three per-axis planes (enable / field / position /
// opacity / colorbar). Re-gates and re-populates on meshLoadStateChanged
// (was a branch of the big gating lambda in MainWindow::connectSettings).
class SlicePlanePage : public SidebarPage {
    Q_OBJECT
public:
    explicit SlicePlanePage(RenderSettings* settings, QWidget* parent = nullptr);

private:
    QCheckBox* m_showCb = nullptr;
    QWidget* m_optionsGroup = nullptr;
    QCheckBox* m_enableX = nullptr;
    QCheckBox* m_enableY = nullptr;
    QCheckBox* m_enableZ = nullptr;
    QComboBox* m_fieldCombo[3] = {nullptr, nullptr, nullptr};
};

} // namespace sidebar
