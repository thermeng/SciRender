#pragma once

#include "sidebar_page.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

namespace sidebar {

// Post Processing Fields page (ω / H / Q / ∇p extraction controls + status).
// Repopulates combo sources and refreshes status on meshDataUpdated (was the
// MainWindow::refreshPostProcPage flow).
class PostProcPage : public SidebarPage {
    Q_OBJECT
public:
    explicit PostProcPage(RenderSettings* settings, QWidget* parent = nullptr);

private:
    void refreshPage();

    QComboBox* m_velCombo = nullptr;
    QComboBox* m_presCombo = nullptr;
    QCheckBox* m_vortCb = nullptr;
    QCheckBox* m_helCb = nullptr;
    QCheckBox* m_qCb = nullptr;
    QCheckBox* m_gradPCb = nullptr;
    QCheckBox* m_autoCb = nullptr;
    QPushButton* m_extractBtn = nullptr;
    QLabel* m_statusLbl = nullptr;
};

} // namespace sidebar
