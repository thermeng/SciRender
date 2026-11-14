#include "ui/cfd_advanced_dialog.h"

#include "ui/uikit.h"
#include "ui/widgets/amg_config_widget.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace sidebar {

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
    m_pressureCorrectors->setToolTip("Pressure-corrector sweeps per iteration.");
    pressureForm->addRow("Correctors", m_pressureCorrectors);

    m_limitPressureCorrection = new QCheckBox("Limit P' Correction");
    m_limitPressureCorrection->setToolTip(
        "Clamp the pressure correction as a fraction of rho*U^2. Prevents the first\n"
        "iterations from overshooting, at the cost of a weaker pressure-velocity coupling.");
    pressureForm->addRow(m_limitPressureCorrection);

    m_pressureCorrectionLimit = uikit::numeric(new QDoubleSpinBox);
    m_pressureCorrectionLimit->setRange(0, 1000);
    m_pressureCorrectionLimit->setDecimals(2);
    m_pressureCorrectionLimit->setToolTip("Upper bound on |p'| as a multiple of rho*U_ref^2.");
    pressureForm->addRow("Correction Limit", m_pressureCorrectionLimit);

    m_pressureRefCell = uikit::numeric(new QSpinBox);
    m_pressureRefCell->setRange(0, 1000000);
    m_pressureRefCell->setSpecialValueText("Auto");
    m_pressureRefCell->setToolTip(
        "Cell that pins the pressure datum when no pressure outlet exists. Auto = last cell.");
    pressureForm->addRow("Reference Cell", m_pressureRefCell);

    // Diagnostics live on the Expert tab: they are scalar settings, not a
    // separate area of the solver.
    QFormLayout* residualForm = nullptr;
    pressureLay->addWidget(uikit::group("Residual Monitoring", &residualForm));

    m_printFrequency = uikit::numeric(new QSpinBox);
    m_printFrequency->setRange(1, 100000);
    m_printFrequency->setToolTip("Print a residual row every N outer iterations.");
    residualForm->addRow("Print Frequency", m_printFrequency);

    m_residualHistoryWindow = uikit::numeric(new QSpinBox);
    m_residualHistoryWindow->setRange(1, 10000);
    m_residualHistoryWindow->setToolTip(
        "Iterations inspected when watching for a continuity residual that keeps growing.");
    residualForm->addRow("Residual Window", m_residualHistoryWindow);

    m_velocityCorrectionMaxSpeed = uikit::numeric(new QDoubleSpinBox);
    m_velocityCorrectionMaxSpeed->setRange(0.0, 1e6);
    m_velocityCorrectionMaxSpeed->setDecimals(2);
    m_velocityCorrectionMaxSpeed->setValue(20.0);
    m_velocityCorrectionMaxSpeed->setSuffix(" m/s");
    m_velocityCorrectionMaxSpeed->setToolTip(
        "Velocity growth limiting is only applied when cell speed exceeds "
        "this threshold. Below it, no damping is applied.");
    residualForm->addRow("Max Unlimited Speed", m_velocityCorrectionMaxSpeed);

    m_velocityCorrectionMaxGrowth = uikit::numeric(new QDoubleSpinBox);
    m_velocityCorrectionMaxGrowth->setRange(0.0, 1.0);
    m_velocityCorrectionMaxGrowth->setDecimals(3);
    m_velocityCorrectionMaxGrowth->setSingleStep(0.01);
    m_velocityCorrectionMaxGrowth->setValue(0.15);
    m_velocityCorrectionMaxGrowth->setToolTip(
        "Maximum per-iteration velocity amplitude growth rate from pressure correction.");
    residualForm->addRow("Acceptable Speed Growth Rate", m_velocityCorrectionMaxGrowth);

    QFormLayout* perfForm = nullptr;
    pressureLay->addWidget(uikit::group("Performance", &perfForm));
    m_numThreads = uikit::numeric(new QSpinBox);
    m_numThreads->setRange(0, 1024);
    m_numThreads->setSpecialValueText("Auto");
    m_numThreads->setToolTip(
        "OpenMP threads used by the solver (matrix assembly, AMG, residuals).\n"
        "0 = runtime default (honours OMP_NUM_THREADS).");
    perfForm->addRow("Threads", m_numThreads);

    // Output-only diagnostics: extra columns for the run table.
    QFormLayout* diagnosticsForm = nullptr;
    pressureLay->addWidget(uikit::group("Diagnostics", &diagnosticsForm));

    m_printResidentSet = new QCheckBox("Process Memory (RSS)");
    m_printResidentSet->setToolTip(
        "Append the process resident-set size to every transient row. Useful\n"
        "when watching a long run for memory growth; off by default, because\n"
        "sampling the OS every step is not part of normal solver output.");
    diagnosticsForm->addRow(m_printResidentSet);

    pressureLay->addStretch(1);

    // ── Pressure AMG ─────────────────────────────────────────────────────────
    auto* pressureAmgLay = addPage("Pressure");
    m_pressureSolverStats = new QCheckBox("Pressure Solver Statistics");
    m_pressureSolverStats->setToolTip("Print per-run pressure linear-solver statistics.");
    pressureAmgLay->addWidget(m_pressureSolverStats);
    m_pressureAmg = new AmgConfigWidget(cfd::AmgSettings::pressureDefaults(), nullptr, true);
    pressureAmgLay->addWidget(m_pressureAmg);
    pressureAmgLay->addStretch(1);

    // ── Momentum ─────────────────────────────────────────────────────────────
    auto* momentumLay = addPage("Momentum");

    m_momentumSolverStats = new QCheckBox("Momentum Solver Statistics");
    m_momentumSolverStats->setToolTip("Print per-run momentum linear-solver statistics.");
    momentumLay->addWidget(m_momentumSolverStats);

    m_momentumAmg = new AmgConfigWidget(cfd::AmgSettings::momentumDefaults());
    momentumLay->addWidget(m_momentumAmg);
    momentumLay->addStretch(1);

    // ── Turbulence ───────────────────────────────────────────────────────────
    auto* turbulenceLay = addPage("Turbulence");

    m_turbulenceAmg = new AmgConfigWidget(cfd::AmgSettings::momentumDefaults());
    turbulenceLay->addWidget(m_turbulenceAmg);
    turbulenceLay->addStretch(1);

    // ── Gradients ────────────────────────────────────────────────────────────
    auto* gradientsLay = addPage("Gradients");
    QFormLayout* gradientsForm = nullptr;
    gradientsLay->addWidget(uikit::group("Slope Limiter", &gradientsForm));

    m_limiterKind = uikit::numeric(new QComboBox);
    m_limiterKind->addItem("MinMod", 0);
    m_limiterKind->addItem("Venkatakrishnan", 1);
    m_limiterKind->addItem("Cubic", 2);
    m_limiterKind->setToolTip(
        "MinMod = Barth-Jespersen minmod, one factor per cell.\n"
        "Venkatakrishnan = smooth Venkatakrishnan function.\n"
        "Cubic = Michalak-Ollivier-Gooch cubic fit, never exceeds 1 "
        "(OpenFOAM-recommended).");
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
    m_limiterK->setToolTip(
        "Venkatakrishnan smoothing: eps = k * (hi - lo), relative to the local "
        "field variation. Only used by the Venkatakrishnan limiter.");
    m_limiterKRow = addLimiterRow("Smoothing Constant (K)", m_limiterK);

    m_limiterCubicTransition = uikit::numeric(new QDoubleSpinBox);
    m_limiterCubicTransition->setRange(1.0, 2.0);
    m_limiterCubicTransition->setDecimals(2);
    m_limiterCubicTransition->setSingleStep(0.05);
    m_limiterCubicTransition->setValue(1.5);
    m_limiterCubicTransition->setToolTip(
        "Cubic transition point where the limiter reaches 1 (OpenFOAM uses 1.5). "
        "Only used by the Cubic limiter.");
    m_limiterCubicRow = addLimiterRow("Cubic Transition", m_limiterCubicTransition);

    auto* directionLay = new QVBoxLayout;
    directionLay->setContentsMargins(0, 0, 0, 0);
    directionLay->setSpacing(uikit::kGroupSpacing);
    m_limiterSpan = new QButtonGroup(this);
    m_limiterFaceCentre = new QRadioButton("Face Centre");
    m_limiterFaceCentre->setChecked(true);
    m_limiterFaceCentre->setToolTip("d = x_face - x_upwind (default).");
    m_limiterSpan->addButton(m_limiterFaceCentre, 0);
    m_limiterCentroidLine = new QRadioButton("Centroid Line");
    m_limiterCentroidLine->setToolTip("d along the cell-centroid line, scaled to "
                                      "the face plane.");
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
    m_limiterCellWide->setToolTip("One scale factor per cell: the most restrictive "
                                  "face limits every face of the cell.");
    m_limiterAggregation->addButton(m_limiterCellWide, 0);
    m_limiterComponentWise = new QRadioButton("Per Face");
    m_limiterComponentWise->setToolTip("One scale factor per face: only faces that "
                                       "leave the bound set are clipped.");
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
    m_pressureAmg->setConfig(s.pressureAmg);

    m_pressureSolverStats->setChecked(s.pressureSolverStats);
    m_momentumSolverStats->setChecked(s.momentumSolverStats);
    m_momentumAmg->setConfig(s.momentumAmg);
    m_turbulenceAmg->setConfig(s.turbulenceAmg);

    m_printFrequency->setValue(static_cast<int>(s.printFrequency));
    m_residualHistoryWindow->setValue(static_cast<int>(s.residualHistoryWindow));
    m_velocityCorrectionMaxSpeed->setValue(s.velocityCorrectionMaxUnlimitedSpeed);
    m_velocityCorrectionMaxGrowth->setValue(s.velocityCorrectionMaxGrowthRate);
    m_numThreads->setValue(s.numThreads);
    m_printResidentSet->setChecked(s.printResidentSet);

    m_limiterKind->setCurrentIndex(s.limiterKind);
    m_limiterFaceCentre->setChecked(s.limiterSpan == 0);
    m_limiterCentroidLine->setChecked(s.limiterSpan == 1);
    m_limiterCellWide->setChecked(s.limiterAggregation == 0);
    m_limiterComponentWise->setChecked(s.limiterAggregation == 1);
    m_limiterK->setValue(s.limiterK);
    m_limiterCubicTransition->setValue(s.cubicTransition);

    syncDependencies();
}

void CfdAdvancedDialog::collectInto(cfd::CaseSettings& s) const {
    s.pressureCorrectors = m_pressureCorrectors->value();
    s.limitPressureCorrection = m_limitPressureCorrection->isChecked();
    s.pressureCorrectionLimit = m_pressureCorrectionLimit->value();
    s.pressureRefCell = static_cast<std::size_t>(m_pressureRefCell->value());
    s.pressureAmg = m_pressureAmg->config();

    s.pressureSolverStats = m_pressureSolverStats->isChecked();
    s.momentumSolverStats = m_momentumSolverStats->isChecked();
    s.momentumAmg = m_momentumAmg->config();
    s.turbulenceAmg = m_turbulenceAmg->config();

    s.printFrequency = static_cast<std::size_t>(m_printFrequency->value());
    s.residualHistoryWindow = static_cast<std::size_t>(m_residualHistoryWindow->value());
    s.velocityCorrectionMaxUnlimitedSpeed = m_velocityCorrectionMaxSpeed->value();
    s.velocityCorrectionMaxGrowthRate = m_velocityCorrectionMaxGrowth->value();
    s.numThreads = m_numThreads->value();
    s.printResidentSet = m_printResidentSet->isChecked();

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