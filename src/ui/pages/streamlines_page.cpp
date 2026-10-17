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

    auto* maxStepsSpin = new QSpinBox;
    maxStepsSpin->setRange(10, 500);
    maxStepsSpin->setValue(s->getStreamlineMaxSteps());
    connect(maxStepsSpin, &QSpinBox::valueChanged, s, &RenderSettings::setStreamlineMaxSteps);
    sb::addCtlRow(flowLay, "maxStepsLabel", "Max Steps", maxStepsSpin, 1);

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
    connect(seedBtn, &QPushButton::clicked, this, [this, seedBtn, s]() {
        if (!m_seedColorDialog) {
            m_seedColorDialog = new QColorDialog(s->getSeedPointColorQml(), this);
            connect(m_seedColorDialog, &QColorDialog::colorSelected, s, &RenderSettings::setSeedPointColorQml);
            connect(m_seedColorDialog, &QColorDialog::colorSelected, seedBtn, [seedBtn](const QColor& c) {
                QPixmap pix(14, 14); pix.fill(c); seedBtn->setIcon(pix);
            });
        }
        m_seedColorDialog->open();
    });
    seedsLay->addWidget(seedBtn);
    seedsLay->addStretch();
    tabs->addTab(seedsTab, tr("Seeds"));

    // Tab: Look
    auto* lookTab = new QWidget;
    auto* lookLay = new QVBoxLayout(lookTab);
    lookLay->setContentsMargins(4, 4, 4, 4);
    lookLay->setSpacing(4);

    auto* streamlineBtn = sb::createSwatchButton("Line color", s->getStreamlineColorQml(), nullptr);
    connect(streamlineBtn, &QPushButton::clicked, this, [this, streamlineBtn, s]() {
        if (!m_streamlineColorDialog) {
            m_streamlineColorDialog = new QColorDialog(s->getStreamlineColorQml(), this);
            connect(m_streamlineColorDialog, &QColorDialog::colorSelected, s, &RenderSettings::setStreamlineColorQml);
            connect(m_streamlineColorDialog, &QColorDialog::colorSelected, streamlineBtn, [streamlineBtn](const QColor& c) {
                QPixmap pix(14, 14); pix.fill(c); streamlineBtn->setIcon(pix);
            });
        }
        m_streamlineColorDialog->open();
    });
    lookLay->addWidget(streamlineBtn);

    // ---- Streamline Color By (combo) ----
    auto* slColorByLabel = new QLabel("Color By");
    slColorByLabel->setObjectName("slColorByLabel");
    lookLay->addWidget(slColorByLabel);
    auto* slColorModeCombo = new QComboBox;
    slColorModeCombo->setObjectName("streamlineColorModeCombo");
    slColorModeCombo->addItems({"Solid Color", "Magnitude", "X Component", "Y Component", "Z Component"});
    slColorModeCombo->setCurrentIndex(s->getStreamlineColorMode());
    slColorModeCombo->setMinimumWidth(sb::kSidebarWidth - sb::kNavWidthApprox - 20);
    connect(slColorModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            s, &RenderSettings::setStreamlineColorMode);
    lookLay->addWidget(slColorModeCombo);
    // Palette now in Colorbar Style dialog — hide sidebar alias
    // Streamline Fixed Range now in Colorbar Style dialog — removed from sidebar
    sb::addSliderRow(s, lookLay, "opacityLabel", "Opacity",
                     s->getStreamlineOpacity(), 0.0, 1.0, 2,
                     &RenderSettings::setStreamlineOpacity);
    sb::addSliderRow(s, lookLay, "ribbonWidthLabel", "Ribbon Width",
                     s->getStreamlineRibbonWidth(), 0.001, 0.05, 3,
                     &RenderSettings::setStreamlineRibbonWidth);
    sb::addSliderRow(s, lookLay, "taperFactorLabel", "Taper Factor",
                     s->getStreamlineTaperFactor(), 0.0, 0.8, 2,
                     &RenderSettings::setStreamlineTaperFactor);

    auto* shadingHeader = new QLabel("Shading");
    shadingHeader->setObjectName("slShadingHeader");
    lookLay->addWidget(shadingHeader);

    sb::addSliderRow(s, lookLay, "streamlineAmbientLabel", "Ambient",
                     s->getStreamlineAmbient(), 0.0, 1.0, 2,
                     &RenderSettings::setStreamlineAmbient);
    sb::addSliderRow(s, lookLay, "streamlineDiffuseLabel", "Diffuse",
                     s->getStreamlineDiffuse(), 0.0, 1.0, 2,
                     &RenderSettings::setStreamlineDiffuse);
    sb::addSliderRow(s, lookLay, "streamlineSpecularLabel", "Specular",
                     s->getStreamlineSpecular(), 0.0, 1.0, 2,
                     &RenderSettings::setStreamlineSpecular);

    auto* specPowerSpin = new QSpinBox;
    specPowerSpin->setRange(2, 128);
    specPowerSpin->setValue(s->getStreamlineSpecularPower());
    connect(specPowerSpin, &QSpinBox::valueChanged, s, &RenderSettings::setStreamlineSpecularPower);
    sb::addCtlRow(lookLay, "specPowerLabel", "Specular Power", specPowerSpin, 1);
    tabs->addTab(lookTab, tr("Look"));

    // Animate section (below the tabs so the tab bar stays arrow-free)
    auto* optionsLay = qobject_cast<QVBoxLayout*>(slUi.optionsGroup->layout());

    auto* particlesCb = new QCheckBox("Show Particles");
    particlesCb->setChecked(s->getShowParticles());
    connect(particlesCb, &QCheckBox::toggled, s, &RenderSettings::setShowParticles);
    optionsLay->addWidget(particlesCb);

    {
        auto row = sb::createLightSlider("Particle Count", s->getParticleCount(),
                                         10.0, 5000.0, 1.0, 0,
                                         [s](double v) { s->setParticleCount(static_cast<int>(v)); },
                                         "particleCountLabel");
        optionsLay->addWidget(row.slider->parentWidget());
    }
    sb::addSliderRow(s, optionsLay, "particleSpeedLabel", "Particle Speed",
                     s->getParticleSpeed(), 0.1, 100.0, 1,
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

    // Streamlines integrate node vectors; cell-only vectors cannot seed an
    // integration (matches the auto-disable in RenderSettings::loadMesh).
    connect(s, &RenderSettings::meshLoadStateChanged, this, [this, s]() {
        const bool hasVectors = s->hasMeshVectors();
        if (m_showCb) {
            m_showCb->setEnabled(hasVectors);
            if (!hasVectors) m_showCb->setChecked(false);
        }
    });

    // Field combos: was a branch of the meshDataUpdated lambda.
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

    setContent(content);
}

} // namespace sidebar
