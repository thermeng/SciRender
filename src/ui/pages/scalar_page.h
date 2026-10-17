#pragma once

#include "sidebar_page.h"

class QComboBox;
class QCheckBox;
class QLineEdit;
class QSlider;

namespace sidebar {

// Scalar (colormap) section page. Owns the filter sliders and the scalar
// field/placement combos; repopulates them on meshDataUpdated and re-reads
// the filter range on meshLoadStateChanged (old refreshScalarFilterRange).
class ScalarPage : public SidebarPage {
    Q_OBJECT
public:
    explicit ScalarPage(RenderSettings* settings, QWidget* parent = nullptr);

private:
    void refreshPageFields();
    void refreshFilterRange();

    QComboBox* m_scalarCombo = nullptr;
    QComboBox* m_placementCombo = nullptr;
    QCheckBox* m_showCb = nullptr;
    QWidget* m_optionsGroup = nullptr;

    // Filter sliders (fixed colormap range lives in the Colorbar Style dialog)
    QSlider* m_filterMinSlider = nullptr;
    QLineEdit* m_filterMinField = nullptr;
    QSlider* m_filterMaxSlider = nullptr;
    QLineEdit* m_filterMaxField = nullptr;
    QCheckBox* m_filterEnabledCb = nullptr;
};

} // namespace sidebar
