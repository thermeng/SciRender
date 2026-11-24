#include "streamlines_page.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

#include "ui_streamlines_page.h"
#include "core/mesh_loader.h"
#include "render/settings/render_settings.h"
#include "ui/panel_helpers.h"

namespace sidebar {
namespace {

const RenderMesh* loadedMesh(RenderSettings* s) {
    auto* r = s ? s->backend() : nullptr;
    return r ? r->lastUploadedMeshForPlot() : nullptr;
}

// Replace the one-size-fits-all particle defaults with values derived from
// the current seed set and domain scale. Sliders remain available as overrides.
void autoTuneParticles(RenderSettings* s) {
    const auto* mesh = loadedMesh(s);
    const double extent = (mesh && mesh->bounds.extent > 0.0) ? mesh->bounds.extent : 1.0;
    const int seeds = s->getSeedMode().startsWith("Plane")
                    ? s->getSeedPlaneCountU() * s->getSeedPlaneCountV()
                    : s->getStreamlineSeedCount();
    s->setParticleCount(std::clamp(seeds * 4, 40, 300));
    s->setParticleSpeed(std::clamp(static_cast<float>(extent / 4.0), 0.1f, 10.0f));
    s->setParticleSize(4.0f);
}

} // namespace

StreamlinesPage::StreamlinesPage(RenderSettings* settings, QWidget* parent)
    : SidebarPage(settings, parent) {
    buildContent();
    // Page-level connections live here, once. buildContent() is re-run by
    // Reset; connecting inside it would stack duplicate handlers on every
    // reset (each re-clearing the field combo on mesh updates).
    auto* s = S();
    // Animation gate: structural controls lock while particles play.
    connect(s, &RenderSettings::viewChanged, this, [this](QFlags<ChangeFlag>) { updateAnimationGate(); });
    // Streamlines integrate node vectors; cell-only vectors cannot seed an
    // integration, and a 2D mesh has no volume to trace through.
    connect(s, &RenderSettings::meshLoadStateChanged, this, [this, s]() {
        const bool hasVectors = s->hasMeshVectors();
        if (m_showCb) {
            m_showCb->setEnabled(hasVectors);
            if (!hasVectors) m_showCb->setChecked(false);
        }
        autoTuneParticles(s);
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
    m_structuralControls.clear();
    m_animBanner = nullptr;
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

    // ---------------------------------------------------------------- Flow
    auto* flowTab = new QWidget;
    auto* flowLay = new QVBoxLayout(flowTab);
    flowLay->setContentsMargins(4, 4, 4, 4);
    flowLay->setSpacing(4);

    auto* flowHeader = new QLabel(tr("Flow Field"));
    flowHeader->setObjectName("flowHeader");
    flowLay->addWidget(flowHeader);

    m_streamlineCombo = new QComboBox;
    m_streamlineCombo->addItems(s->getAvailableVectors());
    m_streamlineCombo->setCurrentText(s->getStreamlineVectorField());
    uikit::sidebarControl(m_streamlineCombo);
    connect(m_streamlineCombo, &QComboBox::activated, s, [this, s](int) {
        s->setStreamlineVectorField(m_streamlineCombo->currentText());
    });
    sb::addCtlRow(flowLay, "vectorFieldLabel", tr("Field"), m_streamlineCombo, 1);

    auto* directionCombo = new QComboBox;
    directionCombo->addItems({"Forward", "Backward", "Both"});
    directionCombo->setCurrentText(s->getStreamlineDirection());
    uikit::sidebarControl(directionCombo);
    m_streamlineDirectionCombo = directionCombo;
    connect(directionCombo, &QComboBox::activated, s, [this, directionCombo, s](int) {
        s->setStreamlineDirection(directionCombo->currentText());
    });
    sb::addCtlRow(flowLay, "directionLabel", tr("Direction"), directionCombo, 1);

    auto* arrowsCb = new QCheckBox(tr("Show Direction Arrows"));
    arrowsCb->setChecked(s->getShowStreamlineArrows());
    connect(arrowsCb, &QCheckBox::toggled, s, &RenderSettings::setShowStreamlineArrows);
    flowLay->addWidget(arrowsCb);

    auto* arrowHeader = new QLabel(tr("Direction Arrows"));
    arrowHeader->setObjectName("arrowHeader");
    flowLay->addWidget(arrowHeader);

    auto* arrowSpacingSpin = uikit::sidebarNumeric(new QDoubleSpinBox);
    arrowSpacingSpin->setToolTip("Distance between arrows as a fraction of the dataset extent");
    arrowSpacingSpin->setRange(0.02, 0.50);
    arrowSpacingSpin->setDecimals(2);
    arrowSpacingSpin->setSingleStep(0.01);
    arrowSpacingSpin->setValue(s->getStreamlineArrowSpacingFrac());
    connect(arrowSpacingSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            s, &RenderSettings::setStreamlineArrowSpacingFrac);
    sb::addCtlRow(flowLay, "arrowSpacingLabel", tr("Spacing"), arrowSpacingSpin, 1);

    sb::addSliderRow(s, flowLay, "arrowSizeLabel", tr("Size"),
                     s->getStreamlineArrowSize(), 0.01, 0.2, 2,
                     &RenderSettings::setStreamlineArrowSize);

    flowLay->addStretch();
    m_structuralControls.push_back(flowTab);
    tabs->addTab(flowTab, tr("Flow"));

    // --------------------------------------------------------------- Seeds
    auto* seedsTab = new QWidget;
    auto* seedsLay = new QVBoxLayout(seedsTab);
    seedsLay->setContentsMargins(4, 4, 4, 4);
    seedsLay->setSpacing(4);

    auto* seedHeader = new QLabel(tr("Seed Placement"));
    seedHeader->setObjectName("seedPlacementHeader");
    seedsLay->addWidget(seedHeader);

    auto* seedModeCombo = new QComboBox;
    seedModeCombo->addItems({"Volume", "Surface", "Plane XY", "Plane XZ", "Plane YZ"});
    const QStringList modeKeys = {"Volume", "Surface", "PlaneXY", "PlaneXZ", "PlaneYZ"};
    seedModeCombo->setCurrentIndex(modeKeys.indexOf(s->getSeedMode()));
    uikit::sidebarControl(seedModeCombo);
    connect(seedModeCombo, &QComboBox::activated, s, [s, modeKeys](int idx) {
        s->setSeedMode(modeKeys[idx]);
    });
    sb::addCtlRow(seedsLay, "seedModeLabel", tr("Mode"), seedModeCombo, 1);

    auto* seedsSpin = uikit::sidebarNumeric(new QSpinBox);
    seedsSpin->setRange(1, 5000);
    seedsSpin->setValue(s->getStreamlineSeedCount());
    seedsSpin->setKeyboardTracking(false);
    seedsSpin->setToolTip("Number of seeds for Volume and Surface modes");
    connect(seedsSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            s, &RenderSettings::setStreamlineSeedCount);
    sb::addCtlRow(seedsLay, "seedCountLabel", tr("Seed Count"), seedsSpin, 1);

    sb::addSliderRow(s, seedsLay, "planePosLabel", tr("Plane Position"),
                     s->getSeedPlanePos(), 0.0, 1.0, 2,
                     &RenderSettings::setSeedPlanePos);

    auto* seedsUSpin = uikit::sidebarNumeric(new QSpinBox);
    seedsUSpin->setRange(1, 200);
    seedsUSpin->setValue(s->getSeedPlaneCountU());
    connect(seedsUSpin, &QSpinBox::valueChanged, s, &RenderSettings::setSeedPlaneCountU);
    sb::addCtlRow(seedsLay, "seedsULabel", tr("Grid U"), seedsUSpin, 1);

    auto* seedsVSpin = uikit::sidebarNumeric(new QSpinBox);
    seedsVSpin->setRange(1, 200);
    seedsVSpin->setValue(s->getSeedPlaneCountV());
    connect(seedsVSpin, &QSpinBox::valueChanged, s, &RenderSettings::setSeedPlaneCountV);
    sb::addCtlRow(seedsLay, "seedsVLabel", tr("Grid V"), seedsVSpin, 1);

    auto updateSeedGridRows = [seedModeCombo, seedsUSpin, seedsVSpin, seedsSpin]() {
        const bool planeSeeds = seedModeCombo->currentText().startsWith("Plane");
        seedsUSpin->setEnabled(planeSeeds);
        seedsVSpin->setEnabled(planeSeeds);
        seedsSpin->setEnabled(!planeSeeds);
    };
    connect(seedModeCombo, &QComboBox::activated, seedsSpin, updateSeedGridRows);
    updateSeedGridRows();

    sb::addSliderRow(s, seedsLay, "jitterLabel", tr("Jitter"),
                     s->getSeedJitter(), 0.0, 1.0, 2,
                     &RenderSettings::setSeedJitter);

    auto* markerHeader = new QLabel(tr("Seed Markers"));
    markerHeader->setObjectName("seedMarkersHeader");
    seedsLay->addWidget(markerHeader);

    auto* showSeedsCb = new QCheckBox(tr("Show seeds"));
    showSeedsCb->setChecked(s->getShowSeeds());
    connect(showSeedsCb, &QCheckBox::toggled, s, &RenderSettings::setShowSeeds);
    seedsLay->addWidget(showSeedsCb);

    sb::addSliderRow(s, seedsLay, "seedSizeLabel", tr("Size"),
                     s->getSeedPointSize(), 1.0, 20.0, 1,
                     &RenderSettings::setSeedPointSize);

    auto* seedBtn = sb::createSwatchButton(tr("Seed Color"), s->getSeedPointColorQml(), nullptr);
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
    m_structuralControls.push_back(seedsTab);
    tabs->addTab(seedsTab, tr("Seeds"));

    // ---------------------------------------------------------------- Look
    auto* lookTab = new QWidget;
    auto* lookLay = new QVBoxLayout(lookTab);
    lookLay->setSpacing(4);
    lookLay->setContentsMargins(4, 4, 4, 4);

    auto* lookHeader = new QLabel(tr("Appearance"));
    lookHeader->setObjectName("appearanceHeader");
    lookLay->addWidget(lookHeader);

    auto* streamlineBtn = sb::createSwatchButton(tr("Line color"), s->getStreamlineColorQml(), nullptr);
    connect(streamlineBtn, &QPushButton::clicked, this, [streamlineBtn, s]() {
        auto* dlg = new QColorDialog(s->getStreamlineColorQml(), streamlineBtn);
        connect(dlg, &QColorDialog::colorSelected, s, &RenderSettings::setStreamlineColorQml);
        connect(dlg, &QColorDialog::colorSelected, streamlineBtn, [streamlineBtn](const QColor& c) {
            QPixmap pix(14, 14); pix.fill(c); streamlineBtn->setIcon(pix);
        });
        dlg->open();
    });
    lookLay->addWidget(streamlineBtn);

    auto* slColorModeCombo = new QComboBox;
    slColorModeCombo->setObjectName("streamlineColorModeCombo");
    slColorModeCombo->addItems({"Solid Color", "Magnitude", "X Component", "Y Component", "Z Component"});
    slColorModeCombo->setCurrentIndex(s->getStreamlineColorMode());
    uikit::sidebarControl(slColorModeCombo);
    connect(slColorModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            s, &RenderSettings::setStreamlineColorMode);
    sb::addCtlRow(lookLay, "slColorByLabel", tr("Color By"), slColorModeCombo, 1);

    sb::addSliderRow(s, lookLay, "opacityLabel", tr("Opacity"),
                     s->getStreamlineOpacity(), 0.0, 1.0, 2,
                     &RenderSettings::setStreamlineOpacity);

    auto* styleCombo = new QComboBox;
    styleCombo->addItems({"Line", "Tube", "Ribbon"});
    styleCombo->setCurrentIndex(s->getStreamlineStyle());
    uikit::sidebarControl(styleCombo);
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
    sb::addCtlRow(lookLay, "streamlineStyleLabel", tr("Style"), styleCombo, 1);
    m_structuralControls.push_back(styleCombo);

    sb::addSliderRow(s, lookLay, "ribbonWidthLabel", tr("Ribbon Width"),
                     s->getStreamlineRibbonWidth(), 0.0001, 0.01, 4,
                     &RenderSettings::setStreamlineRibbonWidth);
    sb::addSliderRow(s, lookLay, "taperFactorLabel", tr("Taper Factor"),
                     s->getStreamlineTaperFactor(), 0.0, 0.8, 2,
                     &RenderSettings::setStreamlineTaperFactor);

    auto* rangeHeader = new QLabel(tr("Color Range"));
    rangeHeader->setObjectName("colorRangeHeader");
    lookLay->addWidget(rangeHeader);
    auto* autoFitBtn = new QPushButton(tr("Auto-fit Ranges"));
    connect(autoFitBtn, &QPushButton::clicked, s, [s]() {
        s->resetStreamlineMagRangeOverride();
    });
    lookLay->addWidget(autoFitBtn);
    lookLay->addStretch();
    tabs->addTab(lookTab, tr("Look"));

    // ------------------------------------------------------------- Animate
    // Below the tabs so the tab bar stays arrow-free. The controls stay hidden
    // until particles are enabled - they are tuning, not defaults.
    auto* optionsLay = qobject_cast<QVBoxLayout*>(slUi.optionsGroup->layout());

    auto* particlesCb = new QCheckBox(tr("Show Particles"));
    particlesCb->setChecked(s->getShowParticles());
    connect(particlesCb, &QCheckBox::toggled, s, &RenderSettings::setShowParticles);
    optionsLay->addWidget(particlesCb);

    // Auto-tuning writes settings on mesh load; keep the sliders in sync.
    auto syncParticleSlider = [this, s](QSlider* slider, QLabel* label, int decimals,
                                        std::function<double()> value) {
        connect(s, &RenderSettings::viewChanged, slider, [s, slider, label, decimals, value](QFlags<ChangeFlag>) {
            const double v = value();
            slider->blockSignals(true);
            slider->setValue(static_cast<int>(std::lround(v * 1000.0)));
            slider->blockSignals(false);
            label->setText(QString::number(v, 'f', decimals));
        });
    };

    auto* particlesBox = new QWidget;
    auto* particlesLay = new QVBoxLayout(particlesBox);
    particlesLay->setContentsMargins(0, 0, 0, 0);
    particlesLay->setSpacing(4);

    auto countRow = sb::createLightSlider(tr("Particle Count"), s->getParticleCount(),
        10.0, 1000.0, 1.0, 0,
        [s](double v) { s->setParticleCount(static_cast<int>(v)); }, "particleCountLabel");
    particlesLay->addWidget(countRow.slider->parentWidget());
    syncParticleSlider(countRow.slider, countRow.valueLabel, 0,
                       [s] { return static_cast<double>(s->getParticleCount()); });

    auto speedRow = sb::createLightSlider(tr("Particle Speed"), s->getParticleSpeed(),
        0.1, 10.0, 0.001, 2, [s](double v) { s->setParticleSpeed(v); }, "particleSpeedLabel");
    particlesLay->addWidget(speedRow.slider->parentWidget());
    syncParticleSlider(speedRow.slider, speedRow.valueLabel, 2,
                       [s] { return static_cast<double>(s->getParticleSpeed()); });

    auto sizeRow = sb::createLightSlider(tr("Particle Size"), s->getParticleSize(),
        1.0, 20.0, 0.1, 1, [s](double v) { s->setParticleSize(v); }, "particleSizeLabel");
    particlesLay->addWidget(sizeRow.slider->parentWidget());
    syncParticleSlider(sizeRow.slider, sizeRow.valueLabel, 1,
                       [s] { return static_cast<double>(s->getParticleSize()); });

    auto* particleAdditiveCb = new QCheckBox(tr("Additive Glow"));
    particleAdditiveCb->setChecked(s->getParticleAdditive());
    connect(particleAdditiveCb, &QCheckBox::toggled, s, &RenderSettings::setParticleAdditive);
    particlesLay->addWidget(particleAdditiveCb);

    optionsLay->addWidget(particlesBox);
    connect(particlesCb, &QCheckBox::toggled, particlesBox, &QWidget::setVisible);
    particlesBox->setVisible(s->getShowParticles());

    tabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);

    // Animation banner: explains why structural controls are locked.
    m_animBanner = new QLabel(tr("Particles playing - pause particles to change integration settings."));
    m_animBanner->setWordWrap(true);
    m_animBanner->setStyleSheet("color: palette(mid); font-style: italic;");
    optionsLay->addWidget(m_animBanner);
    optionsLay->addStretch();

    // Actions live below everything else: they are page-level, not part of
    // any tab, and their position must not move when tabs/toggles change.
    auto* integrateBtn = new QPushButton(tr("Integrate"));
    connect(integrateBtn, &QPushButton::clicked, this, [this, s]() {
        s->backend()->markStreamlineDirty();
        emit viewportUpdateRequested();
    });
    auto* resetBtn = new QPushButton(tr("Reset to Defaults"));
    connect(resetBtn, &QPushButton::clicked, s, [this, s]() {
        // Quiesce the animation first: particles index into the live paths,
        // and the reset below replaces them. Toggling showParticles off
        // also stops MainWindow's particle timer (viewChanged handler).
        const bool wasAnimating = s->getShowParticles() && s->getShowStreamlines();
        if (wasAnimating) s->setShowParticles(false);
        s->resetStreamlineDefaults(); // also quiesces + restores a valid field
        // QScrollArea::setWidget() deletes the previous widget immediately.
        // Take it first: this button is inside that tree and is still emitting
        // clicked, so it must only be deleted once the event loop returns.
        QWidget* old = nullptr;
        if (auto* scroll = findChild<QScrollArea*>()) old = scroll->takeWidget();
        buildContent();
        if (old) old->deleteLater();
        if (wasAnimating) s->setShowParticles(true); // resume on the new paths
    });
    auto* actions = new QHBoxLayout;
    actions->addWidget(integrateBtn);
    actions->addWidget(resetBtn);
    slUi.verticalLayout->addLayout(actions);

    setContent(content);
    updateAnimationGate();
}

void StreamlinesPage::updateAnimationGate() {
    auto* s = S();
    const bool animating = s->getShowParticles() && s->getShowStreamlines();
    for (auto* w : m_structuralControls)
        if (w) w->setEnabled(!animating);
    if (m_animBanner) m_animBanner->setVisible(animating);
}

} // namespace sidebar
