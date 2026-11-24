#include "ui/widgets/amg_config_widget.h"

#include "ui/uikit.h"
#include "ui/widgets/scientific_spin_box.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {

constexpr int kGroupColumns = 3;
constexpr int kGroupSpacing = 8;

QComboBox* styleCombo(QComboBox* combo) {
    combo->setFixedWidth(uikit::kComboWidth);
    return combo;
}

// A scientific-entry spin box for the two tolerances the AMG backend writes in
// exponent form (Convergence Tolerance, Epsilon).
ScientificDoubleSpinBox* toleranceSpin() {
    auto* spin = uikit::numeric(new ScientificDoubleSpinBox);
    spin->setRange(1e-30, 1.0);
    spin->setDecimals(30);
    spin->setSingleStep(1e-9);
    return spin;
}

} // namespace

AmgConfigWidget::AmgConfigWidget(const cfd::AmgSettings& defaults, QWidget* parent)
    : QWidget(parent), m_defaults(defaults) {
    buildUi();
    setConfig(defaults);
}

void AmgConfigWidget::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // The groups live on one grid instead of nested per-column layouts. The grid
    // is what decides how many groups sit side by side; each column keeps its own
    // next-row counter, so hiding one group leaves no hole under the others.
    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(kGroupSpacing);
    grid->setVerticalSpacing(kGroupSpacing);
    root->addLayout(grid);

    // Columns share the page width, so no single group decides how wide it is.
    for (int column = 0; column < kGroupColumns; ++column) {
        grid->setColumnStretch(column, 1);
    }
    int nextRow[kGroupColumns] = {0, 0, 0};

    // Returns the box and its form: only the box is ever hidden, but the caller
    // needs both, and the pairs keep the call sites uniform.
    auto makeGroup = [grid, &nextRow](const QString& title, int column) {
        QFormLayout* form = nullptr;
        auto* box = uikit::group(title, &form);
        grid->addWidget(box, nextRow[column]++, column, Qt::AlignLeft | Qt::AlignTop);
        return std::make_pair(box, form);
    };

    // ── AMG linear solver ────────────────────────────────────────────────────
    auto solverGroup = makeGroup("AMG Linear Solver", 0);
    QFormLayout* solver = solverGroup.second;

    m_maxCycles = uikit::numeric(new QSpinBox(this));
    m_maxCycles->setRange(1, 100000);
    solver->addRow("Maximum Cycle", m_maxCycles);

    m_verbosity = styleCombo(new QComboBox(this));
    m_verbosity->addItems({"None", "Low", "High", "Diagnosis"});
    solver->addRow("Verbosity", m_verbosity);

    m_tolerance = toleranceSpin();
    solver->addRow("Convergence Tolerance", m_tolerance);

    m_cycleType = styleCombo(new QComboBox(this));
    m_cycleType->addItems({"V Cycle", "W Cycle", "F Cycle", "Flexible"});
    solver->addRow("Cycle Type", m_cycleType);

    m_backend = styleCombo(new QComboBox(this));
    m_backend->addItems({"BoomerAMG", "Cached Aggregation"});
    solver->addRow("Backend", m_backend);

    m_preSweeps = uikit::numeric(new QSpinBox(this));
    m_preSweeps->setRange(0, 50);
    solver->addRow("Pre-Smooths", m_preSweeps);

    m_postSweeps = uikit::numeric(new QSpinBox(this));
    m_postSweeps->setRange(0, 50);
    solver->addRow("Post-Smooths", m_postSweeps);

    m_maxLevels = uikit::numeric(new QSpinBox(this));
    m_maxLevels->setRange(1, 100);
    solver->addRow("Maximum Number Of Levels", m_maxLevels);

    // ── Flexible Cycle ──────────────────────────────────────
    // Hidden until the cycle type asks for it: a flexible cycle is the only
    // consumer of these five rows, and it is off by default.
    auto flexGroup = makeGroup("Flexible Cycle", 1);
    m_flexGroup = flexGroup.first;
    QFormLayout* flex = flexGroup.second;
    m_restrictionTolerance = uikit::numeric(new QDoubleSpinBox(this));
    m_restrictionTolerance->setRange(0.01, 0.99);
    m_restrictionTolerance->setDecimals(2);
    m_restrictionTolerance->setSingleStep(0.05);
    flex->addRow("Restriction", m_restrictionTolerance);

    m_prolongationTolerance = uikit::numeric(new QDoubleSpinBox(this));
    m_prolongationTolerance->setRange(0.01, 0.99);
    m_prolongationTolerance->setDecimals(2);
    m_prolongationTolerance->setSingleStep(0.05);
    flex->addRow("Termination", m_prolongationTolerance);

    m_flexSweeps = uikit::numeric(new QSpinBox(this));
    m_flexSweeps->setRange(1, 100);
    flex->addRow("Sweeps", m_flexSweeps);

    m_maxFineRelaxations = uikit::numeric(new QSpinBox(this));
    m_maxFineRelaxations->setRange(1, 1000);
    flex->addRow("Max Fine Relaxations", m_maxFineRelaxations);

    m_maxCoarseRelaxations = uikit::numeric(new QSpinBox(this));
    m_maxCoarseRelaxations->setRange(1, 1000);
    flex->addRow("Max Coarse Relaxations", m_maxCoarseRelaxations);

    // ── Robustness (expert) ──────────────────────────────────────────────────
    QFormLayout* robust = makeGroup("Robustness (Expert)", 0).second;

    m_stagnationFactor = uikit::numeric(new QDoubleSpinBox(this));
    m_stagnationFactor->setRange(1.0, 10.0);
    m_stagnationFactor->setDecimals(2);
    m_stagnationFactor->setSingleStep(0.05);
    robust->addRow("Stagnation Factor", m_stagnationFactor);

    m_stagnationMinIter = uikit::numeric(new QSpinBox(this));
    m_stagnationMinIter->setRange(0, 10000);
    robust->addRow("Stagnation Min Cycles", m_stagnationMinIter);

    m_stagnationLoose = uikit::numeric(new QDoubleSpinBox(this));
    m_stagnationLoose->setRange(1.0, 100.0);
    m_stagnationLoose->setDecimals(2);
    m_stagnationLoose->setSingleStep(0.5);
    robust->addRow("Stagnation Loose Factor", m_stagnationLoose);

    // ── Coarse solve ─────────────────────────────────────────────────────────
    QFormLayout* coarse = makeGroup("Coarse Solve", 1).second;

    m_epsilon = toleranceSpin();
    coarse->addRow("Epsilon", m_epsilon);

    // ── Smoother and acceleration ────────────────────────────────────────────
    QFormLayout* smoother = makeGroup("Smoother And Acceleration", 2).second;

    m_smoother = styleCombo(new QComboBox(this));
    m_smoother->addItems({"Jacobi", "Gauss-Seidel", "ILU"});
    smoother->addRow("Smoother", m_smoother);

    m_acceleration = styleCombo(new QComboBox(this));
    m_acceleration->addItems({"None", "Conjugate Gradient", "BiCGStab", "GMRES"});
    smoother->addRow("Acceleration Method", m_acceleration);

    m_scaling = styleCombo(new QComboBox(this));
    m_scaling->addItems({"Automatic", "Disabled"});
    smoother->addRow("Proportional Scaling", m_scaling);

    // ── Aggregation ──────────────────────────────────────────────────────────
    QFormLayout* aggregation = makeGroup("Aggregation", 2).second;

    m_groupSizeControl = styleCombo(new QComboBox(this));
    m_groupSizeControl->addItems({"Automatic", "Manual"});
    aggregation->addRow("Group Size Control", m_groupSizeControl);

    m_groupSize = uikit::numeric(new QSpinBox(this));
    m_groupSize->setRange(1, 16);
    aggregation->addRow("Group Size", m_groupSize);

    // Smoothed Aggregation prolongation: smooth the tentative piecewise-constant
    // prolongation so the coarse grid can represent smooth error modes.
    m_smoothedProlongation = new QCheckBox("Smoothed Prolongation", this);
    aggregation->addRow(m_smoothedProlongation);

    m_saOmega = uikit::numeric(new QDoubleSpinBox(this));
    m_saOmega->setRange(0.1, 2.0);
    m_saOmega->setDecimals(3);
    m_saOmega->setSingleStep(0.1);
    m_saOmega->setValue(2.0 / 3.0);
    aggregation->addRow("SA Omega", m_saOmega);

    m_saDegree = uikit::numeric(new QSpinBox(this));
    m_saDegree->setRange(1, 10);
    m_saDegree->setValue(1);
    aggregation->addRow("SA Degree", m_saDegree);

    // ── Coarsening (expert) ──────────────────────────────────────────────────
    QFormLayout* build = makeGroup("Coarsening (Expert)", 0).second;

    m_coarsening = styleCombo(new QComboBox(this));
    m_coarsening->addItems({"Ruge-Stuben", "Smoothed Aggregation"});
    build->addRow("Coarsening", m_coarsening);

    m_strength = uikit::numeric(new QDoubleSpinBox(this));
    m_strength->setRange(0.0, 1.0);
    m_strength->setDecimals(3);
    m_strength->setSingleStep(0.05);
    build->addRow("Strength Threshold", m_strength);

    m_truncation = uikit::numeric(new QDoubleSpinBox(this));
    m_truncation->setRange(0.0, 1.0);
    m_truncation->setDecimals(3);
    m_truncation->setSingleStep(0.05);
    build->addRow("Truncation Threshold", m_truncation);

    m_minCoarse = uikit::numeric(new QSpinBox(this));
    m_minCoarse->setRange(1, 1000000);
    build->addRow("Min Coarse Size", m_minCoarse);

    // ── Change notifications ─────────────────────────────────────────────────
    for (QSpinBox* spin : {m_maxCycles, m_preSweeps, m_postSweeps, m_maxLevels,
                           m_groupSize, m_minCoarse, m_stagnationMinIter,
                           m_flexSweeps, m_saDegree}) {
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { emit changed(); });
    }
    for (QDoubleSpinBox* spin : {m_tolerance, m_epsilon,
                                 m_strength, m_truncation, m_stagnationFactor,
                                 m_stagnationLoose, m_restrictionTolerance,
                                 m_prolongationTolerance, m_saOmega}) {
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this](double) { emit changed(); });
    }
    connect(m_smoothedProlongation, &QCheckBox::toggled, this, [this](bool) { syncDependencies(); emit changed(); });
    for (QComboBox* combo : {m_verbosity, m_cycleType, m_backend, m_smoother, m_acceleration,
                             m_scaling, m_groupSizeControl, m_coarsening}) {
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int) { syncDependencies(); emit changed(); });
    }

    syncDependencies();
}

