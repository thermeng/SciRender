#include "ui/widgets/amg_config_widget.h"

#include "ui/widgets/scientific_spin_box.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {

// Field widths are fixed rather than expanding: a form of 18 rows stretched to
// the dialog width buries the values under a field of dead space. These match
// the two natural field shapes, a short number and a named choice.
constexpr int kSpinWidth = 100;
constexpr int kComboWidth = 150;

template <typename Spin>
Spin* styleSpin(Spin* spin) {
    spin->setFixedWidth(kSpinWidth);
    // Monospaced digits so numbers line up down the column.
    QFont f("JetBrains Mono");
    f.setStyleHint(QFont::Monospace);
    f.setPixelSize(11);
    spin->setFont(f);
    return spin;
}

QComboBox* makeCombo(const QStringList& items, const QStringList& tips, QWidget* parent) {
    auto* combo = new QComboBox(parent);
    combo->addItems(items);
    for (int i = 0; i < tips.size() && i < items.size(); ++i)
        combo->setItemData(i, tips[i], Qt::ToolTipRole);
    combo->setFixedWidth(kComboWidth);
    return combo;
}

QLabel* noteLabel(const QString& text) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setStyleSheet("color: palette(mid);");
    QFont f = label->font();
    f.setPixelSize(10);
    label->setFont(f);
    return label;
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

    // Two columns of titled groups. Eighteen knobs in one column is a form
    // nobody reads; grouped and split it scans like the Fluent AMG panel.
    // Right-aligned labels give the fields a single clean edge to start from.
    auto makeGroup = [](QVBoxLayout* column, const QString& title) {
        auto* box = new QGroupBox(title);
        auto* form = new QFormLayout(box);
        form->setContentsMargins(12, 8, 12, 12);
        form->setSpacing(5);
        form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
        form->setRowWrapPolicy(QFormLayout::DontWrapRows);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
        column->addWidget(box);
        return form;
    };

    auto* columns = new QHBoxLayout;
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(12);

    auto* leftCol = new QVBoxLayout;
    leftCol->setSpacing(10);
    auto* rightCol = new QVBoxLayout;
    rightCol->setSpacing(10);
    columns->addLayout(leftCol, 1);
    columns->addLayout(rightCol, 1);
    root->addLayout(columns);

    // ---- Left: how the hierarchy is built --------------------------------
    // Coarsening termination (max levels / min coarse size) belongs with the
    // rest of the coarsening rules, not in a group of its own.
    QFormLayout* build = makeGroup(leftCol, "Coarsening");

    m_coarsening = makeCombo({"RS (Ruge-Stuben)", "PMIS"},
                             {"Classic Ruge-Stuben coarsening; robust default.",
                              "Parallel maximum independent set coarsening; cheaper setup on large meshes."},
                             this);
    build->addRow("Coarsening", m_coarsening);

    m_interpolation = makeCombo({"Direct", "Extended"},
                                {"Direct interpolation; cheaper, adequate for M-matrices.",
                                 "Extended (distance-two) interpolation; stronger, more fill-in."},
                                this);
    build->addRow("Interpolation", m_interpolation);

    m_strength = styleSpin(new QDoubleSpinBox(this));
    m_strength->setRange(0.0, 1.0);
    m_strength->setDecimals(3);
    m_strength->setSingleStep(0.05);
    m_strength->setToolTip("Relative drop below which a matrix entry is not a strong connection.");
    build->addRow("Strength threshold", m_strength);

    m_truncation = styleSpin(new QDoubleSpinBox(this));
    m_truncation->setRange(0.0, 1.0);
    m_truncation->setDecimals(3);
    m_truncation->setSingleStep(0.05);
    m_truncation->setToolTip("Interpolation weights smaller than this fraction of the row max are dropped.");
    build->addRow("Truncation threshold", m_truncation);

    m_galerkinDrop = styleSpin(new ScientificDoubleSpinBox(this));
    m_galerkinDrop->setRange(0.0, 1e-1);
    // Decimals must reach the smallest representable range end (1e-16 for
    // tolerance below); the display itself is scientific via the override.
    m_galerkinDrop->setDecimals(18);
    m_galerkinDrop->setSingleStep(1e-12);
    m_galerkinDrop->setToolTip("Magnitude below which Galerkin coarse-operator entries are dropped.");
    build->addRow("Galerkin drop tol", m_galerkinDrop);

    m_maxLevels = styleSpin(new QSpinBox(this));
    m_maxLevels->setRange(1, 100);
    m_maxLevels->setToolTip("Cap on the number of grid levels. The hierarchy also stops at min coarse size.");
    build->addRow("Max levels", m_maxLevels);

    m_minCoarse = styleSpin(new QSpinBox(this));
    m_minCoarse->setRange(1, 1000000);
    m_minCoarse->setToolTip("Stop coarsening once a level has this few unknowns. The coarsest level is solved directly.");
    build->addRow("Min coarse size", m_minCoarse);

    // ---- Left: standalone solve termination ------------------------------
    QFormLayout* solve = makeGroup(leftCol, "Standalone solve");

    m_maxIterations = styleSpin(new QSpinBox(this));
    m_maxIterations->setRange(1, 1000000);
    m_maxIterations->setToolTip("Iteration cap for a standalone AMG solve.");
    solve->addRow("Max iterations", m_maxIterations);

    m_tolerance = styleSpin(new ScientificDoubleSpinBox(this));
    m_tolerance->setRange(1e-16, 1e-1);
    m_tolerance->setDecimals(18);
    m_tolerance->setSingleStep(1e-9);
    m_tolerance->setToolTip("Relative residual target for a standalone AMG solve.");
    solve->addRow("Tolerance", m_tolerance);

    solve->addRow(noteLabel("Used only when AMG solves the system on its own. As a Krylov "
                            "preconditioner the outer solver's tolerance governs."));

    leftCol->addStretch(1);

    // ---- Right: how a level is relaxed -----------------------------------
    QFormLayout* cycle = makeGroup(rightCol, "Cycle & smoother");

    m_cycle = makeCombo({"V-cycle", "F-cycle"},
                        {"One visit per level; cheapest cycle.",
                         "Extra coarse-level visits; stronger per cycle, more work."},
                        this);
    cycle->addRow("Cycle", m_cycle);

    m_smoother = makeCombo({"Symmetric Gauss-Seidel", "Forward Gauss-Seidel", "Forward SOR"},
                           {"Forward + backward sweeps. Only guaranteed for SPD (pressure) operators.",
                            "Forward sweep only; required for the nonsymmetric momentum operator.",
                            "Forward sweep with relaxation; omega must stay below 1 for nonsymmetric operators."},
                           this);
    cycle->addRow("Smoother", m_smoother);

    m_smootherOmega = styleSpin(new QDoubleSpinBox(this));
    m_smootherOmega->setRange(0.01, 2.0);
    m_smootherOmega->setDecimals(3);
    m_smootherOmega->setSingleStep(0.05);
    m_smootherOmega->setToolTip("Relaxation for Forward SOR. 1.0 degenerates to plain Gauss-Seidel.");
    cycle->addRow("SOR relaxation", m_smootherOmega);

    m_preSweeps = styleSpin(new QSpinBox(this));
    m_preSweeps->setRange(0, 20);
    m_preSweeps->setToolTip("Smoother sweeps before restricting to the coarse grid.");
    cycle->addRow("Pre-sweeps", m_preSweeps);

    m_postSweeps = styleSpin(new QSpinBox(this));
    m_postSweeps->setRange(0, 20);
    m_postSweeps->setToolTip("Smoother sweeps after prolongating from the coarse grid.");
    cycle->addRow("Post-sweeps", m_postSweeps);

    // ---- Right: robustness guards ----------------------------------------
    QFormLayout* robust = makeGroup(rightCol, "Robustness");

    m_rngSeed = styleSpin(new QSpinBox(this));
    m_rngSeed->setRange(0, 1000000);
    m_rngSeed->setSpecialValueText("random");
    m_rngSeed->setToolTip("Seed for nondeterministic coarsening steps. 0 draws a fresh seed each run.");
    robust->addRow("RNG seed", m_rngSeed);

    m_stagnationFactor = styleSpin(new QDoubleSpinBox(this));
    m_stagnationFactor->setRange(1.0, 10.0);
    m_stagnationFactor->setDecimals(2);
    m_stagnationFactor->setSingleStep(0.05);
    m_stagnationFactor->setToolTip("Residual growth ratio that counts as stagnation.");
    robust->addRow("Stagnation factor", m_stagnationFactor);

    m_stagnationMinIter = styleSpin(new QSpinBox(this));
    m_stagnationMinIter->setRange(0, 10000);
    m_stagnationMinIter->setToolTip("Iterations to run before stagnation is allowed to trigger.");
    robust->addRow("Stagnation min iters", m_stagnationMinIter);

    m_stagnationLoose = styleSpin(new QDoubleSpinBox(this));
    m_stagnationLoose->setRange(1.0, 100.0);
    m_stagnationLoose->setDecimals(2);
    m_stagnationLoose->setSingleStep(0.5);
    m_stagnationLoose->setToolTip("Loosened residual factor accepted once stagnation is detected.");
    robust->addRow("Stagnation loose factor", m_stagnationLoose);

    rightCol->addStretch(1);

    // ---- Wiring ----------------------------------------------------------
    for (QComboBox* combo : {m_coarsening, m_interpolation, m_cycle, m_smoother}) {
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
            syncDependencies();
            emit changed();
        });
    }
    for (QSpinBox* spin : {m_maxLevels, m_minCoarse, m_preSweeps, m_postSweeps, m_maxIterations,
                           m_rngSeed, m_stagnationMinIter}) {
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { emit changed(); });
    }
    for (QDoubleSpinBox* spin : {m_smootherOmega, m_strength, m_truncation, m_galerkinDrop,
                                 m_tolerance, m_stagnationFactor, m_stagnationLoose}) {
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this](double) { emit changed(); });
    }

    syncDependencies();
}

