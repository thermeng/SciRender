#pragma once

#include "sidebar_page.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QSlider;

namespace sidebar {

// Slice Plane section page — three per-axis planes (enable / field / position /
// opacity / colorbar). Re-gates and re-populates on meshLoadStateChanged
// (was a branch of the big gating lambda in MainWindow::connectSettings).
class SlicePlanePage : public SidebarPage {
    Q_OBJECT
public:
    explicit SlicePlanePage(RenderSettings* settings, QWidget* parent = nullptr);

private:
    void syncFromSettings();
    void refreshFields();

    QCheckBox* m_showCb = nullptr;
    QWidget* m_optionsGroup = nullptr;
    QCheckBox* m_enableX = nullptr;
    QCheckBox* m_enableY = nullptr;
    QCheckBox* m_enableZ = nullptr;
    QComboBox* m_fieldCombo[3] = {nullptr, nullptr, nullptr};
    QSlider* m_posSlider[3] = {nullptr, nullptr, nullptr};
    QSlider* m_opacitySlider[3] = {nullptr, nullptr, nullptr};
    QLabel* m_posValue[3] = {nullptr, nullptr, nullptr};
    QLabel* m_opacityValue[3] = {nullptr, nullptr, nullptr};
    // "Show Slice Planes" is a gate over the three per-axis planes rather than
    // a fourth piece of persisted state, so it needs its own flag: the axes can
    // all be off while the user is still configuring the group.
    bool m_masterOn = false;
};

} // namespace sidebar
