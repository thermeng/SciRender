#include "streamlines_page.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include "ui_streamlines_page.h"
#include "render/settings/render_settings.h"
#include "ui/panel_helpers.h"

namespace sidebar {

StreamlinesPage::StreamlinesPage(RenderSettings* settings, QWidget* parent)
    : SidebarPage(settings, parent) {
    buildContent();
    // Page-level connections live here, once. buildContent() is re-run by
    // Reset; connecting inside it would stack duplicate handlers on every
    // reset (each re-clearing the field combo on mesh updates).
    auto* s = S();
    // Streamlines integrate node vectors; cell-only vectors cannot seed an
    // integration (matches the auto-disable in RenderSettings::loadMesh).
    connect(s, &RenderSettings::meshLoadStateChanged, this, [this, s]() {
        const bool hasVectors = s->hasMeshVectors();
        if (m_showCb) {
            m_showCb->setEnabled(hasVectors);
            if (!hasVectors) m_showCb->setChecked(false);
        }
    });
    connect(s, &RenderSettings::meshDataUpdated, this, [this, s]() {
        if (m_streamlineCombo) {
            m_streamlineCombo->blockSignals(true);
            m_streamlineCombo->clear();
            m_streamlineCombo->addItems(s->getAvailableVectors());
            m_streamlineCombo->setCurrentText(s->getStreamlineVectorField());
            m_streamlineCombo->blockSignals(false);
        }
        if (m_streamlineDirectionCombo) {
            m_streamlineDirectionCombo->blockSignals(true);
            m_streamlineDirectionCombo->setCurrentText(s->getStreamlineDirection());
            m_streamlineDirectionCombo->blockSignals(false);
        }
    });
}

void StreamlinesPage::buildContent() {
    auto* content = new QWidget;
    Ui::StreamlinesPage slUi;
    slUi.setupUi(content);
    sb::fixLayoutOverflow(content);

    auto* s = S();

    m_showCb = slUi.showCb;
    m_showCb->setChecked(s->getShowStreamlines());
    connect(m_showCb, &QCheckBox::toggled, s, &RenderSettings::setShowStreamlines);
    connect(m_showCb, &QCheckBox::toggled, slUi.optionsGroup, &QWidget::setEnabled);
    slUi.optionsGroup->setEnabled(s->getShowStreamlines());

    auto* tabs = slUi.slTabs;

    // Tab: Flow
    auto* flowTab = new QWidget;
    auto* flowLay = new QVBoxLayout(flowTab);
    flowLay->setContentsMargins(4, 4, 4, 4);
    flowLay->setSpacing(4);

    m_streamlineCombo = new QComboBox;
    m_streamlineCombo->addItems(s->getAvailableVectors());
    m_streamlineCombo->setCurrentText(s->getStreamlineVectorField());
    m_streamlineCombo->setMinimumWidth(sb::kSidebarWidth - sb::kNavWidthApprox - 20);
    connect(m_streamlineCombo, &QComboBox::activated, s, [this, s](int) {
        s->setStreamlineVectorField(m_streamlineCombo->currentText());
    });
    flowLay->addWidget(m_streamlineCombo);

    sb::addSliderRow(s, flowLay, "stepSizeLabel", "Step Size",
                      s->getStreamlineStepSize(), 0.005, 0.1, 3,
                      &RenderSettings::setStreamlineStepSize);

    sb::addSliderRow(s, flowLay, "maxPathLengthLabel", "Max Path Length",
                      s->getStreamlineMaxPathLength(), 0.1, 100.0, 2,
                      &RenderSettings::setStreamlineMaxPathLength);

    auto* maxStepsSpin = new QSpinBox;
    maxStepsSpin->setRange(10, 5000);
    maxStepsSpin->setValue(s->getStreamlineMaxSteps());
    maxStepsSpin->setKeyboardTracking(false);
    maxStepsSpin->setCorrectionMode(QAbstractSpinBox::CorrectToNearestValue);
    connect(maxStepsSpin, &QSpinBox::valueChanged, s, &RenderSettings::setStreamlineMaxSteps);
    sb::addCtlRow(flowLay, "maxStepsLabel", "Max Steps (advanced)", maxStepsSpin, 1);

    auto* summaryLabel = new QLabel;
    summaryLabel->setObjectName("streamlineSummaryLabel");
    auto updateSummary = [summaryLabel, s]() {
        summaryLabel->setText(QString("Seeds: %1  Streamlines: %2  Truncated: %3")
            .arg(s->getStreamlineLastSeedCount())
            .arg(s->getStreamlineLastPathCount())
            .arg(s->getStreamlineLastTruncatedCount()));
    };
    updateSummary();
    connect(s, &RenderSettings::viewChanged, summaryLabel, [updateSummary](QFlags<ChangeFlag>) { updateSummary(); });
    flowLay->addWidget(summaryLabel);

    auto* directionCombo = new QComboBox;
    directionCombo->addItems({"Forward", "Backward", "Both"});
    directionCombo->setCurrentText(s->getStreamlineDirection());
    directionCombo->setFixedWidth(100);
    m_streamlineDirectionCombo = directionCombo;
    connect(directionCombo, &QComboBox::activated, s, [this, directionCombo, s](int) {
        s->setStreamlineDirection(directionCombo->currentText());
    });
    sb::addCtlRow(flowLay, "directionLabel", "Direction", directionCombo);

    auto* arrowsCb = new QCheckBox("Show Direction");
    arrowsCb->setChecked(s->getShowStreamlineArrows());
    connect(arrowsCb, &QCheckBox::toggled, s, &RenderSettings::setShowStreamlineArrows);
    flowLay->addWidget(arrowsCb);

    auto* resetBtn = new QPushButton("Reset to Defaults");
    connect(resetBtn, &QPushButton::clicked, s, [this, s]() {
        // Quiesce the animation first: particles index into the live paths,
        // and the reset below replaces them. Toggling showParticles off
        // also stops MainWindow's particle timer (viewChanged handler).
        const bool wasAnimating = s->getShowParticles() && s->getShowStreamlines();
        if (wasAnimating) s->setShowParticles(false);
        s->resetStreamlineDefaults(); // also quiesces + restores a valid field
        // Delete the old tree: QScrollArea::setWidget() does not delete it,
        // and its viewChanged/mesh connections would otherwise accumulate.
        QWidget* old = nullptr;
        if (auto* scroll = findChild<QScrollArea*>()) old = scroll->widget();
        buildContent();
        if (old && old != findChild<QScrollArea*>()->widget()) old->deleteLater();
        if (wasAnimating) s->setShowParticles(true); // resume on the new paths
    });
    flowLay->addWidget(resetBtn);

    auto* arrowSpacingSpin = new QDoubleSpinBox;
    arrowSpacingSpin->setToolTip("Distance between direction arrows as a fraction of the dataset extent");
    arrowSpacingSpin->setRange(0.02, 0.50);
    arrowSpacingSpin->setDecimals(2);
    arrowSpacingSpin->setSingleStep(0.01);
    arrowSpacingSpin->setValue(s->getStreamlineArrowSpacingFrac());
    connect(arrowSpacingSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            s, &RenderSettings::setStreamlineArrowSpacingFrac);
    sb::addCtlRow(flowLay, "arrowSpacingLabel", "Arrow spacing", arrowSpacingSpin, 1);

    sb::addSliderRow(s, flowLay, "arrowSizeLabel", "Arrow Size",
                     s->getStreamlineArrowSize(), 0.01, 0.2, 2,
                     &RenderSettings::setStreamlineArrowSize);

    auto* integrateBtn = new QPushButton("Integrate");
    connect(integrateBtn, &QPushButton::clicked, this, [this, s]() {
        s->backend()->markStreamlineDirty();
        emit viewportUpdateRequested();
    });
    flowLay->addWidget(integrateBtn);
    flowLay->addStretch();
    tabs->addTab(flowTab, tr("Flow"));

    // Tab: Seeds
    auto* seedsTab = new QWidget;
    auto* seedsLay = new QVBoxLayout(seedsTab);
    seedsLay->setContentsMargins(4, 4, 4, 4);
    seedsLay->setSpacing(4);

    auto* seedModeCombo = new QComboBox;
    seedModeCombo->addItems({"Volume", "Surface", "Plane XY", "Plane XZ", "Plane YZ"});
    const QStringList modeKeys = {"Volume", "Surface", "PlaneXY", "PlaneXZ", "PlaneYZ"};
    seedModeCombo->setCurrentIndex(modeKeys.indexOf(s->getSeedMode()));
    seedModeCombo->setMinimumWidth(sb::kSidebarWidth - sb::kNavWidthApprox - 20);
    connect(seedModeCombo, &QComboBox::activated, s, [s, modeKeys](int idx) {
        s->setSeedMode(modeKeys[idx]);
    });
    seedsLay->addWidget(seedModeCombo);

    sb::addSliderRow(s, seedsLay, "planePosLabel", "Plane Position",
                     s->getSeedPlanePos(), 0.0, 1.0, 2,
                     &RenderSettings::setSeedPlanePos);

    auto* seedsUSpin = new QSpinBox;
    seedsUSpin->setRange(1, 200);
    seedsUSpin->setValue(s->getSeedPlaneCountU());
    connect(seedsUSpin, &QSpinBox::valueChanged, s, &RenderSettings::setSeedPlaneCountU);
    sb::addCtlRow(seedsLay, "seedsULabel", "Seeds U", seedsUSpin, 1);

    auto* seedsVSpin = new QSpinBox;
    seedsVSpin->setRange(1, 200);
    seedsVSpin->setValue(s->getSeedPlaneCountV());
    connect(seedsVSpin, &QSpinBox::valueChanged, s, &RenderSettings::setSeedPlaneCountV);
    sb::addCtlRow(seedsLay, "seedsVLabel", "Seeds V", seedsVSpin, 1);

    auto updateSeedGridRows = [seedModeCombo, seedsUSpin, seedsVSpin]() {
        const bool planeSeeds = seedModeCombo->currentText().startsWith("Plane");
        seedsUSpin->setEnabled(planeSeeds);
        seedsVSpin->setEnabled(planeSeeds);
    };
    connect(seedModeCombo, &QComboBox::activated, seedsUSpin, updateSeedGridRows);
    updateSeedGridRows();

    sb::addSliderRow(s, seedsLay, "jitterLabel", "Jitter",
                     s->getSeedJitter(), 0.0, 1.0, 2,
                     &RenderSettings::setSeedJitter);

    auto* showSeedsCb = new QCheckBox("Show seeds");
    showSeedsCb->setChecked(s->getShowSeeds());
    connect(showSeedsCb, &QCheckBox::toggled, s, &RenderSettings::setShowSeeds);
    seedsLay->addWidget(showSeedsCb);

    sb::addSliderRow(s, seedsLay, "seedSizeLabel", "Seed Size",
                     s->getSeedPointSize(), 1.0, 20.0, 1,
                     &RenderSettings::setSeedPointSize);

    auto* seedBtn = sb::createSwatchButton("Seed Color", s->getSeedPointColorQml(), nullptr);
    connect(seedBtn, &QPushButton::clicked, this, [seedBtn, s]() {
        // Dialog parented to the button: it dies with the content tree on
        // Reset, so no dangling capture and no stale member pointer.
        auto* dlg = new QColorDialog(s->getSeedPointColorQml(), seedBtn);
        connect(dlg, &QColorDialog::colorSelected, s, &RenderSettings::setSeedPointColorQml);
        connect(dlg, &QColorDialog::colorSelected, seedBtn, [seedBtn](const QColor& c) {
            QPixmap pix(14, 14); pix.fill(c); seedBtn->setIcon(pix);
        });
        dlg->open();
    });
    seedsLay->addWidget(seedBtn);
    seedsLay->addStretch();
    tabs->addTab(seedsTab, tr("Seeds"));

    // Tab: Look
    auto* lookTab = new QWidget;
    auto* lookLay = new QVBoxLayout(lookTab);
    lookLay->setSpacing(2);
    lookLay->setContentsMargins(4, 4, 4, 4);

    auto* streamlineBtn = sb::createSwatchButton("Line color", s->getStreamlineColorQml(), nullptr);
    connect(streamlineBtn, &QPushButton::clicked, this, [streamlineBtn, s]() {
        auto* dlg = new QColorDialog(s->getStreamlineColorQml(), streamlineBtn);
        connect(dlg, &QColorDialog::colorSelected, s, &RenderSettings::setStreamlineColorQml);
        connect(dlg, &QColorDialog::colorSelected, streamlineBtn, [streamlineBtn](const QColor& c) {
            QPixmap pix(14, 14); pix.fill(c); streamlineBtn->setIcon(pix);
        });
        dlg->open();
    });
    lookLay->addWidget(streamlineBtn);

    // ---- Streamline Color By (combo) ----
    auto* slColorModeCombo = new QComboBox;
    slColorModeCombo->setObjectName("streamlineColorModeCombo");
    slColorModeCombo->addItems({"Solid Color", "Magnitude", "X Component", "Y Component", "Z Component"});
    slColorModeCombo->setCurrentIndex(s->getStreamlineColorMode());
    slColorModeCombo->setMinimumWidth(sb::kSidebarWidth - sb::kNavWidthApprox - 20);
    connect(slColorModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            s, &RenderSettings::setStreamlineColorMode);
    sb::addCtlRow(lookLay, "slColorByLabel", "Color By", slColorModeCombo);
    // Palette now in Colorbar Style dialog — hide sidebar alias
    // Streamline Fixed Range now in Colorbar Style dialog — removed from sidebar
    sb::addSliderRow(s, lookLay, "opacityLabel", "Opacity",
                     s->getStreamlineOpacity(), 0.0, 1.0, 2,
                     &RenderSettings::setStreamlineOpacity);
    auto* styleCombo = new QComboBox;
    styleCombo->addItems({"Line", "Tube", "Ribbon"});
    styleCombo->setCurrentIndex(s->getStreamlineStyle());
    styleCombo->setFixedWidth(100);
    connect(styleCombo, &QComboBox::activated, s, [s](int idx) {
        s->setStreamlineStyle(idx);
        if (idx == 0) { // Line
            s->setStreamlineRibbonWidth(0.0001);
            s->setStreamlineTaperFactor(0.0);
            s->setStreamlineArrowSize(0.01);
        } else if (idx == 1) { // Tube
            s->setStreamlineRibbonWidth(0.001);
            s->setStreamlineTaperFactor(0.0);
            s->setStreamlineArrowSize(0.02);
        } else { // Ribbon
            s->setStreamlineRibbonWidth(0.002);
            s->setStreamlineTaperFactor(0.3);
            s->setStreamlineArrowSize(0.03);
        }
    });
    sb::addCtlRow(lookLay, "streamlineStyleLabel", "Style", styleCombo);

    sb::addSliderRow(s, lookLay, "ribbonWidthLabel", "Ribbon Width",
                      s->getStreamlineRibbonWidth(), 0.0001, 0.01, 4,
                      &RenderSettings::setStreamlineRibbonWidth);
    sb::addSliderRow(s, lookLay, "taperFactorLabel", "Taper Factor",
                     s->getStreamlineTaperFactor(), 0.0, 0.8, 2,
                     &RenderSettings::setStreamlineTaperFactor);

    auto* autoFitBtn = new QPushButton("Auto-fit Ranges");
    connect(autoFitBtn, &QPushButton::clicked, s, [s]() {
        s->resetStreamlineMagRangeOverride();
    });
    lookLay->addWidget(autoFitBtn);
    lookLay->addStretch();
    tabs->addTab(lookTab, tr("Look"));

    // Animate section (below the tabs so the tab bar stays arrow-free)
    auto* optionsLay = qobject_cast<QVBoxLayout*>(slUi.optionsGroup->layout());

    auto* particlesCb = new QCheckBox("Show Particles");
    particlesCb->setChecked(s->getShowParticles());
    connect(particlesCb, &QCheckBox::toggled, s, &RenderSettings::setShowParticles);
    optionsLay->addWidget(particlesCb);

    {
        auto row = sb::createLightSlider("Particle Count", s->getParticleCount(),
                                         10.0, 1000.0, 1.0, 0,
                                         [s](double v) { s->setParticleCount(static_cast<int>(v)); },
                                         "particleCountLabel");
        optionsLay->addWidget(row.slider->parentWidget());
    }
    sb::addSliderRow(s, optionsLay, "particleSpeedLabel", "Particle Speed",
                     s->getParticleSpeed(), 0.1, 10.0, 2,
                     &RenderSettings::setParticleSpeed);
    sb::addSliderRow(s, optionsLay, "particleSizeLabel", "Particle Size",
                     s->getParticleSize(), 1.0, 20.0, 1,
                     &RenderSettings::setParticleSize);

    auto* particleAdditiveCb = new QCheckBox("Additive Glow");
    particleAdditiveCb->setChecked(s->getParticleAdditive());
    connect(particleAdditiveCb, &QCheckBox::toggled, s, &RenderSettings::setParticleAdditive);
    optionsLay->addWidget(particleAdditiveCb);

    tabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    optionsLay->addStretch();

    setContent(content);
}

} // namespace sidebar
