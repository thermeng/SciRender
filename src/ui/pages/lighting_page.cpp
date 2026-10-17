#include "lighting_page.h"

#include <QCheckBox>
#include <QLabel>
#include <QLayout>
#include <QSlider>
#include <QVBoxLayout>

#include "ui_lighting_page.h"
#include "render/settings/render_settings.h"
#include "ui/panel_helpers.h"

namespace sidebar {

LightingPage::LightingPage(RenderSettings* settings, QWidget* parent)
    : SidebarPage(settings, parent) {
    auto* content = new QWidget;
    Ui::LightingPage lightingUi;
    lightingUi.setupUi(content);
    sb::fixLayoutOverflow(content);

    auto* s = S();
    connect(lightingUi.resetBtn, &QPushButton::clicked, s, &RenderSettings::resetLighting);

    auto* markersCb = lightingUi.markersCb;
    markersCb->setChecked(s->getShowLightMarkers());
    connect(markersCb, &QCheckBox::toggled, s, &RenderSettings::setShowLightMarkers);
    m_markersCb = markersCb;

    auto* kitCb = lightingUi.kitCb;
    kitCb->setChecked(s->getLightKitEnabled());
    connect(kitCb, &QCheckBox::toggled, s, &RenderSettings::setLightKitEnabled);
    m_kitCb = kitCb;

    {
        auto* slider = lightingUi.keyLightSlider;
        auto* valueLabel = lightingUi.keyLightValue;
        m_keySlider = slider;
        m_keyValue = valueLabel;
        slider->setRange(0, 1000);
        slider->setValue(static_cast<int>(s->getLightKeyIntensity() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, s](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 1));
            s->setLightKeyIntensity(v);
        });
    }
    {
        auto* slider = lightingUi.warmthSlider;
        auto* valueLabel = lightingUi.warmthValue;
        m_warmthSlider = slider;
        m_warmthValue = valueLabel;
        slider->setRange(0, 1000);
        slider->setValue(static_cast<int>(s->getLightWarm() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, s](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 1));
            s->setLightWarm(v);
        });
    }
    {
        auto* slider = lightingUi.fillKfSlider;
        auto* valueLabel = lightingUi.fillKFValue;
        m_fillKfSlider = slider;
        m_fillKfValue = valueLabel;
        slider->setRange(1000, 15000);
        slider->setValue(static_cast<int>(s->getLightKF() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, s](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 1));
            s->setLightKF(v);
        });
    }
    {
        auto* slider = lightingUi.backKbSlider;
        auto* valueLabel = lightingUi.backKbValue;
        m_backKbSlider = slider;
        m_backKbValue = valueLabel;
        slider->setRange(1000, 15000);
        slider->setValue(static_cast<int>(s->getLightKB() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, s](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 1));
            s->setLightKB(v);
        });
    }
    {
        auto* slider = lightingUi.headKhSlider;
        auto* valueLabel = lightingUi.headKHValue;
        m_headKhSlider = slider;
        m_headKhValue = valueLabel;
        slider->setRange(1000, 15000);
        slider->setValue(static_cast<int>(s->getLightKH() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, s](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 1));
            s->setLightKH(v);
        });
    }

    auto* tabWidget = lightingUi.directionTabs;

    const char* lightNames[] = {"Key", "Fill", "Back", "Head"};
    using GetterFn = float (RenderSettings::*)() const;
    GetterFn azGetters[] = { &RenderSettings::getLightKeyAzimuth, &RenderSettings::getLightFillAzimuth, &RenderSettings::getLightBackAzimuth, &RenderSettings::getLightHeadAzimuth };
    GetterFn elGetters[] = { &RenderSettings::getLightKeyElevation, &RenderSettings::getLightFillElevation, &RenderSettings::getLightBackElevation, &RenderSettings::getLightHeadElevation };
    void (RenderSettings::*azSetters[])(float) = { &RenderSettings::setLightKeyAzimuth, &RenderSettings::setLightFillAzimuth, &RenderSettings::setLightBackAzimuth, &RenderSettings::setLightHeadAzimuth };
    void (RenderSettings::*elSetters[])(float) = { &RenderSettings::setLightKeyElevation, &RenderSettings::setLightFillElevation, &RenderSettings::setLightBackElevation, &RenderSettings::setLightHeadElevation };

    m_dirTabs.resize(4);
    for (int i = 0; i < 4; ++i) {
        auto* tab = new QWidget;
        auto* tabLayout = new QVBoxLayout(tab);
        tabLayout->setContentsMargins(4, 4, 4, 4);
        {
            auto row = sb::createLightSlider("Azimuth", (s->*azGetters[i])(), -180, 180, 1, 0,
                [s, i, azSetters](double v) { (s->*azSetters[i])(v); });
            m_dirTabs[i].azimuthSlider = row.slider;
            m_dirTabs[i].azimuthValue = row.valueLabel;
            tabLayout->addWidget(row.slider->parentWidget());
        }
        {
            auto row = sb::createLightSlider("Elevation", (s->*elGetters[i])(), -90, 90, 1, 0,
                [s, i, elSetters](double v) { (s->*elSetters[i])(v); });
            m_dirTabs[i].elevationSlider = row.slider;
            m_dirTabs[i].elevationValue = row.valueLabel;
            tabLayout->addWidget(row.slider->parentWidget());
        }
        tabLayout->addStretch();
        tabWidget->addTab(tab, QString::fromUtf8(lightNames[i]));
    }
    m_directionTabs = tabWidget;

    {
        auto* slider = lightingUi.ambientSlider;
        auto* valueLabel = lightingUi.ambientValue;
        m_ambientSlider = slider;
        m_ambientValue = valueLabel;
        slider->setRange(0, 1000);
        slider->setValue(static_cast<int>(s->getMatAmbient() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, s](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 1));
            s->setMatAmbient(v);
        });
    }
    {
        auto* slider = lightingUi.diffuseSlider;
        auto* valueLabel = lightingUi.diffuseValue;
        m_diffuseSlider = slider;
        m_diffuseValue = valueLabel;
        slider->setRange(0, 1000);
        slider->setValue(static_cast<int>(s->getMatDiffuse() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, s](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 1));
            s->setMatDiffuse(v);
        });
    }
    {
        auto* slider = lightingUi.specularSlider;
        auto* valueLabel = lightingUi.specularValue;
        m_specularSlider = slider;
        m_specularValue = valueLabel;
        slider->setRange(0, 1000);
        slider->setValue(static_cast<int>(s->getMatSpecular() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, s](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 1));
            s->setMatSpecular(v);
        });
    }
    {
        auto* slider = lightingUi.roughnessSlider;
        auto* valueLabel = lightingUi.roughnessValue;
        m_roughnessSlider = slider;
        m_roughnessValue = valueLabel;
        slider->setRange(0, 1000);
        slider->setValue(static_cast<int>(s->getMatRoughness() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, s](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 2));
            s->setMatRoughness(v);
        });
    }
    {
        auto* slider = lightingUi.metallicSlider;
        auto* valueLabel = lightingUi.metallicValue;
        m_metallicSlider = slider;
        m_metallicValue = valueLabel;
        slider->setRange(0, 1000);
        slider->setValue(static_cast<int>(s->getMatMetallic() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, s](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 2));
            s->setMatMetallic(v);
        });
    }

    qobject_cast<QVBoxLayout*>(content->layout())->addStretch();

    // Mirror backend lighting updates (menu toggles, scripted resets) into the
    // page widgets; setChecked/setValue only emit when the value differs.
    connect(s, &RenderSettings::viewChanged, this, [this](ChangeFlags flags) {
        if (flags.testFlag(ChangeFlag::Lighting)) syncPage();
    });

    setContent(content);
}

