#include "ui/cfd_advanced_dialog.h"

#include "ui/uikit.h"

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace sidebar {

namespace {

// The table's columns: cycle type, termination, restriction, AMG method.
void readAmgRow(const cfd::AmgSettings& s, QTableWidget* table, int row) {
    auto* cycle = qobject_cast<QComboBox*>(table->cellWidget(row, 0));
    auto* term = qobject_cast<QDoubleSpinBox*>(table->cellWidget(row, 1));
    auto* restriction = qobject_cast<QDoubleSpinBox*>(table->cellWidget(row, 2));
    auto* method = qobject_cast<QComboBox*>(table->cellWidget(row, 3));
    cycle->setCurrentIndex(s.cycleType == cfd::AmgSettings::CycleType::Flexible ? 1 : 0);
    term->setValue(s.tolerance);
    restriction->setValue(s.flexAlpha);
    method->setCurrentIndex(static_cast<int>(s.acceleration));
}

void writeAmgRow(QTableWidget* table, int row, cfd::AmgSettings& s) {
    auto* cycle = qobject_cast<QComboBox*>(table->cellWidget(row, 0));
    auto* term = qobject_cast<QDoubleSpinBox*>(table->cellWidget(row, 1));
    auto* restriction = qobject_cast<QDoubleSpinBox*>(table->cellWidget(row, 2));
    auto* method = qobject_cast<QComboBox*>(table->cellWidget(row, 3));
    s.cycleType = cycle->currentIndex() == 1 ? cfd::AmgSettings::CycleType::Flexible
                                             : cfd::AmgSettings::CycleType::VCycle;
    s.tolerance = term->value();
    s.flexBeta = term->value();
    s.flexAlpha = restriction->value();
    s.acceleration = static_cast<cfd::AmgSettings::Acceleration>(method->currentIndex());
}

} // namespace

CfdAdvancedDialog::CfdAdvancedDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Advanced Solver Settings");
    buildUi();
    resetToDefaults();
    uikit::prepareDialog(this, 0);
}

