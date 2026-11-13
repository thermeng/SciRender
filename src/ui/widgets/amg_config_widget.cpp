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

AmgConfigWidget::AmgConfigWidget(const cfd::AmgSettings& defaults, QWidget* parent, bool lockNonsymmetric)
    : QWidget(parent), m_lockNonsymmetric(lockNonsymmetric), m_defaults(defaults) {
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
    m_maxCycles->setToolTip(
        "Maximum number of multimesh loops run while the linear-system residual is\n"
        "above Epsilon and the convergence tolerance has not been met.");
    solver->addRow("Maximum Cycle", m_maxCycles);

    m_verbosity = styleCombo(new QComboBox(this));
    m_verbosity->addItems({"None", "Low", "High", "Diagnosis"});
    m_verbosity->setToolTip(
        "None: silent.\n"
        "Low: residual after every multimesh loop.\n"
        "High: also the multigrid series and the equation count per level.\n"
        "Diagnosis: top-level AMG performance information (slower).");
    solver->addRow("Verbosity", m_verbosity);

    m_tolerance = toleranceSpin();
    m_tolerance->setToolTip(
        "Factor by which the linear-system residual must drop before the multimesh\n"
        "loop stops. 0.01 = two orders of magnitude.");
    solver->addRow("Convergence Tolerance", m_tolerance);

    m_cycleType = styleCombo(new QComboBox(this));
    m_cycleType->addItems({"V Cycle", "W Cycle", "F Cycle", "Flexible"});
    m_cycleType->setToolTip(
        "Multimesh loop strategy. F = W followed by V (fixed). Flexible = residual-driven\n"
        "α=0.1 termination, β=0.7 reduction tolerance.");
    solver->addRow("Cycle Type", m_cycleType);

    m_preSweeps = uikit::numeric(new QSpinBox(this));
    m_preSweeps->setRange(0, 50);
    m_preSweeps->setToolTip("Smoothing sweeps before restricting to the coarser level.");
    solver->addRow("Pre-Smooths", m_preSweeps);

    m_postSweeps = uikit::numeric(new QSpinBox(this));
    m_postSweeps->setRange(0, 50);
    m_postSweeps->setToolTip("Smoothing sweeps after prolongating from the coarser level.");
    solver->addRow("Post-Smooths", m_postSweeps);

    m_maxLevels = uikit::numeric(new QSpinBox(this));
    m_maxLevels->setRange(1, 100);
    m_maxLevels->setToolTip("Maximum number of multigrid levels in the sequence.");
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
    m_restrictionTolerance->setToolTip("Restriction tolerance (beta). Go coarser when R_i > beta * R_{i-1} (residual not dropping).");
    flex->addRow("Restriction", m_restrictionTolerance);

    m_prolongationTolerance = uikit::numeric(new QDoubleSpinBox(this));
    m_prolongationTolerance->setRange(0.01, 0.99);
    m_prolongationTolerance->setDecimals(2);
    m_prolongationTolerance->setSingleStep(0.05);
    m_prolongationTolerance->setToolTip("Termination tolerance (alpha). Go finer / terminate when R_i < alpha * R_0 (correction converged).");
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
    m_stagnationFactor->setToolTip("Residual growth ratio that counts as stagnation.");
    robust->addRow("Stagnation Factor", m_stagnationFactor);

    m_stagnationMinIter = uikit::numeric(new QSpinBox(this));
    m_stagnationMinIter->setRange(0, 10000);
    m_stagnationMinIter->setToolTip("Cycles to run before stagnation may trigger.");
    robust->addRow("Stagnation Min Cycles", m_stagnationMinIter);

    m_stagnationLoose = uikit::numeric(new QDoubleSpinBox(this));
    m_stagnationLoose->setRange(1.0, 100.0);
    m_stagnationLoose->setDecimals(2);
    m_stagnationLoose->setSingleStep(0.5);
    m_stagnationLoose->setToolTip("Loosened residual factor accepted once stagnation is detected.");
    robust->addRow("Stagnation Loose Factor", m_stagnationLoose);

    // ── Coarse solve ─────────────────────────────────────────────────────────
    QFormLayout* coarse = makeGroup("Coarse Solve", 1).second;

    m_directSolver = new QCheckBox("Enable The Direct Solver", this);
    m_directSolver->setToolTip(
        "Solve the coarsest level directly instead of relaxing it.");
    coarse->addRow(m_directSolver);

    m_directMaxEquations = uikit::numeric(new QSpinBox(this));
    m_directMaxEquations->setRange(1, 100000);
    m_directMaxEquations->setToolTip(
        "Maximum number of equations handled by the direct coarse solve. Coarsening\n"
        "stops once a level has at most this many unknowns.");
    coarse->addRow("Maximum Direct Solver Equations", m_directMaxEquations);

    m_epsilon = toleranceSpin();
    m_epsilon->setToolTip(
        "Absolute linear-system residual floor. Below this value the multimesh loop\n"
        "stops regardless of the convergence tolerance.");
    coarse->addRow("Epsilon", m_epsilon);

    // ── Smoother and acceleration ────────────────────────────────────────────
    QFormLayout* smoother = makeGroup("Smoother And Acceleration", 2).second;

    m_smoother = styleCombo(new QComboBox(this));
    m_smoother->addItems({"Jacobi", "Gauss-Seidel", "ILU"});
    m_smoother->setToolTip(
        "Jacobi: slow and simple.\n"
        "Gauss-Seidel: improved convergence (default).\n"
        "ILU: more efficient but more costly.");
    smoother->addRow("Smoother", m_smoother);

    m_acceleration = styleCombo(new QComboBox(this));
    m_acceleration->addItems({"None", "Conjugate Gradient", "BiCGStab", "GMRES"});
    m_acceleration->setToolTip(
        "None: the AMG cycle is the solver.\n"
        "Conjugate gradient: SPD systems only (pressure).\n"
        "BiCGStab: nonsymmetric systems.\n"
        "GMRES: general, not necessarily symmetric systems.");
    smoother->addRow("Acceleration Method", m_acceleration);

    m_scaling = styleCombo(new QComboBox(this));
    m_scaling->addItems({"Automatic", "Disabled"});
    m_scaling->setToolTip(
        "Proportional scaling of the coarse-level correction. Applicable to symmetric\n"
        "positive definite matrices; the amgcl backend stores the choice but does not\n"
        "apply scaling.");
    smoother->addRow("Proportional Scaling", m_scaling);

    // ── Aggregation ──────────────────────────────────────────────────────────
    QFormLayout* aggregation = makeGroup("Aggregation", 2).second;

    m_groupSizeControl = styleCombo(new QComboBox(this));
    m_groupSizeControl->addItems({"Automatic", "Manual"});
    m_groupSizeControl->setToolTip(
        "Automatic: STAR Auto = 2 (2D) / 4 (3D) with padding. Manual: set below.");
    aggregation->addRow("Group Size Control", m_groupSizeControl);

    m_groupSize = uikit::numeric(new QSpinBox(this));
    m_groupSize->setRange(1, 16);
    m_groupSize->setToolTip(
        "Number of equations aggregated into one equation on the coarser level.\n"
        "Read-only under Automatic: our pressure and momentum systems are scalar, so\n"
        "the automatic group size is 1.");
    aggregation->addRow("Group Size", m_groupSize);

    // ── Coarsening (expert) ──────────────────────────────────────────────────
    QFormLayout* build = makeGroup("Coarsening (Expert)", 0).second;

    m_coarsening = styleCombo(new QComboBox(this));
    m_coarsening->addItems({"Ruge-Stuben", "Smoothed Aggregation"});
    m_coarsening->setToolTip(
        "Smoothed aggregation is 's choice and is robust for nonsymmetric\n"
        "operators. Ruge-Stuben is kept for comparison.");
    build->addRow("Coarsening", m_coarsening);

    m_strength = uikit::numeric(new QDoubleSpinBox(this));
    m_strength->setRange(0.0, 1.0);
    m_strength->setDecimals(3);
    m_strength->setSingleStep(0.05);
    m_strength->setToolTip("Relative drop below which a matrix entry is not a strong connection.");
    build->addRow("Strength Threshold", m_strength);

    m_truncation = uikit::numeric(new QDoubleSpinBox(this));
    m_truncation->setRange(0.0, 1.0);
    m_truncation->setDecimals(3);
    m_truncation->setSingleStep(0.05);
    m_truncation->setToolTip(
        "Interpolation weights smaller than this fraction of the row max are dropped\n"
        "(Ruge-Stuben only).");
    build->addRow("Truncation Threshold", m_truncation);

    m_minCoarse = uikit::numeric(new QSpinBox(this));
    m_minCoarse->setRange(1, 1000000);
    m_minCoarse->setToolTip("Stop coarsening once a level has this few unknowns.");
    build->addRow("Min Coarse Size", m_minCoarse);

    m_nonsymmetric = new QCheckBox("Non-Symmetric Operator (Momentum)", this);
    m_nonsymmetric->setToolTip(
        "Forces smoothed-aggregation coarsening, the robust choice for the\n"
        "convection-diffusion momentum operator.");
    build->addRow("Non-Symmetric", m_nonsymmetric);

    if (m_lockNonsymmetric) {
        m_nonsymmetric->setChecked(false);
        m_nonsymmetric->setEnabled(false);
    }

    // ── Change notifications ─────────────────────────────────────────────────
    for (QSpinBox* spin : {m_maxCycles, m_preSweeps, m_postSweeps, m_maxLevels,
                           m_directMaxEquations, m_groupSize, m_minCoarse,
                           m_stagnationMinIter, m_flexSweeps}) {
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { emit changed(); });
    }
    for (QDoubleSpinBox* spin : {m_tolerance, m_epsilon,
                                 m_strength, m_truncation, m_stagnationFactor,
                                 m_stagnationLoose, m_restrictionTolerance,
                                 m_prolongationTolerance}) {
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this](double) { emit changed(); });
    }
    for (QComboBox* combo : {m_verbosity, m_cycleType, m_smoother, m_acceleration,
                             m_scaling, m_groupSizeControl, m_coarsening}) {
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int) { syncDependencies(); emit changed(); });
    }
    connect(m_directSolver, &QCheckBox::toggled, this, [this](bool) {
        syncDependencies();
        emit changed();
    });
    connect(m_nonsymmetric, &QCheckBox::toggled, this, [this](bool) {
        syncDependencies();
        emit changed();
    });

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
    m_directMaxEquations->setEnabled(m_directSolver->isChecked());

    const bool manualGroup =
        m_groupSizeControl->currentIndex() == static_cast<int>(cfd::AmgSettings::GroupSizeControl::Manual);
    const bool aggregation =
        m_coarsening->currentIndex() == static_cast<int>(cfd::AmgSettings::Coarsening::SmoothedAggregation) ||
        m_nonsymmetric->isChecked();
    m_groupSizeControl->setEnabled(aggregation);
    if (manualGroup && aggregation) {
        m_groupSize->setEnabled(true);
        m_groupSize->setValue(m_manualGroupSize);
    } else {
        if (m_groupSize->isEnabled()) m_manualGroupSize = m_groupSize->value();
        // Automatic (or Ruge-Stuben, which has no aggregation) resolves to the
        // scalar group size. Show the value the solver will use, read-only.
        m_groupSize->setEnabled(false);
        m_groupSize->setValue(1);
    }

    // Aggregation coarsening is forced for a nonsymmetric operator, so the
    // coarsening choice is not the user's to make in that case.
    m_coarsening->setEnabled(!m_nonsymmetric->isChecked());
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
    m_preSweeps->setValue(s.preSweeps);
    m_postSweeps->setValue(s.postSweeps);
    m_maxLevels->setValue(s.maxLevels);

    m_directSolver->setChecked(s.directCoarse);
    m_directMaxEquations->setValue(s.coarseEnough);
    m_epsilon->setValue(s.epsilon);

    m_smoother->setCurrentIndex(static_cast<int>(s.smoother));
    m_acceleration->setCurrentIndex(static_cast<int>(s.acceleration));
    m_scaling->setCurrentIndex(static_cast<int>(s.scaling));

    m_groupSizeControl->setCurrentIndex(static_cast<int>(s.groupSizeControl));
    m_manualGroupSize = (s.blockSize > 0) ? s.blockSize : 1;
    m_groupSize->setValue(m_manualGroupSize);

    m_coarsening->setCurrentIndex(static_cast<int>(s.coarsening));
    m_strength->setValue(s.strengthThreshold);
    m_truncation->setValue(s.truncThreshold);
    m_minCoarse->setValue(s.minCoarseSize);
    m_nonsymmetric->setChecked(m_lockNonsymmetric ? false : s.nonsymmetric);

    m_stagnationFactor->setValue(s.stagnationFactor);
    m_stagnationMinIter->setValue(s.stagnationMinIter);
    m_stagnationLoose->setValue(s.stagnationLooseFactor);

    m_restrictionTolerance->setValue(s.flexBeta);
    m_prolongationTolerance->setValue(s.flexAlpha);
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
    s.preSweeps = m_preSweeps->value();
    s.postSweeps = m_postSweeps->value();
    s.maxLevels = m_maxLevels->value();

    s.directCoarse = m_directSolver->isChecked();
    s.coarseEnough = m_directMaxEquations->value();
    s.epsilon = m_epsilon->value();

    s.smoother = static_cast<cfd::AmgSettings::Smoother>(m_smoother->currentIndex());
    s.acceleration = static_cast<cfd::AmgSettings::Acceleration>(m_acceleration->currentIndex());
    s.scaling = static_cast<cfd::AmgSettings::ProportionalScaling>(m_scaling->currentIndex());

    s.groupSizeControl =
        static_cast<cfd::AmgSettings::GroupSizeControl>(m_groupSizeControl->currentIndex());
    // Under Automatic the solver uses 1 for a scalar system; store that rather
    // than whatever the disabled spin box last displayed.
    if (s.groupSizeControl == cfd::AmgSettings::GroupSizeControl::Manual) {
        s.blockSize = m_groupSize->value();
    } else {
        s.blockSize = 1;
    }

    s.coarsening = static_cast<cfd::AmgSettings::Coarsening>(m_coarsening->currentIndex());
    s.strengthThreshold = m_strength->value();
    s.truncThreshold = m_truncation->value();
    s.minCoarseSize = m_minCoarse->value();
    s.nonsymmetric = m_lockNonsymmetric ? false : m_nonsymmetric->isChecked();

    s.stagnationFactor = m_stagnationFactor->value();
    s.stagnationMinIter = m_stagnationMinIter->value();
    s.stagnationLooseFactor = m_stagnationLoose->value();

    s.flexBeta = m_restrictionTolerance->value();
    s.flexAlpha = m_prolongationTolerance->value();
    s.flexMaxSweeps = m_flexSweeps->value();
    s.flexMaxFineRelaxations = m_maxFineRelaxations->value();
    s.flexMaxCoarseRelaxations = m_maxCoarseRelaxations->value();
    return s;
}
