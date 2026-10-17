#pragma once

#include "sidebar_page.h"

#include <QList>
#include <QVariantList>
#include <QVariantMap>

class QCheckBox;
class QComboBox;
class TransferFunctionWidget;

namespace sidebar {

// Volume rendering page: enable + scalar field, step size / opacity, use-colormap
// toggle and the transfer-function editor. Self-subscribes: viewChanged refreshes
// the TF editor and gating; meshLoadStateChanged re-gates on data availability.
class VolumePage : public SidebarPage {
    Q_OBJECT
public:
    explicit VolumePage(RenderSettings* settings, QWidget* parent = nullptr);

private:
    void applyGating();
    void refreshVolumeTF();

    QCheckBox* m_showCb = nullptr;
    QWidget* m_optionsGroup = nullptr;
    QComboBox* m_fieldCombo = nullptr;
    QList<QWidget*> m_renderCtrls;  // greyed when volume rendering is off
    class TransferFunctionWidget* m_tfWidget = nullptr;
    QCheckBox* m_tfEnableCb = nullptr;
};

} // namespace sidebar
