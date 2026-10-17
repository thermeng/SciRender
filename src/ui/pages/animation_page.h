#pragma once

#include "sidebar_page.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QLabel;

namespace sidebar {

// Animation playback page. All controls live here; the page self-syncs from
// AnimationController::stateChanged. Export stays on MainWindow (file dialog +
// exporter capture plumbing), so the page re-emits exportRequested().
class AnimationPage : public SidebarPage {
    Q_OBJECT
public:
    explicit AnimationPage(RenderSettings* settings, QWidget* parent = nullptr);

signals:
    void exportRequested();

private:
    void refreshPage();

    QPushButton* m_playBtn = nullptr;
    QPushButton* m_stepBackBtn = nullptr;
    QPushButton* m_stepFwdBtn = nullptr;
    QComboBox* m_frameCombo = nullptr;
    QDoubleSpinBox* m_jumpTimeSpin = nullptr;
    QPushButton* m_jumpTimeBtn = nullptr;
    QDoubleSpinBox* m_fpsSpin = nullptr;
    QSpinBox* m_loopFromSpin = nullptr;
    QSpinBox* m_loopToSpin = nullptr;
    QCheckBox* m_loopRangeCb = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLabel* m_sequenceLabel = nullptr;
    QCheckBox* m_loopCb = nullptr;
    QComboBox* m_scaleCombo = nullptr;
    QPushButton* m_exportBtn = nullptr;
    QProgressBar* m_bufferBar = nullptr;
    int m_frameCount = -1;
};

} // namespace sidebar