void AmgConfigWidget::syncDependencies() {
    // SOR relaxation only means anything for the SOR smoother.
    const bool isSor = m_smoother->currentIndex() == int(cfd::AmgSettings::SmootherType::ForwardSOR);
    m_smootherOmega->setEnabled(isSor);
}

void AmgConfigWidget::resetToDefaults() {
    setConfig(m_defaults);
    emit changed();
}

void AmgConfigWidget::setConfig(const cfd::AmgSettings& s) {
    m_coarsening->setCurrentIndex(int(s.coarseningType));
    m_interpolation->setCurrentIndex(int(s.interpolationType));
    m_cycle->setCurrentIndex(int(s.cycleType));
    m_smoother->setCurrentIndex(int(s.smootherType));
    m_smootherOmega->setValue(s.smootherOmega);
    m_strength->setValue(s.strengthThreshold);
    m_truncation->setValue(s.truncThreshold);
    m_galerkinDrop->setValue(s.galerkinDropTol);
    m_maxLevels->setValue(s.maxLevels);
    m_minCoarse->setValue(s.minCoarseSize);
    m_preSweeps->setValue(s.preSweeps);
    m_postSweeps->setValue(s.postSweeps);
    m_maxIterations->setValue(s.maxIterations);
    m_tolerance->setValue(s.tolerance);
    m_rngSeed->setValue(s.rngSeed);
    m_stagnationFactor->setValue(s.stagnationFactor);
    m_stagnationMinIter->setValue(s.stagnationMinIter);
    m_stagnationLoose->setValue(s.stagnationLooseFactor);
    syncDependencies();
}

