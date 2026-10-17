#pragma once

#include "sidebar_page.h"

class QCheckBox;
class QLabel;
class QSlider;

namespace sidebar {

// Clipping section page. Refreshes its slider bounds from the dataset on
// meshLoadStateChanged (the old MainWindow::refreshClippingPageBounds).
class ClippingPage : public SidebarPage {
    Q_OBJECT
public:
    explicit ClippingPage(RenderSettings* settings, QWidget* parent = nullptr);

private:
    void refreshPageBounds();

    QSlider* m_xSlider = nullptr;
    QLabel* m_xValue = nullptr;
    QSlider* m_ySlider = nullptr;
    QLabel* m_yValue = nullptr;
    QSlider* m_zSlider = nullptr;
    QLabel* m_zValue = nullptr;
    QCheckBox* m_enableCb = nullptr;
    QCheckBox* m_crinkleCb = nullptr;
    QCheckBox* m_axisXCb = nullptr;
    QCheckBox* m_axisYCb = nullptr;
    QCheckBox* m_axisZCb = nullptr;
};

} // namespace sidebar