void AmgConfigWidget::syncDependencies() {
    const bool flex = m_cycleType->currentIndex() == static_cast<int>(cfd::AmgSettings::CycleType::Flexible);
    // A flexible cycle is the only reader of these rows. Hiding the group (rather
    // than only disabling it) is what keeps the page compact by default; the
    // values are still read by config(), so nothing is lost while hidden.
    m_flexGroup->setVisible(flex);
    m_restrictionTolerance->setEnabled(flex);
    m_prolongationTolerance->setEnabled(flex);
    m_flexSweeps->setEnabled(flex);
    m_maxFineRelaxations->setEnabled(flex);
    m_maxCoarseRelaxations->setEnabled(flex);

    const bool manualGroup =
        m_groupSizeControl->currentIndex() == static_cast<int>(cfd::AmgSettings::GroupSizeControl::Manual);
    const bool isBoomerAMG =
        m_backend->currentIndex() == static_cast<int>(cfd::AmgSettings::Backend::BoomerAMG);
    // Smoothed Aggregation prolongation is only meaningful for the cached
    // aggregation backend; BoomerAMG uses HMIS coarsening instead.
    const bool aggregation =
        !isBoomerAMG &&
        m_coarsening->currentIndex() == static_cast<int>(cfd::AmgSettings::Coarsening::SmoothedAggregation);
    m_groupSizeControl->setEnabled(aggregation);
    m_smoothedProlongation->setEnabled(aggregation);
    m_saOmega->setEnabled(aggregation && m_smoothedProlongation->isChecked());
    m_saDegree->setEnabled(aggregation && m_smoothedProlongation->isChecked());
    // Strength / truncation thresholds are read only by BoomerAMG (Hypre). The
    // cached aggregation backend derives its coarse operators from the Galerkin
    // product over a fixed sparsity pattern and ignores both.
    m_strength->setEnabled(isBoomerAMG);
    m_truncation->setEnabled(isBoomerAMG);
    if (manualGroup && aggregation) {
        m_groupSize->setEnabled(true);
        m_groupSize->setValue(m_manualGroupSize);
    } else {
        if (m_groupSize->isEnabled()) m_manualGroupSize = m_groupSize->value();
        // Automatic resolves to the STAR auto group size: 2 (2D) / 4 (3D),
        // resolved from the mesh dimension.
        m_groupSize->setEnabled(false);
        m_groupSize->setValue((m_dim >= 3) ? 4 : 2);
    }
}

