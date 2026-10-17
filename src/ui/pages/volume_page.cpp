#include "volume_page.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <cmath>

#include "ui_volume_page.h"
#include "ui/transfer_function_widget.h"
#include "render/settings/render_settings.h"
#include "ui/panel_helpers.h"

namespace sidebar {

VolumePage::VolumePage(RenderSettings* settings, QWidget* parent)
    : SidebarPage(settings, parent) {
    auto* content = new QWidget;
    Ui::VolumePage volumeUi;
    volumeUi.setupUi(content);
    auto* s = S();
    // fixLayoutOverflow is deferred to just before applyPanelStyling: the
    // TransferFunctionWidget is constructed further down and needs relaxing too.

    m_showCb = volumeUi.showVolumeCb;
    m_optionsGroup = volumeUi.optionsGroup;
    m_showCb->setChecked(s->getShowVolume());
    m_showCb->setEnabled(s->hasVolumeData());
    if (!s->hasVolumeData()) m_showCb->setChecked(false);
    connect(m_showCb, &QCheckBox::toggled, s, &RenderSettings::setShowVolume);
    // Full-volume controls (step size, opacity) are individually disabled when showVolume is off.
    connect(m_showCb, &QCheckBox::toggled, this, [this](bool) { applyGating(); });
    applyGating();

    m_fieldCombo = volumeUi.volumeFieldCombo;
    m_fieldCombo->addItems(s->getAvailableScalars());
    m_fieldCombo->setCurrentText(s->getActiveScalarNameQml());
    m_fieldCombo->setEnabled(s->hasMeshScalars());
    m_fieldCombo->setMinimumWidth(sb::kSidebarWidth - sb::kNavWidthApprox - 20);
    connect(m_fieldCombo, &QComboBox::activated, s, [this, s](int idx) {
        m_fieldCombo->setCurrentIndex(idx);
        s->setActiveScalarField(m_fieldCombo->itemText(idx));
    });

    // Palette, bands, ticks, fixed range live in the Colorbar Style dialog.
    // "Use Colormap" stays here: it switches volume ray-march output between
    // palette-mapped color and raw grayscale, not colorbar styling.
    auto* useCmapCb = volumeUi.volumeUseCmapCb;
    useCmapCb->setChecked(s->getVolumeUseColormap());
    connect(useCmapCb, &QCheckBox::toggled, s, &RenderSettings::setVolumeUseColormap);

    {
        auto* slider = volumeUi.stepSlider;
        auto* valueLabel = volumeUi.stepValue;
        // Log mapping: raw in [1000,4000] <-> step in [1e-3,1e0].
        // (Previous range 1..1000 only covered 1e-4..1e-3, pinning the
        // default 0.01 to the slider max and making the slider dead.)
        slider->setRange(1000, 4000);
        slider->setValue(static_cast<int>(std::log10(s->getVolumeStepSize()) * 1000.0 + 4000.0));
        valueLabel->setText(QString::number(s->getVolumeStepSize(), 'f', 4));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, s](int raw) {
            double v = std::pow(10.0, (raw - 4000.0) / 1000.0);
            valueLabel->setText(QString::number(v, 'f', 4));
            s->setVolumeStepSize(v);
        });
    }

    {
        auto* slider = volumeUi.opacitySlider;
        auto* valueLabel = volumeUi.opacityValue;
        slider->setRange(0, 1000);
        slider->setValue(static_cast<int>(s->getVolumeOpacity() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, s](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 3));
            s->setVolumeOpacity(v);
        });
    }

    // Full-volume-rendering-only controls: greyed when "Enable Volume Rendering" is off.
    m_renderCtrls << volumeUi.stepLabel << volumeUi.stepSlider << volumeUi.stepValue
                  << volumeUi.opacityLabel << volumeUi.opacitySlider << volumeUi.opacityValue;
    applyGating();

    // --- Transfer Function Editor ---
    // Lives in its own zero-margin group rather than directly on the page root,
    // matching how the Isosurface page nests code-built widgets. This keeps the
    // TF section a single gating unit: applyPanelStyling inserts the header
    // divider as a sibling of tfHeader inside tfGroup, so greying tfGroup greys
    // the divider too. On the root layout the divider sat outside the disabled
    // set and stayed bright above a greyed header.
    auto* tfGroup = new QWidget;
    auto* tfLayout = new QVBoxLayout(tfGroup);
    tfLayout->setSpacing(4);
    tfLayout->setContentsMargins(0, 0, 0, 0);

    auto* tfHeader = new QLabel("Transfer Function (Opacity)");
    tfHeader->setObjectName("tfHeader");
    tfLayout->addWidget(tfHeader);
    m_tfEnableCb = new QCheckBox("Enable TF");
    m_tfEnableCb->setChecked(s->getVolumeTFEnabled());
    connect(m_tfEnableCb, &QCheckBox::toggled, s, &RenderSettings::setVolumeTFEnabled);
    tfLayout->addWidget(m_tfEnableCb);
    m_tfWidget = new TransferFunctionWidget;
    {
        auto curve = s->getVolumeTFCurve();
        m_tfWidget->setCurve(tf::deserialize(curve.toStdString()));
        auto [lo, hi] = std::pair<float, float>{s->getDataScalarMinQml(), s->getDataScalarMaxQml()};
        m_tfWidget->setFieldRange(lo, hi);
    }
    connect(m_tfWidget, &TransferFunctionWidget::curveChanged, this, [this](const tf::Curve& c) {
        QVariantList pts;
        for (auto& p : c) { QVariantMap m; m["pos"] = p.pos; m["alpha"] = p.alpha; pts << m; }
        S()->setVolumeTransferCurve(pts);
        // Editing the curve is an explicit intent to use it. With TF off the
        // shader never samples it (volume.frag: uTFEnabled==0 falls back to raw
        // tVal), so the edit would silently do nothing on screen. Turn it on and
        // let the checkbox visibly tick rather than leave the user hunting.
        if (!m_tfEnableCb->isChecked()) m_tfEnableCb->setChecked(true);
    });
    tfLayout->addWidget(m_tfWidget);
    // Preset row. The combo applies on selection, so it needs no commit button —
    // the curve it edits has been applying live on every drag frame all along,
    // and a button labelled "Apply" under it described an action that was
    // already continuous rather than the one it actually performed (loading the
    // preset). A label is what makes the combo read as an action.
    auto* tfPresetLabel = new QLabel("Preset");
    tfPresetLabel->setObjectName("tfPresetLabel"); // applyPanelStyling: 10px muted
    auto* tfPresetCombo = new QComboBox;
    tfPresetCombo->addItems({"Linear", "Step", "Low"});
    auto* tfPresetRow = new QWidget;
    auto* tfPresetRowLay = new QHBoxLayout(tfPresetRow);
    tfPresetRowLay->setContentsMargins(0, 0, 0, 0);
    tfPresetRowLay->setSpacing(6);
    tfPresetRowLay->addWidget(tfPresetLabel);
    tfPresetRowLay->addWidget(tfPresetCombo, 1);
    tfLayout->addWidget(tfPresetRow);
    // Connected after addItems so populating the combo does not fire a preset
    // change during construction. No explicit setCurve here (or on Reset): both
    // setVolumeTransferPreset and resetVolumeTransferFunction emit
    // viewChanged, which reaches this page's refreshVolumeTF and repaints the
    // editor. That round-trip is what made a manual repaint here look
    // necessary in the first place.
    connect(tfPresetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        S()->setVolumeTransferPreset(idx);
        // A preset is the same explicit intent as a hand edit; see curveChanged.
        if (!m_tfEnableCb->isChecked()) m_tfEnableCb->setChecked(true);
    });
    auto* tfResetBtn = new QPushButton("Reset");
    connect(tfResetBtn, &QPushButton::clicked, this, [this, tfPresetCombo]() {
        // Curve only. It used to also clear volumeTFEnabled, so "Reset" silently
        // switched the transfer function off as a side effect.
        S()->resetVolumeTransferFunction();
        // Reset lands on the linear default, so put the combo back to Linear
        // rather than leave it claiming a preset the curve no longer matches.
        tfPresetCombo->blockSignals(true);
        tfPresetCombo->setCurrentIndex(0);
        tfPresetCombo->blockSignals(false);
    });
    tfLayout->addWidget(tfResetBtn);

    content->layout()->addWidget(tfGroup);
    // One entry gates the header, its divider, the checkbox, the curve editor
    // and the preset controls together.
    m_renderCtrls << tfGroup;

    qobject_cast<QVBoxLayout*>(content->layout())->addStretch();

    sb::fixLayoutOverflow(content);
    setContent(content);

    connect(s, &RenderSettings::meshLoadStateChanged, this, [this, s]() {
        if (m_showCb) {
            const bool ok = s->hasVolumeData();
            m_showCb->blockSignals(true);
            m_showCb->setEnabled(ok);
            if (!ok) {
                m_showCb->setChecked(false);
                s->setShowVolume(false);
            } else {
                m_showCb->setChecked(s->getShowVolume());
            }
            m_showCb->blockSignals(false);
        }
        applyGating();
    });
    connect(s, &RenderSettings::viewChanged, this, [this, s](ChangeFlags) {
        if (m_showCb) {
            const bool ok = s->hasVolumeData();
            m_showCb->blockSignals(true);
            m_showCb->setEnabled(ok);
            m_showCb->setChecked(ok && s->getShowVolume());
            m_showCb->blockSignals(false);
        }
        refreshVolumeTF();
        applyGating();
    });
}

