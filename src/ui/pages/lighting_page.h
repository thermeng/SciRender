#pragma once

#include <QTabWidget>
#include <QVector>
#include "sidebar_page.h"

class QCheckBox;
class QLabel;
class QSlider;

namespace sidebar {

// Lighting section page. Fully self-wiring: constructor reads the initial
// values from RenderSettings, connects every control, and mirrors backend
// lighting changes back into the widgets on ChangeFlag::Lighting.
class LightingPage : public SidebarPage {
    Q_OBJECT
public:
    explicit LightingPage(RenderSettings* settings, QWidget* parent = nullptr);

private:
    void syncPage();

    struct LightDirTabData {
        QSlider* azimuthSlider = nullptr;
        QLabel* azimuthValue = nullptr;
        QSlider* elevationSlider = nullptr;
        QLabel* elevationValue = nullptr;
    };

    QCheckBox* m_markersCb = nullptr;
    QCheckBox* m_kitCb = nullptr;
    QSlider* m_keySlider = nullptr;
    QLabel* m_keyValue = nullptr;
    QSlider* m_warmthSlider = nullptr;
    QLabel* m_warmthValue = nullptr;
    QSlider* m_fillKfSlider = nullptr;
    QLabel* m_fillKfValue = nullptr;
    QSlider* m_backKbSlider = nullptr;
    QLabel* m_backKbValue = nullptr;
    QSlider* m_headKhSlider = nullptr;
    QLabel* m_headKhValue = nullptr;
    QTabWidget* m_directionTabs = nullptr;
    QVector<LightDirTabData> m_dirTabs;
    QSlider* m_ambientSlider = nullptr;
    QLabel* m_ambientValue = nullptr;
    QSlider* m_diffuseSlider = nullptr;
    QLabel* m_diffuseValue = nullptr;
    QSlider* m_specularSlider = nullptr;
    QLabel* m_specularValue = nullptr;
    QSlider* m_roughnessSlider = nullptr;
    QLabel* m_roughnessValue = nullptr;
    QSlider* m_metallicSlider = nullptr;
    QLabel* m_metallicValue = nullptr;
};

} // namespace sidebar