void AmgConfigWidget::setDimension(int dim) {
    m_dim = dim;
    syncDependencies();
}

void AmgConfigWidget::resetToDefaults() {
    setConfig(m_defaults);
    emit changed();
}

void AmgConfigWidget::setConfig(const cfd::AmgSettings& s) {
    m_maxCycles->setValue(s.maxIterations);
    m_verbosity->setCurrentIndex(static_cast<int>(s.verbosity));
    m_tolerance->setValue(s.tolerance);
    m_cycleType->setCurrentIndex(static_cast<int>(s.cycleType));
    m_backend->setCurrentIndex(static_cast<int>(s.backend));
    m_preSweeps->setValue(s.preSweeps);
    m_postSweeps->setValue(s.postSweeps);
    m_maxLevels->setValue(s.maxLevels);

    m_epsilon->setValue(s.epsilon);

    m_smoother->setCurrentIndex(static_cast<int>(s.smoother));
    m_acceleration->setCurrentIndex(static_cast<int>(s.acceleration));
    m_scaling->setCurrentIndex(static_cast<int>(s.scaling));

    m_groupSizeControl->setCurrentIndex(static_cast<int>(s.groupSizeControl));
    m_manualGroupSize = (s.blockSize > 0) ? s.blockSize : 1;
    m_groupSize->setValue(m_manualGroupSize);

    m_smoothedProlongation->setChecked(s.smoothedProlongation);
    m_saOmega->setValue(s.saOmega);
    m_saDegree->setValue(s.saDegree);

    m_coarsening->setCurrentIndex(static_cast<int>(s.coarsening));
    m_strength->setValue(s.strengthThreshold);
    m_truncation->setValue(s.truncThreshold);
    m_minCoarse->setValue(s.minCoarseSize);

    m_stagnationFactor->setValue(s.stagnationFactor);
    m_stagnationMinIter->setValue(s.stagnationMinIter);
    m_stagnationLoose->setValue(s.stagnationLooseFactor);

    m_restrictionTolerance->setValue(s.flexAlpha);
    m_prolongationTolerance->setValue(s.flexBeta);
    m_flexSweeps->setValue(s.flexMaxSweeps);
    m_maxFineRelaxations->setValue(s.flexMaxFineRelaxations);
    m_maxCoarseRelaxations->setValue(s.flexMaxCoarseRelaxations);

    syncDependencies();
}

