#include "slice_plane_page.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLayout>
#include <QLabel>
#include <QSlider>
#include <QVBoxLayout>

#include "ui_slice_plane_page.h"
#include "render/settings/render_settings.h"
#include "ui/panel_helpers.h"

namespace sidebar {

SlicePlanePage::SlicePlanePage(RenderSettings* settings, QWidget* parent)
    : SidebarPage(settings, parent) {
    auto* content = new QWidget;
    Ui::SlicePlanePage sliceUi;
    sliceUi.setupUi(content);
    sb::fixLayoutOverflow(content);

    auto* s = S();

    // -- Master enable (toggles all options group) --
    auto* showCb = sliceUi.showSliceCb;
    m_showCb = showCb;
    m_optionsGroup = sliceUi.optionsGroup;
    showCb->setEnabled(s->hasVolumeData());
    if (!s->hasVolumeData()) showCb->setChecked(false);
    connect(showCb, &QCheckBox::toggled, sliceUi.optionsGroup, &QWidget::setEnabled);
    sliceUi.optionsGroup->setEnabled(showCb->isChecked() && showCb->isEnabled());

    // -- Per-plane setup helper --
    auto setupPlane = [&](QCheckBox* enableCb, QComboBox* fieldCombo, QSlider* posSlider, QLabel* posValue,
                          QSlider* opacitySlider, QLabel* opacityValue, QCheckBox* colorbarCb,
                          int axis, bool enabled, float pos, float opacity, bool showColorbar,
                          auto setEnabled, auto setPos, auto setOpacity, auto setShowColorbar) {
        enableCb->setChecked(enabled);
        connect(enableCb, &QCheckBox::toggled, s, setEnabled);

        fieldCombo->addItems(s->getAvailableScalars());
        fieldCombo->setCurrentText(s->getActiveScalarNameQml());
        fieldCombo->setEnabled(s->hasMeshScalars());
        connect(fieldCombo, &QComboBox::activated, this, [this, fieldCombo, axis, s](int idx) {
            fieldCombo->setCurrentIndex(idx);
            s->setSlicePlaneField(axis, fieldCombo->itemText(idx));
        });

        posSlider->setRange(0, 1000);
        posSlider->setValue(static_cast<int>(pos * 1000));
        connect(posSlider, &QSlider::valueChanged, this, [posValue, setPos, s](int raw) {
            double v = raw / 1000.0;
            posValue->setText(QString::number(v * 100.0, 'f', 0) + "%");
            setPos(v);
        });

        opacitySlider->setRange(0, 1000);
        opacitySlider->setValue(static_cast<int>(opacity * 1000));
        connect(opacitySlider, &QSlider::valueChanged, this, [opacityValue, setOpacity, s](int raw) {
            double v = raw / 1000.0;
            opacityValue->setText(QString::number(v * 100.0, 'f', 0) + "%");
            setOpacity(v);
        });

        colorbarCb->setChecked(showColorbar);
        connect(colorbarCb, &QCheckBox::toggled, s, setShowColorbar);
    };

    m_fieldCombo[0] = new QComboBox;
    m_fieldCombo[1] = new QComboBox;
    m_fieldCombo[2] = new QComboBox;

    setupPlane(sliceUi.enableX, m_fieldCombo[0], sliceUi.posXSlider, sliceUi.xPosValue,
               sliceUi.opacityXSlider, sliceUi.xOpacityValue, sliceUi.colorbarX,
               0, s->getSlicePlaneEnabledX(), s->getSlicePlanePosX(),
               s->getSlicePlaneOpacityX(), s->getSlicePlaneShowColorbarX(),
               &RenderSettings::setSlicePlaneEnabledX,
               [s](double v) { s->setSlicePlanePosX(v); },
               [s](double v) { s->setSlicePlaneOpacityX(v); },
               &RenderSettings::setSlicePlaneShowColorbarX);

    setupPlane(sliceUi.enableY, m_fieldCombo[1], sliceUi.posYSlider, sliceUi.yPosValue,
               sliceUi.opacityYSlider, sliceUi.yOpacityValue, sliceUi.colorbarY,
               1, s->getSlicePlaneEnabledY(), s->getSlicePlanePosY(),
               s->getSlicePlaneOpacityY(), s->getSlicePlaneShowColorbarY(),
               &RenderSettings::setSlicePlaneEnabledY,
               [s](double v) { s->setSlicePlanePosY(v); },
               [s](double v) { s->setSlicePlaneOpacityY(v); },
               &RenderSettings::setSlicePlaneShowColorbarY);

    setupPlane(sliceUi.enableZ, m_fieldCombo[2], sliceUi.posZSlider, sliceUi.zPosValue,
               sliceUi.opacityZSlider, sliceUi.zOpacityValue, sliceUi.colorbarZ,
               2, s->getSlicePlaneEnabledZ(), s->getSlicePlanePosZ(),
               s->getSlicePlaneOpacityZ(), s->getSlicePlaneShowColorbarZ(),
               &RenderSettings::setSlicePlaneEnabledZ,
               [s](double v) { s->setSlicePlanePosZ(v); },
               [s](double v) { s->setSlicePlaneOpacityZ(v); },
               &RenderSettings::setSlicePlaneShowColorbarZ);

    m_enableX = sliceUi.enableX;
    m_enableY = sliceUi.enableY;
    m_enableZ = sliceUi.enableZ;

    // Insert per-axis field combo boxes below each enable checkbox.
    auto insertFieldCombo = [](QWidget* group, QComboBox* combo) {
        if (auto* lay = qobject_cast<QVBoxLayout*>(group->layout()))
            lay->insertWidget(1, combo);
    };
    insertFieldCombo(sliceUi.xGroup, m_fieldCombo[0]);
    insertFieldCombo(sliceUi.yGroup, m_fieldCombo[1]);
    insertFieldCombo(sliceUi.zGroup, m_fieldCombo[2]);
    // Slice palette + fixed range live in the Colorbar Style dialog.

    qobject_cast<QVBoxLayout*>(content->layout())->addStretch();

    // Gating + combo re-population on mesh (re)load — was in the big
    // meshLoadStateChanged lambda of MainWindow::connectSettings. The dead
    // m_volumeSliceShowCb branches there (never assigned, always null) are not
    // reproduced.
    connect(s, &RenderSettings::meshLoadStateChanged, this, [this, s]() {
        if (m_showCb) {
            bool ok = s->hasVolumeData();
            m_showCb->setEnabled(ok);
            m_showCb->blockSignals(true);
            m_showCb->setChecked(false);
            m_showCb->blockSignals(false);
            if (m_optionsGroup) m_optionsGroup->setEnabled(false);
            if (m_enableX) m_enableX->setChecked(false);
            if (m_enableY) m_enableY->setChecked(false);
            if (m_enableZ) m_enableZ->setChecked(false);
        }
        for (int axis = 0; axis < 3; ++axis) {
            if (!m_fieldCombo[axis]) continue;
            m_fieldCombo[axis]->blockSignals(true);
            m_fieldCombo[axis]->clear();
            m_fieldCombo[axis]->addItems(s->getAvailableScalars());
            QString field = s->getSlicePlaneField(axis);
            if (field.isEmpty()) field = s->getActiveScalarNameQml();
            m_fieldCombo[axis]->setCurrentText(field);
            m_fieldCombo[axis]->setEnabled(s->hasVolumeData());
            m_fieldCombo[axis]->blockSignals(false);
        }
    });

    setContent(content);
}

} // namespace sidebar
