#include "vectors_page.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QVBoxLayout>

#include "ui_vectors_page.h"
#include "render/settings/render_settings.h"
#include "ui/panel_helpers.h"

namespace sidebar {

VectorsPage::VectorsPage(RenderSettings* settings, QWidget* parent)
    : SidebarPage(settings, parent) {
    auto* page = new QWidget;
    Ui::VectorsPage vectorsUi;
    vectorsUi.setupUi(page);
    sb::fixLayoutOverflow(page);

    auto* s = S();

    // Unified visualization mode: Off / Glyph / Surface LIC (single active field)
    auto* visModeCombo = vectorsUi.visModeCombo;
    m_vectorVisModeCombo = visModeCombo;
    visModeCombo->addItems({"Off", "Glyph", "Surface LIC"});
    visModeCombo->setCurrentIndex(s->getVectorVisMode());
    visModeCombo->setMinimumWidth(sb::kSidebarWidth - sb::kNavWidthApprox - 20);
    visModeCombo->setEnabled(!s->getAvailableVectors().isEmpty());
    connect(visModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            s, &RenderSettings::setVectorVisMode);

    // Legacy checkboxes kept hidden for backward compat but not used as primary UI
    auto* showCb = vectorsUi.showCb;
    m_vecShowCb = showCb;
    showCb->hide();
    showCb->setChecked(s->getShowVectors());

    m_vectorCombo = vectorsUi.vectorCombo;
    m_vectorCombo->addItems(s->getAvailableVectors());
    m_vectorCombo->setCurrentText(s->getVectorField());
    m_vectorCombo->setMinimumWidth(sb::kSidebarWidth - sb::kNavWidthApprox - 20);
    connect(m_vectorCombo, &QComboBox::activated, s, [this, s](int) {
        s->setActiveVectorField(m_vectorCombo->currentText());
    });

    m_vectorPlacementCombo = vectorsUi.placementCombo;
    m_vectorPlacementCombo->addItems(s->getVectorPlacementOptions());
    m_vectorPlacementCombo->setCurrentIndex(s->getVectorPlacement());
    connect(m_vectorPlacementCombo, &QComboBox::currentIndexChanged, s, [s](int idx) {
        s->setVectorPlacement(idx);
    });

    {
        auto* slider = vectorsUi.scaleSlider;
        auto* valueLabel = vectorsUi.scaleValue;
        slider->setRange(static_cast<int>(0.01 * 1000), static_cast<int>(5.0 * 1000));
        slider->setValue(static_cast<int>(s->getVectorScale() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel, s](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 2));
            s->setVectorScale(v);
        });
    }

    auto* scaleMagCb = vectorsUi.scaleMagCb;
    scaleMagCb->setChecked(s->getVectorScaleByMagnitude());
    connect(scaleMagCb, &QCheckBox::toggled, s, &RenderSettings::setVectorScaleByMagnitude);

    {
        auto* slider = vectorsUi.strideSlider;
        auto* valueLabel = vectorsUi.strideValue;
        slider->setRange(static_cast<int>(1 * 1000), static_cast<int>(20 * 1000));
        slider->setValue(static_cast<int>(s->getVectorStride() * 1000));
        connect(slider, &QSlider::valueChanged, this, [valueLabel](int raw) {
            double v = raw / 1000.0;
            valueLabel->setText(QString::number(v, 'f', 0));
        });
        connect(slider, &QSlider::sliderReleased, this, [slider, s]() {
            int v = static_cast<int>(slider->value() / 1000.0);
            if (v < 1) v = 1;
            s->setVectorStride(v);
        });
    }

    auto* licGroup = vectorsUi.licGroup;
    auto* optionsGroup = vectorsUi.optionsGroup;
    auto* fieldGroup = vectorsUi.fieldGroup;
    m_vectorFieldGroup = fieldGroup;
    m_vectorGlyphGroup = optionsGroup;
    m_vectorLicGroup = licGroup;
    // Mode-driven group enable: Glyph=1, LIC=2 — Field is shared (enabled for any non-Off mode)
    bool hasVectorsAvail = !s->getAvailableVectors().isEmpty();
    fieldGroup->setEnabled(hasVectorsAvail && s->getVectorVisMode() != 0);
    licGroup->setEnabled(s->getVectorVisMode() == 2);
    optionsGroup->setEnabled(s->getVectorVisMode() == 1);
    connect(visModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [licGroup, optionsGroup, fieldGroup, s](int idx) {
                bool hasVec = !s->getAvailableVectors().isEmpty();
                fieldGroup->setEnabled(hasVec && idx != 0);
                licGroup->setEnabled(idx == 2);
                optionsGroup->setEnabled(idx == 1);
            });

    {
        auto* spin = vectorsUi.licStepsSpin;
        spin->setRange(4, 128);
        spin->setValue(s->getLicSteps());
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [s](double v) {
            s->setLicSteps(static_cast<int>(std::round(v)));
        });
    }
    {
        auto* spin = vectorsUi.licStepSizeSpin;
        spin->setRange(0.001, 2.0);
        spin->setValue(s->getLicStepSize());
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), s, &RenderSettings::setLicStepSize);
    }
    {
        auto* spin = vectorsUi.licFreqSpin;
        spin->setRange(0.5, 64.0);
        spin->setValue(s->getLicNoiseFreq());
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), s, &RenderSettings::setLicNoiseFreq);
    }

    auto* licGrainCombo = vectorsUi.licGrainCombo;
    licGrainCombo->addItems({"64", "128", "256", "512"});
    licGrainCombo->setCurrentText(QString::number(s->getLicNoiseGrain()));
    connect(licGrainCombo, &QComboBox::activated, this, [s](int idx) {
        int grain = 64;
        switch (idx) {
            case 0: grain = 64; break;
            case 1: grain = 128; break;
            case 2: grain = 256; break;
            case 3: grain = 512; break;
        }
        s->setLicNoiseGrain(grain);
    });

    auto* licEnhancedCb = vectorsUi.licEnhancedCb;
    licEnhancedCb->setChecked(s->getLicEnhanced());
    connect(licEnhancedCb, &QCheckBox::toggled, s, &RenderSettings::setLicEnhanced);

    auto* licOnlyCb = vectorsUi.licOnlyCb;
    licOnlyCb->setChecked(s->getLicOnly());
    connect(licOnlyCb, &QCheckBox::toggled, s, &RenderSettings::setLicOnly);

    auto* licIntegratorCombo = vectorsUi.licIntegratorCombo;
    licIntegratorCombo->addItems(s->getLicIntegratorOptions());
    licIntegratorCombo->setCurrentIndex(s->getLicIntegrator());
    connect(licIntegratorCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            s, &RenderSettings::setLicIntegrator);

    auto* magCombo = vectorsUi.magCombo;
    magCombo->addItems({"Linear", "Square root", "Logarithmic"});
    magCombo->setCurrentIndex(s->getVectorMagTransform());
    connect(magCombo, &QComboBox::activated, s, [s](int idx) { s->setVectorMagTransform(idx); });

    vectorsUi.optionsGroup->layout()->replaceWidget(vectorsUi.vectorColorBtn,
        sb::detail::createColorButton(this, s, "Vector", s->getVectorColorQml(),
                                      &m_vectorColorDialog, &RenderSettings::setVectorColorQml));
    delete vectorsUi.vectorColorBtn;

    auto* colorModeCombo = new QComboBox;
    colorModeCombo->addItems({"Solid Color", "Magnitude", "X Component", "Y Component", "Z Component"});
    colorModeCombo->setCurrentIndex(s->getVectorColorMode());
    colorModeCombo->setMinimumWidth(sb::kSidebarWidth - sb::kNavWidthApprox - 20);
    connect(colorModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            s, &RenderSettings::setVectorColorMode);
    vectorsUi.optionsGroup->layout()->replaceWidget(vectorsUi.vectorColorModeCombo, colorModeCombo);
    delete vectorsUi.vectorColorModeCombo;

    // All colorbar properties live in the Colorbar Style dialog.

    qobject_cast<QVBoxLayout*>(page->layout())->addStretch();

    // Mesh (re)load gating: was a branch of the meshLoadStateChanged lambda in
    // MainWindow::connectSettings.
    connect(s, &RenderSettings::meshLoadStateChanged, this, [this, s]() {
        const bool hasVectors = s->hasMeshVectors();
        const bool hasCellVectors = s->hasMeshCellVectors();
        if (m_vectorVisModeCombo) {
            bool hasAny = hasVectors || hasCellVectors;
            m_vectorVisModeCombo->setEnabled(hasAny);
        }
        if (m_vectorFieldGroup) {
            bool hasAny = hasVectors || hasCellVectors;
            m_vectorFieldGroup->setEnabled(hasAny && s->getVectorVisMode() != 0);
        }
        if (m_vecShowCb) {
            m_vecShowCb->setEnabled(hasVectors || hasCellVectors);
            if (!(hasVectors || hasCellVectors)) m_vecShowCb->setChecked(false);
        }
    });

    // Field combos: was a branch of the meshDataUpdated lambda.
    connect(s, &RenderSettings::meshDataUpdated, this, &VectorsPage::refreshPageFields);

    // Vis-mode / placement sync from code paths: was in the viewChanged handler.
    connect(s, &RenderSettings::viewChanged, this, [this, s](ChangeFlags) {
        if (m_vectorVisModeCombo) {
            m_vectorVisModeCombo->blockSignals(true);
            m_vectorVisModeCombo->setCurrentIndex(s->getVectorVisMode());
            m_vectorVisModeCombo->blockSignals(false);
        }
        if (m_vectorFieldGroup) {
            bool hasAny = s->hasMeshVectors() || s->hasMeshCellVectors();
            m_vectorFieldGroup->setEnabled(hasAny && s->getVectorVisMode() != 0);
        }
        if (m_vectorGlyphGroup) m_vectorGlyphGroup->setEnabled(s->getVectorVisMode() == 1);
        if (m_vectorLicGroup) m_vectorLicGroup->setEnabled(s->getVectorVisMode() == 2);
        if (m_vecShowCb) m_vecShowCb->setChecked(s->getShowVectors());
    });

    setContent(page);
}

void VectorsPage::refreshPageFields() {
    auto* s = S();
    const QStringList vectors = s->getAvailableVectors();
    if (m_vectorCombo) {
        m_vectorCombo->blockSignals(true);
        m_vectorCombo->clear();
        m_vectorCombo->addItems(vectors);
        m_vectorCombo->setCurrentText(s->getVectorField());
        m_vectorCombo->blockSignals(false);
    }
    if (m_vectorPlacementCombo) {
        m_vectorPlacementCombo->blockSignals(true);
        m_vectorPlacementCombo->setCurrentIndex(s->getVectorPlacement());
        m_vectorPlacementCombo->setEnabled(s->hasMeshCellVectors());
        m_vectorPlacementCombo->blockSignals(false);
    }
    if (m_vectorVisModeCombo) {
        bool hasField = !s->getVectorField().isEmpty();
        // Keep LIC option available only when field selected; otherwise force Off
        if (!hasField && m_vectorVisModeCombo->currentIndex() == 2) {
            m_vectorVisModeCombo->setCurrentIndex(0);
        }
    }
}

} // namespace sidebar
