#pragma once

#include "sidebar_page.h"

#include <QColor>
#include <QList>
#include <QVariant>
#include <QVariantList>

class QCheckBox;
class QColorDialog;
class QComboBox;
class QDoubleSpinBox;
class QListWidget;
class QPushButton;

namespace sidebar {

// Isosurface (contour) page. Independent of the Volume page: the former
// syncVolumePage fan-out is split here so this page self-subscribes to
// viewChanged / meshLoadStateChanged / meshDataUpdated and refreshes its own
// widgets (the plan's IsoSurfacePage resolution).
class IsosurfacePage : public SidebarPage {
    Q_OBJECT
public:
    explicit IsosurfacePage(RenderSettings* settings, QWidget* parent = nullptr);

private:
    void refreshIsosurfaceValue();
    void refreshIsosurfaceList();

    QCheckBox* m_enableCb = nullptr;
    QComboBox* m_fieldCombo = nullptr;
    QDoubleSpinBox* m_valueSpin = nullptr;
    QListWidget* m_list = nullptr;
    QPushButton* m_removeBtn = nullptr;
    QColorDialog* m_colorDialog = nullptr;
};

} // namespace sidebar
