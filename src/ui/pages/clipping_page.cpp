#include "clipping_page.h"

#include <QCheckBox>
#include <QLabel>
#include <QLayout>
#include <QSlider>
#include <QVBoxLayout>

#include "ui_clipping_page.h"
#include "render/settings/render_settings.h"
#include "ui/panel_helpers.h"

namespace sidebar {

ClippingPage::ClippingPage(RenderSettings* settings, QWidget* parent)
    : SidebarPage(settings, parent) {
    auto* page = new QWidget;
    Ui::ClippingPage clippingUi;
    clippingUi.setupUi(page);
    sb::fixLayoutOverflow(page);

    auto* s = S();

    // -- Master enable --------------------------------------------------------
    auto* enableCb = clippingUi.enableCb;
    m_enableCb = enableCb;
    enableCb->setChecked(s->getClipEnabled());
    connect(enableCb, &QCheckBox::toggled, s, &RenderSettings::setClipEnabled);
    connect(enableCb, &QCheckBox::toggled, clippingUi.optionsGroup, &QWidget::setEnabled);
    clippingUi.optionsGroup->setEnabled(s->getClipEnabled());

    // -- Crinkle Clip checkbox -----------------------------------------------
    m_crinkleCb = clippingUi.crinkleCb;
    m_crinkleCb->setEnabled(s->getClipEnabled());
    m_crinkleCb->setChecked(s->getCrinkleClipMode());
    connect(enableCb, &QCheckBox::toggled, m_crinkleCb, &QWidget::setEnabled);
    connect(m_crinkleCb, &QCheckBox::toggled, s, &RenderSettings::setCrinkleClipMode);

    // -- Per-axis setup lambda -----------------------------------------------
    auto setupAxis = [&](QCheckBox* axisCb, QCheckBox* invertCb,
                         QSlider* slider, QLabel* valueLabel,
                         bool enabled, bool invert, double from, double to, double val,
                         auto enableSlot, auto setter, auto invertSlot) {
        axisCb->setChecked(enabled);
        connect(axisCb, &QCheckBox::toggled, s, enableSlot);

        invertCb->setChecked(invert);
        connect(invertCb, &QCheckBox::toggled, s, invertSlot);

        int minI = static_cast<int>(from * 1000);
        int maxI = static_cast<int>(to * 1000);
        slider->setRange(minI, maxI);
        slider->setValue(static_cast<int>(val * 1000));

        valueLabel->setText(QString::number(val, 'f', 3));

        // Slider -> valueLabel + setter
        connect(slider, &QSlider::valueChanged, this,
            [valueLabel, setter, s](int raw) {
                double v = raw / 1000.0;
                valueLabel->setText(QString::number(v, 'f', 3));
                setter(v);
            });
    };

    // -- X Axis ---------------------------------------------------------------
    m_xSlider = clippingUi.sliceXSlider;
    m_xValue = clippingUi.xValue;
    m_axisXCb = clippingUi.axisX;
    setupAxis(clippingUi.axisX, clippingUi.invX,
              m_xSlider, m_xValue,
              s->getClipEnabledX(), s->getInvertX(),
              s->getWorldMinX(), s->getWorldMaxX(), s->getClipX(),
              &RenderSettings::setClipEnabledX,
              [s](double v) { s->setClipX(v); },
              &RenderSettings::setInvertX);

    // -- Y Axis ---------------------------------------------------------------
    m_ySlider = clippingUi.sliceYSlider;
    m_yValue = clippingUi.yValue;
    m_axisYCb = clippingUi.axisY;
    setupAxis(clippingUi.axisY, clippingUi.invY,
              m_ySlider, m_yValue,
              s->getClipEnabledY(), s->getInvertY(),
              s->getWorldMinY(), s->getWorldMaxY(), s->getClipY(),
              &RenderSettings::setClipEnabledY,
              [s](double v) { s->setClipY(v); },
              &RenderSettings::setInvertY);

    // -- Z Axis ---------------------------------------------------------------
    m_zSlider = clippingUi.sliceZSlider;
    m_zValue = clippingUi.zValue;
    m_axisZCb = clippingUi.axisZ;
    setupAxis(clippingUi.axisZ, clippingUi.invZ,
              m_zSlider, m_zValue,
              s->getClipEnabledZ(), s->getInvertZ(),
              s->getWorldMinZ(), s->getWorldMaxZ(), s->getClipZ(),
              &RenderSettings::setClipEnabledZ,
              [s](double v) { s->setClipZ(v); },
              &RenderSettings::setInvertZ);

    qobject_cast<QVBoxLayout*>(page->layout())->addStretch();

    connect(s, &RenderSettings::meshLoadStateChanged, this, &ClippingPage::refreshPageBounds);

    setContent(page);
}

// New mesh: slider bounds follow the world bbox and the plane re-centers,
// matching the inherited behaviour of old refreshClippingPageBounds.
void ClippingPage::refreshPageBounds() {
    auto* s = S();
    if (m_enableCb) m_enableCb->setChecked(s->getClipEnabled());
    if (m_crinkleCb) {
        m_crinkleCb->setEnabled(s->getClipEnabled());
        m_crinkleCb->setChecked(s->getCrinkleClipMode());
    }
    if (m_axisXCb)  m_axisXCb->setChecked(s->getClipEnabledX());
    if (m_axisYCb)  m_axisYCb->setChecked(s->getClipEnabledY());
    if (m_axisZCb)  m_axisZCb->setChecked(s->getClipEnabledZ());

    auto updateSlider = [](QSlider* slider, QLabel* valueLabel, double from, double to, double val) {
        if (!slider || !valueLabel) return;
        int minI = static_cast<int>(from * 1000);
        int maxI = static_cast<int>(to * 1000);
        slider->setRange(minI, maxI);
        double center = (from + to) * 0.5;
        slider->blockSignals(true);
        slider->setValue(static_cast<int>(center * 1000));
        slider->blockSignals(false);
        valueLabel->setText(QString::number(center, 'f', 3));
    };
    updateSlider(m_xSlider, m_xValue, s->getWorldMinX(), s->getWorldMaxX(), s->getClipX());
    updateSlider(m_ySlider, m_yValue, s->getWorldMinY(), s->getWorldMaxY(), s->getClipY());
    updateSlider(m_zSlider, m_zValue, s->getWorldMinZ(), s->getWorldMaxZ(), s->getClipZ());
}

} // namespace sidebar