void CfdAdvancedDialog::buildUi() {
    auto* tabs = new QTabWidget(this);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(uikit::kDialogMargin, uikit::kDialogMargin,
                              uikit::kDialogMargin, uikit::kDialogMargin);
    outer->setSpacing(uikit::kDialogSpacing);
    outer->addWidget(tabs);

    // No scroll area: every page is short enough to sit in a plain tab, so the
    // dialog can be sized to its content and never grow a scrollbar. Each page
    // hugs the top (a trailing stretch); without it a short page spreads its
    // rows apart to fill the viewport.
    auto addPage = [tabs](const QString& title) {
        auto* page = new QWidget;
        auto* lay = new QVBoxLayout(page);
        lay->setContentsMargins(uikit::kGroupMargin, uikit::kGroupMargin,
                                uikit::kGroupMargin, uikit::kGroupMargin);
        lay->setSpacing(uikit::kGroupSpacing);
        tabs->addTab(page, title);
        return lay;
    };

    // ── Expert (pressure correction) ─────────────────────────────────────────
    auto* pressureLay = addPage("Expert");
    QFormLayout* pressureForm = nullptr;
    pressureLay->addWidget(uikit::group("Pressure Correction", &pressureForm));

    m_pressureCorrectors = uikit::numeric(new QSpinBox);
    m_pressureCorrectors->setRange(1, 100);
    pressureForm->addRow("Correctors", m_pressureCorrectors);

    m_limitPressureCorrection = new QCheckBox("Limit P' Correction");
    pressureForm->addRow(m_limitPressureCorrection);

    m_pressureCorrectionLimit = uikit::numeric(new QDoubleSpinBox);
    m_pressureCorrectionLimit->setRange(0, 1000);
    m_pressureCorrectionLimit->setDecimals(2);
    pressureForm->addRow("Correction Limit", m_pressureCorrectionLimit);

    m_pressureRefCell = uikit::numeric(new QSpinBox);
    m_pressureRefCell->setRange(0, 1000000);
    m_pressureRefCell->setSpecialValueText("Auto");
    pressureForm->addRow("Reference Cell", m_pressureRefCell);

    // Diagnostics live on the Expert tab: they are scalar settings, not a
    // separate area of the solver.
    QFormLayout* residualForm = nullptr;
    pressureLay->addWidget(uikit::group("Residual Monitoring", &residualForm));

    m_printFrequency = uikit::numeric(new QSpinBox);
    m_printFrequency->setRange(1, 100000);
    residualForm->addRow("Print Frequency", m_printFrequency);

    m_residualHistoryWindow = uikit::numeric(new QSpinBox);
    m_residualHistoryWindow->setRange(1, 10000);
    residualForm->addRow("Residual Window", m_residualHistoryWindow);

    m_velocityCorrectionMaxSpeed = uikit::numeric(new QDoubleSpinBox);
    m_velocityCorrectionMaxSpeed->setRange(0.0, 1e6);
    m_velocityCorrectionMaxSpeed->setDecimals(2);
    m_velocityCorrectionMaxSpeed->setValue(20.0);
    m_velocityCorrectionMaxSpeed->setSuffix(" m/s");
    residualForm->addRow("Max Unlimited Speed", m_velocityCorrectionMaxSpeed);

    m_velocityCorrectionMaxGrowth = uikit::numeric(new QDoubleSpinBox);
    m_velocityCorrectionMaxGrowth->setRange(0.0, 1.0);
    m_velocityCorrectionMaxGrowth->setDecimals(3);
    m_velocityCorrectionMaxGrowth->setSingleStep(0.01);
    m_velocityCorrectionMaxGrowth->setValue(0.15);
    residualForm->addRow("Acceptable Speed Growth Rate", m_velocityCorrectionMaxGrowth);

    QFormLayout* perfForm = nullptr;
    pressureLay->addWidget(uikit::group("Performance", &perfForm));
    m_numThreads = uikit::numeric(new QSpinBox);
    m_numThreads->setRange(0, 1024);
    m_numThreads->setSpecialValueText("Auto");
    perfForm->addRow("Threads", m_numThreads);

    // Output-only diagnostics: extra columns for the run table.
    QFormLayout* diagnosticsForm = nullptr;
    pressureLay->addWidget(uikit::group("Diagnostics", &diagnosticsForm));

    m_printResidentSet = new QCheckBox("Process Memory (MiB)");
    diagnosticsForm->addRow(m_printResidentSet);

    m_printIterationTime = new QCheckBox("Iteration Time");
    diagnosticsForm->addRow(m_printIterationTime);

    m_printTotalTime = new QCheckBox("Total Time");
    diagnosticsForm->addRow(m_printTotalTime);

    m_pressureSolverStats = new QCheckBox("Pressure Solver Statistics");
    diagnosticsForm->addRow(m_pressureSolverStats);
    m_momentumSolverStats = new QCheckBox("Momentum Solver Statistics");
    diagnosticsForm->addRow(m_momentumSolverStats);

    pressureLay->addStretch(1);

    // ── Multigrid ────────────────────────────────────────────────────────────
    // Solver statistics are diagnostics, so they live with the other switches
    // on Expert; the Multigrid tab holds only per-equation and shared AMG knobs.
    auto* amgLay = addPage("Multigrid");

    m_amgTable = new QTableWidget(kAmgRows, 4, nullptr);
    m_amgTable->setVerticalHeaderLabels({"Pressure", "Momentum", "Turbulence"});
    m_amgTable->setHorizontalHeaderLabels(
        {"Cycle Type", "Termination", "Restriction", "AMG Method"});
    m_amgTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_amgTable->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_amgTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_amgTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_amgTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    for (int row = 0; row < kAmgRows; ++row) {
        auto* cycle = new QComboBox(m_amgTable);
        cycle->addItems({"V-Cycle", "Flexible"});
        m_amgTable->setCellWidget(row, kCycleType, cycle);

        auto* term = uikit::numeric(new QDoubleSpinBox(m_amgTable));
        term->setRange(0.0, 1.0);
        term->setDecimals(3);
        m_amgTable->setCellWidget(row, kTermination, term);

        auto* restriction = uikit::numeric(new QDoubleSpinBox(m_amgTable));
        restriction->setRange(0.0, 1.0);
        restriction->setDecimals(3);
        m_amgTable->setCellWidget(row, kRestriction, restriction);

        auto* method = new QComboBox(m_amgTable);
        method->addItems({"None", "CG", "BiCGStab", "GMRES"});
        m_amgTable->setCellWidget(row, kMethod, method);
    }
    amgLay->addWidget(m_amgTable);

    QFormLayout* amgForm = nullptr;
    amgLay->addWidget(uikit::group("Algebraic Multigrid Controls", &amgForm));

    QFormLayout* fixedForm = nullptr;
    m_amgPreSweeps = uikit::numeric(new QSpinBox);
    m_amgPostSweeps = uikit::numeric(new QSpinBox);
    m_amgMaxCycles = uikit::numeric(new QSpinBox);
    fixedForm = nullptr;
    auto* fixedBox = uikit::group("Fixed Cycle Parameters", &fixedForm);
    m_amgPreSweeps->setRange(0, 100);
    m_amgPostSweeps->setRange(0, 100);
    m_amgMaxCycles->setRange(1, 100000);
    fixedForm->addRow("Pre-Sweeps", m_amgPreSweeps);
    fixedForm->addRow("Post-Sweeps", m_amgPostSweeps);
    fixedForm->addRow("Max Cycles", m_amgMaxCycles);
    amgForm->addRow(fixedBox);

    QFormLayout* coarseForm = nullptr;
    m_amgMaxLevels = uikit::numeric(new QSpinBox);
    m_amgCoarsenBy = uikit::numeric(new QSpinBox);
    m_amgConservative = new QCheckBox("Conservative Coarsening");
    m_amgAggressive = new QCheckBox("Aggressive Coarsening");
    m_amgLaplace = new QCheckBox("Laplace Coarsening");
    auto* coarseBox = uikit::group("Coarsening Parameters", &coarseForm);
    m_amgMaxLevels->setRange(1, 1000);
    m_amgCoarsenBy->setRange(1, 16);
    coarseForm->addRow("Max Coarse Levels", m_amgMaxLevels);
    coarseForm->addRow("Coarsen by", m_amgCoarsenBy);
    coarseForm->addRow(m_amgConservative);
    coarseForm->addRow(m_amgAggressive);
    coarseForm->addRow(m_amgLaplace);
    amgForm->addRow(coarseBox);

    auto* smootherLayout = new QVBoxLayout;
    smootherLayout->setContentsMargins(0, 0, 0, 0);
    m_amgGaussSeidel = new QRadioButton("Gauss-Seidel");
    m_amgILU = new QRadioButton("ILU");
    m_amgGaussSeidel->setChecked(true);
    smootherLayout->addWidget(m_amgGaussSeidel);
    smootherLayout->addWidget(m_amgILU);
    amgForm->addRow("Smoother Type", smootherLayout);

    QFormLayout* flexForm = nullptr;
    amgLay->addWidget(uikit::group("Flexible Cycle Parameters", &flexForm));
    m_amgSweeps = uikit::numeric(new QSpinBox);
    m_amgMaxFineRelaxations = uikit::numeric(new QSpinBox);
    m_amgMaxCoarseRelaxations = uikit::numeric(new QSpinBox);
    m_amgVerbosity = uikit::numeric(new QSpinBox);
    m_amgSweeps->setRange(0, 100);
    m_amgMaxFineRelaxations->setRange(0, 1000);
    m_amgMaxCoarseRelaxations->setRange(0, 1000);
    m_amgVerbosity->setRange(0, 3);
    flexForm->addRow("Sweeps", m_amgSweeps);
    flexForm->addRow("Max Fine Relaxations", m_amgMaxFineRelaxations);
    flexForm->addRow("Max Coarse Relaxations", m_amgMaxCoarseRelaxations);
    flexForm->addRow("Verbosity", m_amgVerbosity);

    amgLay->addStretch(1);

    // ── Gradients ────────────────────────────────────────────────────────────
    auto* gradientsLay = addPage("Gradients");
    QFormLayout* gradientsForm = nullptr;
    gradientsLay->addWidget(uikit::group("Slope Limiter", &gradientsForm));

    m_limiterKind = uikit::numeric(new QComboBox);
    m_limiterKind->addItem("MinMod", 0);
    m_limiterKind->addItem("Venkatakrishnan", 1);
    m_limiterKind->addItem("Cubic", 2);
    gradientsForm->addRow("Limiter Method", m_limiterKind);

    // Each limiter-only field keeps its caption in the same row, so the row is
    // hidden and re-shown as one unit when the limiter changes.
    auto addLimiterRow = [gradientsForm](const QString& caption, QWidget* field) {
        auto* row = new QWidget;
        auto* lay = new QHBoxLayout(row);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(6);
        lay->addWidget(uikit::note(caption, row));
        lay->addWidget(field);
        gradientsForm->addRow(row);
        return row;
    };

    m_limiterK = uikit::numeric(new QDoubleSpinBox);
    m_limiterK->setRange(0.0, 1e6);
    m_limiterK->setDecimals(4);
    m_limiterK->setSingleStep(0.01);
    m_limiterK->setValue(0.1);
    m_limiterKRow = addLimiterRow("Smoothing Constant (K)", m_limiterK);

    m_limiterCubicTransition = uikit::numeric(new QDoubleSpinBox);
    m_limiterCubicTransition->setRange(1.0, 2.0);
    m_limiterCubicTransition->setDecimals(2);
    m_limiterCubicTransition->setSingleStep(0.05);
    m_limiterCubicTransition->setValue(1.5);
    m_limiterCubicRow = addLimiterRow("Cubic Transition", m_limiterCubicTransition);

    auto* directionLay = new QVBoxLayout;
    directionLay->setContentsMargins(0, 0, 0, 0);
    directionLay->setSpacing(uikit::kGroupSpacing);
    m_limiterSpan = new QButtonGroup(this);
    m_limiterFaceCentre = new QRadioButton("Face Centre");
    m_limiterFaceCentre->setChecked(true);
    m_limiterSpan->addButton(m_limiterFaceCentre, 0);
    m_limiterCentroidLine = new QRadioButton("Centroid Line");
    m_limiterSpan->addButton(m_limiterCentroidLine, 1);
    directionLay->addWidget(m_limiterFaceCentre);
    directionLay->addWidget(m_limiterCentroidLine);
    gradientsForm->addRow("Limiting Span", directionLay);

    auto* aggLay = new QVBoxLayout;
    aggLay->setContentsMargins(0, 0, 0, 0);
    aggLay->setSpacing(uikit::kGroupSpacing);
    m_limiterAggregation = new QButtonGroup(this);
    m_limiterCellWide = new QRadioButton("Cell-Wide");
    m_limiterCellWide->setChecked(true);
    m_limiterAggregation->addButton(m_limiterCellWide, 0);
    m_limiterComponentWise = new QRadioButton("Per Face");
    m_limiterAggregation->addButton(m_limiterComponentWise, 1);
    aggLay->addWidget(m_limiterCellWide);
    aggLay->addWidget(m_limiterComponentWise);
    gradientsForm->addRow("Limiting Scope", aggLay);

    gradientsLay->addStretch(1);

    // ── Footer ───────────────────────────────────────────────────────────────
    auto* buttons = uikit::buttons(this, true);
    outer->addWidget(buttons);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked,
            this, [this] { resetToDefaults(); });

    // ── Dependencies ─────────────────────────────────────────────────────────
    connect(m_limitPressureCorrection, &QCheckBox::toggled, this,
            [this](bool) { syncDependencies(); });
    connect(m_limiterKind, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { syncDependencies(); });

    syncDependencies();
}

