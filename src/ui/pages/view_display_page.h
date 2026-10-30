#pragma once

#include "sidebar_page.h"

class QColorDialog;
class QCheckBox;
class QComboBox;

namespace sidebar {

// View & Display section page. Syncs itself from RenderSettings::viewChanged so
// toolbar toggles and keyboard shortcuts stay mirrored here.
class ViewDisplayPage : public SidebarPage {
    Q_OBJECT
public:
    explicit ViewDisplayPage(RenderSettings* settings, QWidget* parent = nullptr);

private:
    void syncPage();

    QCheckBox* m_wireframeCb = nullptr;
    QCheckBox* m_surfaceCb = nullptr;
    QCheckBox* m_pointsCb = nullptr;
    QCheckBox* m_bboxCb = nullptr;
    QCheckBox* m_defectsCb = nullptr;
    QCheckBox* m_scalarCb = nullptr;
    QCheckBox* m_gizmoCb = nullptr;
    QCheckBox* m_gizmoShowNegativeCb = nullptr;
    QComboBox* m_gizmoSizeCombo = nullptr;
    QComboBox* m_gizmoCornerCombo = nullptr;
    QCheckBox* m_fpsCb = nullptr;
    QCheckBox* m_parallelCb = nullptr;

    QColorDialog* m_meshColorDialog = nullptr;
    QColorDialog* m_surfaceColorDialog = nullptr;
    QColorDialog* m_bgColorDialog = nullptr;
    QColorDialog* m_textColorDialog = nullptr;
};

} // namespace sidebar
