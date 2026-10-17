#pragma once

#include "sidebar_page.h"

class QCheckBox;
class QColorDialog;
class QComboBox;

namespace sidebar {

// Streamlines section page (flow / seed / look tabs + particle animation).
class StreamlinesPage : public SidebarPage {
    Q_OBJECT
public:
    explicit StreamlinesPage(RenderSettings* settings, QWidget* parent = nullptr);

signals:
    // Emitted after reintegration is marked dirty on the backend; MainWindow
    // wakes the viewport. Keeps the page free of ViewportWidget knowledge.
    void viewportUpdateRequested();

private:
    QComboBox* m_streamlineCombo = nullptr;
    QComboBox* m_streamlineDirectionCombo = nullptr;
    QCheckBox* m_showCb = nullptr;

    QColorDialog* m_seedColorDialog = nullptr;
    QColorDialog* m_streamlineColorDialog = nullptr;
};

} // namespace sidebar