void CfdAdvancedDialog::setSettings(const cfd::CaseSettings& s) {
    m_pressureCorrectors->setValue(s.pressureCorrectors);
    m_limitPressureCorrection->setChecked(s.limitPressureCorrection);
    m_pressureCorrectionLimit->setValue(s.pressureCorrectionLimit);
    m_pressureRefCell->setValue(static_cast<int>(s.pressureRefCell));

    m_pressureSolverStats->setChecked(s.pressureSolverStats);
    m_momentumSolverStats->setChecked(s.momentumSolverStats);
    readAmgRow(s.pressureAmg, m_amgTable, 0);
    readAmgRow(s.momentumAmg, m_amgTable, 1);
    readAmgRow(s.turbulenceAmg, m_amgTable, 2);
    readSharedAmg(s.pressureAmg);

    m_printFrequency->setValue(static_cast<int>(s.printFrequency));
    m_residualHistoryWindow->setValue(static_cast<int>(s.residualHistoryWindow));
    m_velocityCorrectionMaxSpeed->setValue(s.velocityCorrectionMaxUnlimitedSpeed);
    m_velocityCorrectionMaxGrowth->setValue(s.velocityCorrectionMaxGrowthRate);
    m_numThreads->setValue(s.numThreads);
    m_printResidentSet->setChecked(s.printResidentSet);
    m_printIterationTime->setChecked(s.printIterationTime);
    m_printTotalTime->setChecked(s.printTotalTime);

    m_limiterKind->setCurrentIndex(s.limiterKind);
    m_limiterFaceCentre->setChecked(s.limiterSpan == 0);
    m_limiterCentroidLine->setChecked(s.limiterSpan == 1);
    m_limiterCellWide->setChecked(s.limiterAggregation == 0);
    m_limiterComponentWise->setChecked(s.limiterAggregation == 1);
    m_limiterK->setValue(s.limiterK);
    m_limiterCubicTransition->setValue(s.cubicTransition);

    syncDependencies();
}