// The Volume page places both full-volume-rendering controls (Step Size, Opacity,
// Wireframe) and slice-plane controls inside one `optionsGroup`. Only the
// full-volume controls should be greyed when "Enable Volume Rendering" is off; the
// slice controls must remain configurable so a slice plane can be viewed with no
// volume rendering. This re-applies that split whenever showVolume/data availability
// changes (checkbox toggle, quick-bar button, mesh load). The group itself is
// enabled purely on volume-data availability so slice options are always reachable.
void VolumePage::applyGating() {
    auto* s = S();
    const bool hasVol = s->hasVolumeData() || s->getIsosurfaceAvailable();
    if (m_optionsGroup) m_optionsGroup->setEnabled(hasVol);
    const bool enableRenderControls = hasVol && s->getShowVolume();
    for (QWidget* w : m_renderCtrls) {
        if (w) w->setEnabled(enableRenderControls);
    }
}

void VolumePage::refreshVolumeTF() {
    auto* s = S();
    if (m_tfWidget) {
        auto str = s->getVolumeTFCurve();
        tf::Curve c = tf::deserialize(str.toStdString());
        // This runs on every viewChanged, including the one setVolumeTransferCurve
        // emits on each drag frame — so without this guard the editor is rebuilt
        // from a serialize/deserialize round-trip while m_dragging is still true.
        // The drag survived that only because mouseMoveEvent re-finds the dragged
        // index with a 1e-5 tolerance; comparing here removes the dependency.
        if (!(m_tfWidget->curve() == c)) m_tfWidget->setCurve(c);
        auto [lo, hi] = std::pair<float, float>{s->getDataScalarMinQml(), s->getDataScalarMaxQml()};
        m_tfWidget->setFieldRange(lo, hi);
    }
    if (m_tfEnableCb) {
        m_tfEnableCb->blockSignals(true);
        m_tfEnableCb->setChecked(s->getVolumeTFEnabled());
        m_tfEnableCb->blockSignals(false);
    }
}

} // namespace sidebar