cfd::AmgSettings AmgConfigWidget::config() const {
    // The combo item order is the enum order declared in cfd::AmgSettings, so
    // the current index maps straight across.
    cfd::AmgSettings s;
    s.coarseningType = static_cast<cfd::AmgSettings::CoarseningType>(m_coarsening->currentIndex());
    s.interpolationType = static_cast<cfd::AmgSettings::InterpolationType>(m_interpolation->currentIndex());
    s.cycleType = static_cast<cfd::AmgSettings::CycleType>(m_cycle->currentIndex());
    s.smootherType = static_cast<cfd::AmgSettings::SmootherType>(m_smoother->currentIndex());
    s.smootherOmega = m_smootherOmega->value();
    s.strengthThreshold = m_strength->value();
    s.truncThreshold = m_truncation->value();
    s.galerkinDropTol = m_galerkinDrop->value();
    s.maxLevels = m_maxLevels->value();
    s.minCoarseSize = m_minCoarse->value();
    s.preSweeps = m_preSweeps->value();
    s.postSweeps = m_postSweeps->value();
    s.maxIterations = m_maxIterations->value();
    s.tolerance = m_tolerance->value();
    s.rngSeed = m_rngSeed->value();
    s.stagnationFactor = m_stagnationFactor->value();
    s.stagnationMinIter = m_stagnationMinIter->value();
    s.stagnationLooseFactor = m_stagnationLoose->value();
    return s;
}