void CfdAdvancedDialog::setDimension(int) {
    // No dimension-dependent AMG control in the simplified UI.
}

void CfdAdvancedDialog::readSharedAmg(const cfd::AmgSettings& s) {
    m_amgPreSweeps->setValue(s.preSweeps);
    m_amgPostSweeps->setValue(s.postSweeps);
    m_amgMaxCycles->setValue(s.maxIterations);
    m_amgMaxLevels->setValue(s.maxLevels);
    m_amgCoarsenBy->setValue(s.blockSize);
    m_amgConservative->setChecked(s.coarsening == cfd::AmgSettings::Coarsening::SmoothedAggregation);
    m_amgAggressive->setChecked(false);
    m_amgLaplace->setChecked(false);
    m_amgGaussSeidel->setChecked(s.smoother != cfd::AmgSettings::Smoother::ILU);
    m_amgILU->setChecked(s.smoother == cfd::AmgSettings::Smoother::ILU);
    m_amgSweeps->setValue(s.flexMaxSweeps);
    m_amgMaxFineRelaxations->setValue(s.flexMaxFineRelaxations);
    m_amgMaxCoarseRelaxations->setValue(s.flexMaxCoarseRelaxations);
    m_amgVerbosity->setValue(static_cast<int>(s.verbosity));
}

