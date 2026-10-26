#include "slice_plane_page.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLayout>
#include <QLabel>
#include <QSignalBlocker>
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
    m_masterOn = s->getSlicePlaneEnabledX() || s->getSlicePlaneEnabledY()
                 || s->getSlicePlaneEnabledZ();
    showCb->setEnabled(s->hasVolumeData());
    showCb->setChecked(s->hasVolumeData() && m_masterOn);
    connect(showCb, &QCheckBox::toggled, this, [this, s, showCb](bool on) {
        m_masterOn = on;
        if (!on) {
            // Master off hides every plane, and writing the setters (rather
            // than just unchecking with blocked signals) keeps the stored
            // state in step with the UI.
            s->setSlicePlaneEnabledX(false);
            s->setSlicePlaneEnabledY(false);
            s->setSlicePlaneEnabledZ(false);
            for (QCheckBox* cb : {m_enableX, m_enableY, m_enableZ}) {
                if (!cb) continue;
                QSignalBlocker block(cb);
                cb->setChecked(false);
            }
        } else if (!s->getSlicePlaneEnabledX() && !s->getSlicePlaneEnabledY()
                   && !s->getSlicePlaneEnabledZ()) {
            // Ticking the master with no axis selected used to just unlock an
            // empty group: the viewport stayed blank with no hint why. Turn on
            // the X plane so the toggle always produces a visible plane; the
            // user can swap to Y/Z afterwards.
            s->setSlicePlaneEnabledX(true);
            if (m_enableX) {
                QSignalBlocker block(m_enableX);
                m_enableX->setChecked(true);
            }
        }
        if (m_optionsGroup) m_optionsGroup->setEnabled(on && showCb->isEnabled());
    });
    sliceUi.optionsGroup->setEnabled(showCb->isChecked() && showCb->isEnabled());

    // -- Per-plane setup helper --
    auto setupPlane = [&](QCheckBox* enableCb, QComboBox* fieldCombo, QSlider* posSlider, QLabel* posValue,
                           QSlider* opacitySlider, QLabel* opacityValue, QCheckBox* colorbarCb,
                           int axis, bool enabled, float pos, float opacity, bool showColorbar,
                           auto setEnabled, auto setPos, auto setOpacity, auto setShowColorbar) {
        m_posSlider[axis] = posSlider;
        m_opacitySlider[axis] = opacitySlider;
        m_posValue[axis] = posValue;
        m_opacityValue[axis] = opacityValue;
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
    // meshLoadStateChanged lambda of MainWindow::connectSettings. This signal
    // also fires for every transient frame, so the handler must be idempotent
    // and must restore the checkboxes from the stored flags instead of forcing
    // them off (which made the planes impossible to keep enabled during a run).
    connect(s, &RenderSettings::meshLoadStateChanged, this, [this]() {
        syncFromSettings();
        refreshFields();
    });
    connect(s, &RenderSettings::meshDataUpdated, this, [this]() {
        syncFromSettings();
        refreshFields();
    });

    syncFromSettings();
    refreshFields();

    setContent(content);
}

// Mirror RenderSettings into the checkboxes. Per-axis state is authoritative:
// the page never writes it here, only reflects it (with signals blocked) so a
// mesh reload cannot clobber the user's selection.
void SlicePlanePage::syncFromSettings() {
    auto* s = S();
    const bool ok = s->hasVolumeData();
    const bool axisOn[3] = {s->getSlicePlaneEnabledX(),
                            s->getSlicePlaneEnabledY(),
                            s->getSlicePlaneEnabledZ()};
    QCheckBox* boxes[3] = {m_enableX, m_enableY, m_enableZ};
    for (int axis = 0; axis < 3; ++axis) {
        if (!boxes[axis]) continue;
        QSignalBlocker block(boxes[axis]);
        boxes[axis]->setChecked(axisOn[axis]);
        boxes[axis]->setEnabled(ok);
    }
    // A plane restored on/off through settings implies the master gate is on.
    if (ok && (axisOn[0] || axisOn[1] || axisOn[2])) m_masterOn = true;

    if (m_showCb) {
        QSignalBlocker block(m_showCb);
        m_showCb->setEnabled(ok);
        m_showCb->setChecked(ok && m_masterOn);
    }
    if (m_optionsGroup) m_optionsGroup->setEnabled(ok && m_masterOn);

    // Keep the sliders and their value labels in step with the stored state;
    // they are only initialized once in setupPlane.
    const double positions[3] = {s->getSlicePlanePosX(), s->getSlicePlanePosY(), s->getSlicePlanePosZ()};
    const double opacities[3] = {s->getSlicePlaneOpacityX(), s->getSlicePlaneOpacityY(), s->getSlicePlaneOpacityZ()};
    for (int axis = 0; axis < 3; ++axis) {
        if (m_posSlider[axis]) {
            QSignalBlocker block(m_posSlider[axis]);
            m_posSlider[axis]->setValue(static_cast<int>(positions[axis] * 1000.0f));
        }
        if (m_posValue[axis])
            m_posValue[axis]->setText(QString::number(positions[axis] * 100.0f, 'f', 0) + "%");
        if (m_opacitySlider[axis]) {
            QSignalBlocker block(m_opacitySlider[axis]);
            m_opacitySlider[axis]->setValue(static_cast<int>(opacities[axis] * 1000.0f));
        }
        if (m_opacityValue[axis])
            m_opacityValue[axis]->setText(QString::number(opacities[axis] * 100.0f, 'f', 0) + "%");
    }
}

// Rebuild each axis combo only when the available field set changed, preserving
// the stored per-axis field (falling back to the active scalar).
void SlicePlanePage::refreshFields() {
    auto* s = S();
    const QStringList names = s->getAvailableScalars();
    for (int axis = 0; axis < 3; ++axis) {
        auto* combo = m_fieldCombo[axis];
        if (!combo) continue;
        QStringList shown;
        shown.reserve(combo->count());
        for (int i = 0; i < combo->count(); ++i) shown << combo->itemText(i);
        if (shown != names) {
            QSignalBlocker block(combo);
            combo->clear();
            combo->addItems(names);
        }
        QString field = s->getSlicePlaneField(axis);
        if (field.isEmpty()) field = s->getActiveScalarNameQml();
        if (!field.isEmpty() && combo->currentText() != field
            && combo->findText(field) >= 0) {
            QSignalBlocker block(combo);
            combo->setCurrentText(field);
        }
        combo->setEnabled(!names.isEmpty());
    }
}

} // namespace sidebar