cfd::AmgSettings AmgConfigWidget::config() const {
    cfd::AmgSettings s;
    s.maxIterations = m_maxCycles->value();
    s.verbosity = static_cast<cfd::AmgSettings::Verbosity>(m_verbosity->currentIndex());
    s.tolerance = m_tolerance->value();
    s.cycleType = static_cast<cfd::AmgSettings::CycleType>(m_cycleType->currentIndex());
    s.backend = static_cast<cfd::AmgSettings::Backend>(m_backend->currentIndex());
    s.preSweeps = m_preSweeps->value();
    s.postSweeps = m_postSweeps->value();
    s.maxLevels = m_maxLevels->value();

    s.epsilon = m_epsilon->value();

    s.smoother = static_cast<cfd::AmgSettings::Smoother>(m_smoother->currentIndex());
    s.acceleration = static_cast<cfd::AmgSettings::Acceleration>(m_acceleration->currentIndex());
    s.scaling = static_cast<cfd::AmgSettings::ProportionalScaling>(m_scaling->currentIndex());

    s.groupSizeControl =
        static_cast<cfd::AmgSettings::GroupSizeControl>(m_groupSizeControl->currentIndex());
    // Manual: store whatever the user entered. Automatic: store STAR Auto = 2
    // (2D) / 4 (3D) so the case file round-trips what the UI displays.
    if (s.groupSizeControl == cfd::AmgSettings::GroupSizeControl::Manual) {
        s.blockSize = m_groupSize->value();
    } else {
        s.blockSize = (m_dim >= 3) ? 4 : 2; // STAR Auto: 2 (2D) / 4 (3D)
    }

    s.smoothedProlongation = m_smoothedProlongation->isChecked();
    s.saOmega = m_saOmega->value();
    s.saDegree = m_saDegree->value();

    s.coarsening = static_cast<cfd::AmgSettings::Coarsening>(m_coarsening->currentIndex());
    s.strengthThreshold = m_strength->value();
    s.truncThreshold = m_truncation->value();
    s.minCoarseSize = m_minCoarse->value();

    s.stagnationFactor = m_stagnationFactor->value();
    s.stagnationMinIter = m_stagnationMinIter->value();
    s.stagnationLooseFactor = m_stagnationLoose->value();

    s.flexAlpha = m_restrictionTolerance->value();
    s.flexBeta = m_prolongationTolerance->value();
    s.flexMaxSweeps = m_flexSweeps->value();
    s.flexMaxFineRelaxations = m_maxFineRelaxations->value();
    s.flexMaxCoarseRelaxations = m_maxCoarseRelaxations->value();
    return s;
}
