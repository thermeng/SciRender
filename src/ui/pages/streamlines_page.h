#pragma once

#include "sidebar_page.h"

#include <vector>

class QCheckBox;
class QColorDialog;
class QComboBox;
class QLabel;
class QWidget;

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
    void buildContent();
    // Enables/disables the structural (recompute-triggering) controls.
    // While particles animate, replacing `paths` underneath them is
    // unsafe, so structural changes are gated off; render-only controls
    // (particle speed/size, opacity, colours) stay live.
    void updateAnimationGate();

    QComboBox* m_streamlineCombo = nullptr;
    QComboBox* m_streamlineDirectionCombo = nullptr;
    QCheckBox* m_showCb = nullptr;
    QLabel* m_animBanner = nullptr;
    std::vector<QWidget*> m_structuralControls;

    QColorDialog* m_seedColorDialog = nullptr;
    QColorDialog* m_streamlineColorDialog = nullptr;
};

} // namespace sidebar