void LightingPage::syncPage() {
    auto* s = S();
    if (m_markersCb) m_markersCb->setChecked(s->getShowLightMarkers());
    if (m_kitCb) m_kitCb->setChecked(s->getLightKitEnabled());

    auto syncSlider = [s](QSlider* slider, QLabel* label, double value, int decimals) {
        if (!slider) return;
        slider->blockSignals(true);
        slider->setValue(static_cast<int>(value * 1000));
        slider->blockSignals(false);
        if (label) label->setText(QString::number(value, 'f', decimals));
    };

    syncSlider(m_keySlider, m_keyValue, s->getLightKeyIntensity(), 1);
    syncSlider(m_warmthSlider, m_warmthValue, s->getLightWarm(), 1);
    syncSlider(m_fillKfSlider, m_fillKfValue, s->getLightKF(), 1);
    syncSlider(m_backKbSlider, m_backKbValue, s->getLightKB(), 1);
    syncSlider(m_headKhSlider, m_headKhValue, s->getLightKH(), 1);

    using GetterFn = float (RenderSettings::*)() const;
    static const GetterFn azGetters[] = { &RenderSettings::getLightKeyAzimuth, &RenderSettings::getLightFillAzimuth, &RenderSettings::getLightBackAzimuth, &RenderSettings::getLightHeadAzimuth };
    static const GetterFn elGetters[] = { &RenderSettings::getLightKeyElevation, &RenderSettings::getLightFillElevation, &RenderSettings::getLightBackElevation, &RenderSettings::getLightHeadElevation };

    for (int i = 0; i < 4 && i < m_dirTabs.size(); ++i) {
        const auto& tab = m_dirTabs[i];
        syncSlider(tab.azimuthSlider, tab.azimuthValue, (s->*azGetters[i])(), 0);
        syncSlider(tab.elevationSlider, tab.elevationValue, (s->*elGetters[i])(), 0);
    }

    syncSlider(m_ambientSlider, m_ambientValue, s->getMatAmbient(), 1);
    syncSlider(m_diffuseSlider, m_diffuseValue, s->getMatDiffuse(), 1);
    syncSlider(m_specularSlider, m_specularValue, s->getMatSpecular(), 1);
    syncSlider(m_roughnessSlider, m_roughnessValue, s->getMatRoughness(), 2);
    syncSlider(m_metallicSlider, m_metallicValue, s->getMatMetallic(), 2);
}

} // namespace sidebar