void CfdAdvancedDialog::writeSharedAmgInto(cfd::AmgSettings& s) const {
    s.preSweeps = m_amgPreSweeps->value();
    s.postSweeps = m_amgPostSweeps->value();
    s.maxIterations = m_amgMaxCycles->value();
    s.maxLevels = m_amgMaxLevels->value();
    s.blockSize = m_amgCoarsenBy->value();
    s.coarsening = m_amgConservative->isChecked()
                       ? cfd::AmgSettings::Coarsening::SmoothedAggregation
                       : cfd::AmgSettings::Coarsening::RugeStuben;
    // Aggressive / Laplace coarsening are shown for layout parity with the
    // reference UI; the backend has no equivalent modes yet (deferred).
    s.smoother = m_amgILU->isChecked() ? cfd::AmgSettings::Smoother::ILU
                                       : cfd::AmgSettings::Smoother::GaussSeidel;
    s.flexMaxSweeps = m_amgSweeps->value();
    s.flexMaxFineRelaxations = m_amgMaxFineRelaxations->value();
    s.flexMaxCoarseRelaxations = m_amgMaxCoarseRelaxations->value();
    s.verbosity = static_cast<cfd::AmgSettings::Verbosity>(m_amgVerbosity->value());
}

void CfdAdvancedDialog::collectInto(cfd::CaseSettings& s) const {
    s.pressureCorrectors = m_pressureCorrectors->value();
    s.limitPressureCorrection = m_limitPressureCorrection->isChecked();
    s.pressureCorrectionLimit = m_pressureCorrectionLimit->value();
    s.pressureRefCell = static_cast<std::size_t>(m_pressureRefCell->value());

    s.pressureSolverStats = m_pressureSolverStats->isChecked();
    s.momentumSolverStats = m_momentumSolverStats->isChecked();
    // Start from the existing configs so knobs with no UI control keep their
    // values (case file, CLI, or previously collected settings).
    writeAmgRow(m_amgTable, 0, s.pressureAmg);
    writeAmgRow(m_amgTable, 1, s.momentumAmg);
    writeAmgRow(m_amgTable, 2, s.turbulenceAmg);
    writeSharedAmgInto(s.pressureAmg);
    writeSharedAmgInto(s.momentumAmg);
    writeSharedAmgInto(s.turbulenceAmg);

    s.printFrequency = static_cast<std::size_t>(m_printFrequency->value());
    s.residualHistoryWindow = static_cast<std::size_t>(m_residualHistoryWindow->value());
    s.velocityCorrectionMaxUnlimitedSpeed = m_velocityCorrectionMaxSpeed->value();
    s.velocityCorrectionMaxGrowthRate = m_velocityCorrectionMaxGrowth->value();
    s.numThreads = m_numThreads->value();
    s.printResidentSet = m_printResidentSet->isChecked();
    s.printIterationTime = m_printIterationTime->isChecked();
    s.printTotalTime = m_printTotalTime->isChecked();

    s.limiterKind = m_limiterKind->currentIndex();
    s.limiterSpan = m_limiterSpan->checkedId();
    if (s.limiterSpan < 0) s.limiterSpan = 0;
    s.limiterAggregation = m_limiterAggregation->checkedId();
    if (s.limiterAggregation < 0) s.limiterAggregation = 0;
    s.limiterK = m_limiterK->value();
    s.cubicTransition = m_limiterCubicTransition->value();
}

void CfdAdvancedDialog::resetToDefaults() {
    // CaseSettings' member initialisers are the single source of defaults for
    // the solver, the case file and this dialog.
    setSettings(cfd::CaseSettings{});
}

void CfdAdvancedDialog::syncDependencies() {
    m_pressureCorrectionLimit->setEnabled(m_limitPressureCorrection->isChecked());

    const int limiter = m_limiterKind->currentIndex();
    m_limiterKRow->setVisible(limiter == 1);
    m_limiterCubicRow->setVisible(limiter == 2);
    m_limiterK->setEnabled(limiter == 1);
    m_limiterCubicTransition->setEnabled(limiter == 2);
}

} // namespace sidebar