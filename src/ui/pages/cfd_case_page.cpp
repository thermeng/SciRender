#include "ui/pages/cfd_case_page.h"

#include "cfd/cfd_types.h"
#include "render/settings/render_settings.h"
#include "ui/cfd_bc_dialog.h"
#include "ui/controllers/cfd_controller.h"
#include "ui/panel_helpers.h"
#include "ui/widgets/wheel_guard.h"
#include "ui/widgets/scientific_spin_box.h"
#include "ui/widgets/wheel_guard.h"
#include "ui_cfd_case_page.h"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <iostream>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace sidebar {

namespace {

// Read-only boxes (status, solve monitor) use palette roles so they stay
// legible in both light and dark themes. palette(base)/palette(text) is the
// standard text-area pairing; palette(mid) matches the section dividers.
QString readoutStyle() {
    return "background: palette(base); color: palette(text);"
           "border: 1px solid palette(mid); border-radius: 4px;";
}

// Numeric telemetry reads better when the digits do not wander as values
// change width, so residual/iteration values use a fixed-pitch face.
QFont telemetryFont() {
    QFont f("Consolas");
    f.setStyleHint(QFont::Monospace);
    f.setPixelSize(11);
    return f;
}

// Section captions inside the advanced dialog. The page's own styling hook only
// runs on the sidebar content, so the dialog labels carry their own font.
QLabel* dialogSectionHeader(const QString& text) {
    auto* label = new QLabel(text);
    QFont f = label->font();
    f.setPixelSize(11);
    f.setWeight(QFont::Medium);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.3);
    label->setFont(f);
    return label;
}

} // namespace

CfdCasePage::CfdCasePage(RenderSettings* settings, QWidget* parent)
    : SidebarPage(settings, parent) {
    auto* content = new QWidget;
    Ui::CfdCasePage cfdUi;
    cfdUi.setupUi(content);
    sb::fixLayoutOverflow(content);

    m_ctrl = new CfdController(this);
    connect(m_ctrl, &CfdController::meshLoaded, this, &CfdCasePage::onMeshLoaded);
    connect(m_ctrl, &CfdController::caseLoaded, this, &CfdCasePage::onCaseLoaded);


    connect(m_ctrl, &CfdController::progress, this, &CfdCasePage::onProgress);
    connect(m_ctrl, &CfdController::finished, this, &CfdCasePage::onFinished);
    connect(m_ctrl, &CfdController::failed, this, &CfdCasePage::onFailed);

    // ── Widgets ──────────────────────────────────────────────────────────────
    m_iterValue = cfdUi.iterValue;
    m_contValue = cfdUi.contValue;
    m_momValue = cfdUi.momValue;

    m_bcEditBtn = cfdUi.bcEditBtn;
    m_advancedBtn = cfdUi.advancedBtn;
    m_initBtn = cfdUi.initBtn;
    m_runBtn = cfdUi.runBtn;
    m_cancelBtn = cfdUi.cancelBtn;
    m_pauseBtn = cfdUi.pauseBtn;
    m_stepBtn = cfdUi.stepBtn;



    m_rho = cfdUi.rhoSpin;
    m_nu = cfdUi.nuSpin;
    m_tol = cfdUi.tolSpin;
    m_maxIter = cfdUi.maxIterSpin;
    m_alphaU = cfdUi.alphaUSpin;
    m_alphaP = cfdUi.alphaPSpin;
    m_simplec = cfdUi.simplecCb;
    m_scheme = cfdUi.schemeCombo;
    m_gradientScheme = cfdUi.gradientSchemeCombo;
    m_rhie = cfdUi.rhieCb;
    m_turb = cfdUi.turbCb;

    // ── Time mode (steady / transient) ───────────────────────────────
    m_steadyRadio = cfdUi.steadyRadio;
    m_transientRadio = cfdUi.transientRadio;
    m_transientOptions = cfdUi.transientOptions;
    m_timeSchemeCombo = cfdUi.timeSchemeCombo;
    m_dtSpin = cfdUi.dtSpin;
    m_tEndSpin = cfdUi.tEndSpin;
    m_adaptiveCflCb = cfdUi.adaptiveCflCb;
    m_cflSpin = cfdUi.cflSpin;
    m_minDtSpin = cfdUi.minDtSpin;
    m_maxDtSpin = cfdUi.maxDtSpin;
    m_writeIntervalSpin = cfdUi.writeIntervalSpin;
    m_outerCorrectorsSpin = cfdUi.outerCorrectorsSpin;
    m_nonOrthCorrectorsSpin = cfdUi.nonOrthCorrectorsSpin;
    m_pvdFileEdit = cfdUi.pvdFileEdit;
    m_pvdBrowseBtn = cfdUi.pvdBrowseBtn;

    // ── Physics defaults ─────────────────────────────────────────────────────
    m_rho->setText("1.0");
    m_nu->setText("0.01");
    // ── Solver defaults ──────────────────────────────────────────────────────
    // The tolerance spin box displays scientific notation (see
    // ScientificDoubleSpinBox) but keeps a high decimal precision so values
    // below 1e-3 stay representable and the 1e-6 arrow step stays visible.
    m_tol->setRange(1e-7, 1e-1);   m_tol->setDecimals(9);  m_tol->setValue(1e-3);
    m_tol->setSingleStep(1e-6);
    m_tol->setCorrectionMode(QDoubleSpinBox::CorrectToPreviousValue);
    m_maxIter->setRange(1, 100000); m_maxIter->setValue(500);
    m_alphaU->setRange(0.01, 1.0); m_alphaU->setDecimals(2); m_alphaU->setValue(0.7);
    m_alphaP->setRange(0.01, 1.0); m_alphaP->setDecimals(2); m_alphaP->setValue(0.3);
    m_scheme->addItems({"Upwind", "Central", "Second-Order Upwind"});
    m_scheme->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_scheme->setMinimumContentsLength(8);
    m_gradientScheme->addItems({"Least Squares", "Green-Gauss Cell", "Green-Gauss Node"});
    m_gradientScheme->setCurrentIndex(0);
    m_rhie->setChecked(true);

    // ── Time mode defaults ───────────────────────────────────────────────────
    m_timeSchemeCombo->addItems({"Backward Euler", "BDF2"});
    m_timeSchemeCombo->setCurrentIndex(0);
    m_dtSpin->setRange(1e-12, 1e3);   m_dtSpin->setDecimals(9);  m_dtSpin->setValue(1e-3);
    m_dtSpin->setSingleStep(1e-4);
    m_tEndSpin->setRange(1e-12, 1e6); m_tEndSpin->setDecimals(6); m_tEndSpin->setValue(1.0);
    m_cflSpin->setRange(1e-3, 1e3);   m_cflSpin->setDecimals(3);  m_cflSpin->setValue(0.5);
    m_minDtSpin->setRange(1e-12, 1e3); m_minDtSpin->setDecimals(9); m_minDtSpin->setValue(1e-6);
    m_maxDtSpin->setRange(1e-12, 1e3); m_maxDtSpin->setDecimals(6); m_maxDtSpin->setValue(0.01);
    m_writeIntervalSpin->setRange(0, 1000000); m_writeIntervalSpin->setValue(1);
    m_outerCorrectorsSpin->setRange(1, 50); m_outerCorrectorsSpin->setValue(3);
    m_nonOrthCorrectorsSpin->setRange(0, 10); m_nonOrthCorrectorsSpin->setValue(1);

    auto syncTime = [this]() { syncTimeDependencies(); };
    connect(m_steadyRadio, &QRadioButton::toggled, this, [syncTime](bool) { syncTime(); });
    connect(m_transientRadio, &QRadioButton::toggled, this, [syncTime](bool) { syncTime(); });
    connect(m_pvdBrowseBtn, &QPushButton::clicked, this, [this]() {
        QString f = QFileDialog::getSaveFileName(this, "PVD file", m_pvdFileEdit->text(), "PVD files (*.pvd)");
        if (!f.isEmpty()) m_pvdFileEdit->setText(f);
    });

    // ── Frame streaming from transient runs ──────────────────────────────────
    m_frameTimer = new QTimer(this);
    m_frameTimer->setInterval(16);
    connect(m_frameTimer, &QTimer::timeout, this, [this]() {
        if (!m_ctrl || !m_running) return;
        if (m_ctrl->isCancelled()) return;
        // One immutable slot per publish => mesh and snapshot always match.
        auto slot = m_ctrl->latestFrameSlot();
        if (!slot || !slot->mesh || slot->mesh == m_lastFrame) return;
        m_lastFrame = slot->mesh;
        // RenderSettings is GUI-thread state: the worker never writes it.
        S()->setComputedSnapshot(slot->computed);
        if (slot->postExtracted)
            S()->applyPostStatus(slot->postStatus);
        onTransientFrame(slot->mesh, slot->step, slot->time);
    });

    // The solver only observes Pause at a step boundary, so the button text and
    // the Step gating must follow the solver state rather than the click.
    m_stateTimer = new QTimer(this);
    m_stateTimer->setInterval(100);
    connect(m_stateTimer, &QTimer::timeout, this, [this]() {
        if (!m_running) { m_stateTimer->stop(); return; }
        const bool paused = m_ctrl->isPaused();
        const bool settled = m_ctrl->isPausedSettled();
        if (m_pauseBtn) {
            m_pauseBtn->setText(paused ? (settled ? "Resume" : "Pausing...") : "Pause");
        }
        if (m_stepBtn) m_stepBtn->setEnabled(settled);
        if (m_cancelBtn && !m_ctrl->isCancelled())
            m_cancelBtn->setEnabled(true);
        if (!paused) {
            m_pauseSettledLogged = false;
        } else if (settled && !m_pauseSettledLogged) {
            m_pauseSettledLogged = true;
            const int step = m_ctrl->currentStep();
            const double t = m_ctrl->currentTime();
            if (m_ctrl->settings().transient.enabled) {
                m_iterValue->setText(QString::number(step));
                log(QString("Paused at step %1, t = %2 s").arg(step).arg(t, 0, 'g', 8));
            } else {
                log(QString("Paused at iteration %1").arg(m_iterValue->text()));
            }
        }
    });

    // ── Advanced solver settings (dialog) ────────────────────────────────────
    // Built up front and kept hidden. Its fields are read by collectSettings(),
    // so they must exist from the start; creating them as children of the page
    // and reparenting them only on first open would leave them painted over the
    // top-left corner of the panel (a parented widget with no layout floats).
    buildAdvancedDialog();
    connect(m_advancedBtn, &QPushButton::clicked, this, [this] { m_advancedDlg->exec(); });
    syncTime(); // depends on dialog widgets; must run after buildAdvancedDialog()
    // ── Read-out boxes ───────────────────────────────────────────────────────
    QWidget* monitor = cfdUi.solveMonitor;
    monitor->setAttribute(Qt::WA_StyledBackground, true);
    monitor->setStyleSheet(QString("#solveMonitor { %1 }").arg(readoutStyle()));
    if (auto* grid = qobject_cast<QGridLayout*>(monitor->layout())) {
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 0);
    }
    for (QLabel* value : {m_iterValue, m_contValue, m_momValue}) {
        value->setFont(telemetryFont());
    }

    // ── Signals ──────────────────────────────────────────────────────────────
    connect(m_bcEditBtn, &QPushButton::clicked, this, &CfdCasePage::onEditBCs);
    connect(m_runBtn, &QPushButton::clicked, this, &CfdCasePage::onRun);
    connect(m_cancelBtn, &QPushButton::clicked, this, &CfdCasePage::onCancel);
    connect(m_initBtn, &QPushButton::clicked, this, &CfdCasePage::onInitialize);
    connect(m_pauseBtn, &QPushButton::clicked, this, &CfdCasePage::onPause);
    connect(m_stepBtn, &QPushButton::clicked, this, &CfdCasePage::onStep);


    log("No mesh loaded.");
    setRunning(false);

    qobject_cast<QVBoxLayout*>(content->layout())->addStretch();
    installWheelGuard(content);
    setContent(content, true);
}

void CfdCasePage::buildAdvancedDialog() {
    m_advancedDlg = new QDialog(this);
    m_advancedDlg->setWindowTitle("Advanced solver settings");
    m_advancedDlg->resize(660, 560);

    auto* tabs = new QTabWidget(m_advancedDlg);
    auto* outer = new QVBoxLayout(m_advancedDlg);
    outer->addWidget(tabs);

    // Every page is scrollable: the AMG pages are taller than the dialog.
    // Content hugs the top because each page ends with a stretch; without it a
    // short page spreads its rows apart to fill the viewport, which is what
    // opened the large gaps between the pressure fields and their header.
    auto addScrollPage = [&](const QString& title) {
        auto* page = new QWidget;
        auto* lay = new QVBoxLayout(page);
        lay->setContentsMargins(12, 12, 12, 12);
        lay->setSpacing(10);
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(page);
        tabs->addTab(scroll, title);
        return lay;
    };
    // A titled group per related set of settings. Titled frames make the
    // structure visible without the floating header labels the old layout had.
    auto addGroup = [](QVBoxLayout* lay, const QString& title) {
        auto* box = new QGroupBox(title);
        auto* form = new QFormLayout(box);
        form->setContentsMargins(12, 8, 12, 12);
        form->setSpacing(6);
        form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
        form->setRowWrapPolicy(QFormLayout::DontWrapRows);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
        lay->addWidget(box);
        return form;
    };
    auto styleSpin = [](auto* spin) {
        spin->setFixedWidth(110);
        QFont f("JetBrains Mono");
        f.setStyleHint(QFont::Monospace);
        f.setPixelSize(11);
        spin->setFont(f);
        return spin;
    };

    // ── Pressure ─────────────────────────────────────────────────────────────
    auto* pressureLay = addScrollPage("Pressure");
    auto* pressureForm = addGroup(pressureLay, "Pressure correction");

    m_pressureCorrectors = styleSpin(new QSpinBox);
    m_pressureCorrectors->setRange(1, 100);
    m_pressureCorrectors->setToolTip("Pressure-corrector sweeps per SIMPLE iteration.");
    pressureForm->addRow("Correctors", m_pressureCorrectors);

    m_limitPressureCorrection = new QCheckBox("Limit p' correction");
    m_limitPressureCorrection->setToolTip(
        "Clamp the pressure correction as a fraction of rho*U^2. Prevents the first\n"
        "iterations from overshooting, at the cost of a weaker pressure-velocity coupling.");
    pressureForm->addRow(m_limitPressureCorrection);

    m_pressureCorrectionLimit = styleSpin(new QDoubleSpinBox);
    m_pressureCorrectionLimit->setRange(0, 1000);
    m_pressureCorrectionLimit->setDecimals(2);
    m_pressureCorrectionLimit->setToolTip("Upper bound on |p'| as a multiple of rho*U_ref^2.");
    pressureForm->addRow("Correction limit", m_pressureCorrectionLimit);

    m_pressureRefCell = styleSpin(new QSpinBox);
    m_pressureRefCell->setRange(0, 1000000);
    m_pressureRefCell->setSpecialValueText("auto");
    m_pressureRefCell->setToolTip(
        "Cell that pins the pressure datum when no pressure outlet exists. auto = last cell.");
    pressureForm->addRow("Reference cell", m_pressureRefCell);

    auto* pressureSolveForm = addGroup(pressureLay, "Pressure linear solver");

    m_pressureUseAMG = new QCheckBox("Build an AMG hierarchy");
    m_pressureUseAMG->setToolTip(
        "Rebuild the multigrid hierarchy for the pressure Poisson operator. When off,\n"
        "the pressure solve always uses CG+IC (or BiCGSTAB+ILU).");
    pressureSolveForm->addRow(m_pressureUseAMG);

    m_pressureUseAMGPreconditioner = new QCheckBox("Use AMG as preconditioner");
    m_pressureUseAMGPreconditioner->setToolTip(
        "On: BiCGSTAB preconditioned by one AMG cycle per iteration.\n"
        "Off: a standalone AMG solve, terminated by the max iterations and\n"
        "tolerance on the Pressure AMG tab.");
    pressureSolveForm->addRow(m_pressureUseAMGPreconditioner);

    m_pressureAmgThreshold = styleSpin(new QSpinBox);
    m_pressureAmgThreshold->setRange(0, 100000000);
    m_pressureAmgThreshold->setSingleStep(1000);
    m_pressureAmgThreshold->setSpecialValueText("always");
    m_pressureAmgThreshold->setToolTip(
        "Cell count below which AMG is skipped because its setup cost is not amortised.\n"
        "Closed (pure-Neumann) meshes use twice this value. 0 = always build it.");
    pressureSolveForm->addRow("Minimum cells", m_pressureAmgThreshold);

    pressureLay->addStretch(1);

    // ── Pressure AMG ─────────────────────────────────────────────────────────
    // The tab name already scopes these, so no caption is needed.
    auto* pressureAmgLay = addScrollPage("Pressure AMG");
    m_pressureAmgWidget = new AmgConfigWidget(cfd::AmgSettings::pressureDefaults());
    pressureAmgLay->addWidget(m_pressureAmgWidget);
    pressureAmgLay->addStretch(1);

    // ── Momentum ─────────────────────────────────────────────────────────────
    // The AMG toggle and its hierarchy live together: the toggle is meaningless
    // without the settings it switches on.
    auto* momentumLay = addScrollPage("Momentum");
    auto* momentumForm = addGroup(momentumLay, "Momentum linear solver");

    m_useAMGMomentum = new QCheckBox("Use AMG for momentum");
    m_useAMGMomentum->setToolTip(
        "Momentum is a nonsymmetric convection-diffusion operator. Below the threshold\n"
        "BiCGSTAB+ILU wins, so this is off by default even though AMG scales better.");
    momentumForm->addRow(m_useAMGMomentum);

    m_momentumAmgThreshold = styleSpin(new QSpinBox);
    m_momentumAmgThreshold->setRange(0, 100000000);
    m_momentumAmgThreshold->setSingleStep(1000);
    m_momentumAmgThreshold->setSpecialValueText("always");
    m_momentumAmgThreshold->setToolTip(
        "Cell count below which the momentum solve stays on ILU regardless of the switch above.\n"
        "0 = always build it.");
    momentumForm->addRow("Minimum cells", m_momentumAmgThreshold);

    m_momentumAmgCycles = styleSpin(new QSpinBox);
    m_momentumAmgCycles->setRange(1, 100);
    m_momentumAmgCycles->setToolTip("Multigrid cycles per preconditioner application.");
    momentumForm->addRow("Cycles", m_momentumAmgCycles);

    m_momentumAmgMaxResidual = styleSpin(new ScientificDoubleSpinBox);
    m_momentumAmgMaxResidual->setRange(1e-16, 1e-1);
    m_momentumAmgMaxResidual->setDecimals(18);
    m_momentumAmgMaxResidual->setSingleStep(1e-9);
    m_momentumAmgMaxResidual->setToolTip(
        "Relative residual above which the AMG result is rejected and the solve is\n"
        "redone with ILU. Keep at or below the ILU tolerance.");
    momentumForm->addRow("Max residual", m_momentumAmgMaxResidual);

    m_momentumSolverStats = new QCheckBox("Momentum solver stats");
    m_momentumSolverStats->setToolTip("Print per-run momentum linear-solver statistics.");
    momentumForm->addRow(m_momentumSolverStats);

    auto* hierarchyCaption = dialogSectionHeader("Momentum AMG hierarchy");
    momentumLay->addWidget(hierarchyCaption);
    m_momentumAmgWidget = new AmgConfigWidget(cfd::AmgSettings::momentumDefaults());
    momentumLay->addWidget(m_momentumAmgWidget);
    momentumLay->addStretch(1);

    // ── Diagnostics ──────────────────────────────────────────────────────────
    auto* diagLay = addScrollPage("Diagnostics");
    auto* diagForm = addGroup(diagLay, "Residual monitoring");

    m_printFrequency = styleSpin(new QSpinBox);
    m_printFrequency->setRange(1, 100000);
    m_printFrequency->setToolTip("Print a residual row every N outer iterations.");
    diagForm->addRow("Print frequency", m_printFrequency);

    m_residualHistoryWindow = styleSpin(new QSpinBox);
    m_residualHistoryWindow->setRange(1, 10000);
    m_residualHistoryWindow->setToolTip(
        "Iterations inspected when watching for a continuity residual that keeps growing.");
    diagForm->addRow("Residual window", m_residualHistoryWindow);

    auto* perfForm = addGroup(diagLay, "Performance");
    m_numThreads = styleSpin(new QSpinBox);
    m_numThreads->setRange(0, 1024);
    m_numThreads->setSpecialValueText("auto");
    m_numThreads->setToolTip(
        "OpenMP threads used by the solver (matrix assembly, AMG, residuals).\n"
        "0 = runtime default (honours OMP_NUM_THREADS).");
    perfForm->addRow("Threads", m_numThreads);

    diagLay->addStretch(1);

    // ── Footer ───────────────────────────────────────────────────────────────
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults,
        m_advancedDlg);
    auto* restoreBtn = buttons->button(QDialogButtonBox::RestoreDefaults);
    restoreBtn->setText("Restore defaults");
    restoreBtn->setToolTip("Reset every setting on every tab of this dialog.");
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, m_advancedDlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, m_advancedDlg, &QDialog::reject);
    connect(restoreBtn, &QPushButton::clicked, this, [this] { applyDialogDefaults(); });

    // ── Dependencies ─────────────────────────────────────────────────────────
    connect(m_pressureUseAMG, &QCheckBox::toggled, this,
            [this](bool) { syncDialogDependencies(); });
    connect(m_limitPressureCorrection, &QCheckBox::toggled, this,
            [this](bool) { syncDialogDependencies(); });
    connect(m_useAMGMomentum, &QCheckBox::toggled, this,
            [this](bool) { syncDialogDependencies(); });

    // Seed every control from the shipped defaults, so the values the dialog
    // opens with and the values "Restore defaults" produces cannot drift.
    applyDialogDefaults();

    // Scrolling a page must not silently edit numeric fields under the cursor.
    installWheelGuard(m_advancedDlg);
}

void CfdCasePage::applyDialogDefaults() {
    // cfd::CaseSettings carries the defaults as member initialisers, which
    // makes it the single source of truth for both the solver and this dialog.
    const cfd::CaseSettings d;

    m_pressureCorrectors->setValue(d.pressureCorrectors);
    m_limitPressureCorrection->setChecked(d.limitPressureCorrection);
    m_pressureCorrectionLimit->setValue(d.pressureCorrectionLimit);
    m_pressureRefCell->setValue(static_cast<int>(d.pressureRefCell));
    m_pressureUseAMG->setChecked(d.pressureUseAMG);
    m_pressureUseAMGPreconditioner->setChecked(d.pressureUseAMGPreconditioner);
    m_pressureAmgThreshold->setValue(static_cast<int>(d.pressureAmgThreshold));
    m_pressureAmgWidget->resetToDefaults();

    m_useAMGMomentum->setChecked(d.useAMGMomentum);
    m_momentumAmgThreshold->setValue(static_cast<int>(d.momentumAmgThreshold));
    m_momentumAmgCycles->setValue(d.momentumAmgCycles);
    m_momentumAmgMaxResidual->setValue(d.momentumAmgMaxResidual);
    m_momentumSolverStats->setChecked(d.momentumSolverStats);
    m_momentumAmgWidget->resetToDefaults();

    m_printFrequency->setValue(static_cast<int>(d.printFrequency));
    m_residualHistoryWindow->setValue(static_cast<int>(d.residualHistoryWindow));
    m_numThreads->setValue(d.numThreads);

    // Checkbox signals only fire on an actual change, so sync explicitly.
    syncDialogDependencies();
}

void CfdCasePage::syncDialogDependencies() {
    const bool pressureAmg = m_pressureUseAMG->isChecked();
    m_pressureUseAMGPreconditioner->setEnabled(pressureAmg);
    m_pressureAmgThreshold->setEnabled(pressureAmg);
    m_pressureAmgWidget->setEnabled(pressureAmg);

    m_pressureCorrectionLimit->setEnabled(m_limitPressureCorrection->isChecked());

    const bool momentumAmg = m_useAMGMomentum->isChecked();
    m_momentumAmgThreshold->setEnabled(momentumAmg);
    m_momentumAmgCycles->setEnabled(momentumAmg);
    m_momentumAmgMaxResidual->setEnabled(momentumAmg);
    m_momentumAmgWidget->setEnabled(momentumAmg);

    // "Correctors" is usable in both steady (SIMPLE) and transient (PISO) runs.
    m_pressureCorrectors->setEnabled(true);
    m_pressureCorrectors->setToolTip("Pressure-corrector sweeps per iteration.");
}

void CfdCasePage::syncTimeDependencies() {
    const bool transient = m_transientRadio && m_transientRadio->isChecked();
    m_transientOptions->setVisible(transient);

    // Adaptive CFL only gates the CFL number; min/max dt still clamp a fixed dt.
    const bool adaptive = m_adaptiveCflCb->isChecked();
    m_cflSpin->setEnabled(transient && adaptive);
    m_minDtSpin->setEnabled(transient);
    m_maxDtSpin->setEnabled(transient);

    // PISO makes these inert. Reflect that honestly instead of silently ignoring.
    const bool steady = !transient;
    m_alphaU->setEnabled(steady);
    m_alphaP->setEnabled(steady);
    m_rhie->setEnabled(steady);
    if (transient) {
        m_rhie->setChecked(true);
        m_alphaU->setToolTip("Forced to 1.0 by PISO in transient mode.");
        m_alphaP->setToolTip("Forced to 1.0 by PISO in transient mode.");
        m_rhie->setToolTip("Forced on by PISO in transient mode.");
    } else {
        m_alphaU->setToolTip("");
        m_alphaP->setToolTip("");
        m_rhie->setToolTip("");
    }
    m_maxIter->setEnabled(steady);
    m_maxIter->setToolTip(transient
        ? "Not used in transient mode: the run marches to \"End time\"."
        : "");

    // Max iterations / tolerance labels: tolerance is the per-step corrector target.
    m_tol->setToolTip(transient
        ? "Per-step corrector convergence target in transient (PISO) mode."
        : "");

    // Keep the dialog's gating in sync too.
    syncDialogDependencies();
}

void CfdCasePage::setRunning(bool running) {
    m_running = running;
    if (m_runBtn) m_runBtn->setEnabled(!running);
    if (m_initBtn) m_initBtn->setEnabled(!running);
    if (m_pauseBtn) m_pauseBtn->setEnabled(running);
    if (m_stepBtn) m_stepBtn->setEnabled(running && m_ctrl && m_ctrl->isPausedSettled());
    if (m_cancelBtn) {
        m_cancelBtn->setText("Cancel");
        m_cancelBtn->setEnabled(running);
    }
    if (m_advancedBtn) m_advancedBtn->setEnabled(!running);
    if (m_bcEditBtn) m_bcEditBtn->setEnabled(!running && !m_patchBCs.empty());
    if (!running) {
        m_pauseSettledLogged = false;
        if (m_pauseBtn) m_pauseBtn->setText("Pause");
        if (m_stateTimer) m_stateTimer->stop();
    } else if (m_stateTimer) {
        m_stateTimer->start();
    }
}

void CfdCasePage::onLoadMesh() {
    if (m_running) {
        log("Stop the current run before loading a mesh.");
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, "Load CFD Mesh", QString(), "Mesh (*.msh *.vtk *.vtu);;All files (*)");
    if (path.isEmpty()) return;
    log("Loading " + QFileInfo(path).fileName() + "...");
    m_ctrl->loadMesh(path); // emits meshLoaded() (or failed()) synchronously
}

void CfdCasePage::log(const QString& msg) {
    std::cout << msg.toStdString() << std::endl;
}

void CfdCasePage::applySettings(const cfd::CaseSettings& s) {
    m_rho->setText(QString::number(s.rho));
    m_nu->setText(QString::number(s.nu));
    m_tol->setValue(s.tolerance);
    m_maxIter->setValue((int)s.maxIterations);
    m_alphaU->setValue(s.alphaU);
    m_alphaP->setValue(s.alphaP);
    m_simplec->setChecked(s.simplec);
    m_scheme->setCurrentIndex(s.scheme);
    m_gradientScheme->setCurrentIndex(s.gradientScheme);
    m_rhie->setChecked(s.rhieChow);
    m_turb->setChecked(s.turbulence);

    m_pressureCorrectors->setValue(s.pressureCorrectors);
    m_limitPressureCorrection->setChecked(s.limitPressureCorrection);
    m_pressureCorrectionLimit->setValue(s.pressureCorrectionLimit);
    m_pressureRefCell->setValue((int)s.pressureRefCell);
    m_printFrequency->setValue((int)s.printFrequency);
    m_residualHistoryWindow->setValue((int)s.residualHistoryWindow);
    m_numThreads->setValue(s.numThreads);

    m_pressureUseAMG->setChecked(s.pressureUseAMG);
    m_pressureUseAMGPreconditioner->setChecked(s.pressureUseAMGPreconditioner);
    m_pressureAmgThreshold->setValue((int)s.pressureAmgThreshold);
    m_pressureAmgWidget->setConfig(s.pressureAmg);

    m_useAMGMomentum->setChecked(s.useAMGMomentum);
    m_momentumAmgThreshold->setValue((int)s.momentumAmgThreshold);
    m_momentumAmgCycles->setValue(s.momentumAmgCycles);
    m_momentumAmgMaxResidual->setValue(s.momentumAmgMaxResidual);
    m_momentumSolverStats->setChecked(s.momentumSolverStats);
    m_momentumAmgWidget->setConfig(s.momentumAmg);

    m_transientRadio->setChecked(s.transient.enabled);
    m_dtSpin->setValue(s.transient.dt);
    m_tEndSpin->setValue(s.transient.tEnd);
    m_timeSchemeCombo->setCurrentIndex(s.transient.timeScheme == cfd::TransientSettings::TimeScheme::BDF2 ? 1 : 0);
    m_adaptiveCflCb->setChecked(s.transient.adaptiveCfl);
    m_cflSpin->setValue(s.transient.cfl);
    m_minDtSpin->setValue(s.transient.minDt);
    m_maxDtSpin->setValue(s.transient.maxDt);
    m_writeIntervalSpin->setValue(s.transient.writeInterval);
    m_outerCorrectorsSpin->setValue(s.transient.maxOuterCorrectors);
    m_nonOrthCorrectorsSpin->setValue(s.transient.nonOrthogonalCorrectors);
    m_pvdFileEdit->setText(QString::fromStdString(s.transient.pvdFile));

    syncDialogDependencies();
    syncTimeDependencies();
}

void CfdCasePage::onMeshLoaded() {
    if (!m_ctrl->meshName().isEmpty()) m_meshName = m_ctrl->meshName();
    rebuildPatchTable();
    const auto& res = m_ctrl->meshResult();
    if (res.renderMesh) {
        S()->applyLoadedMesh(res.renderMesh, m_meshName.isEmpty() ? "CFD Mesh" : m_meshName);
    }
    log(QString("%1 — %2D, %3 patches")
                          .arg(m_meshName.isEmpty() ? "Mesh" : m_meshName)
                          .arg(res.dim)
                          .arg(res.patches.size()));
    setRunning(m_running);
}

void CfdCasePage::rebuildPatchTable() {
    m_patchBCs.clear();
    for (const auto& patch : m_ctrl->meshResult().patches) {
        cfd::PatchBC pbc;
        pbc.patchName = patch.name;
        pbc.type = cfd::BCType::WallNoSlip;
        m_patchBCs.push_back(pbc);
    }
}

void CfdCasePage::collectSettings() {
    cfd::CaseSettings s;
    s.rho = m_rho->text().toDouble();
    s.nu = m_nu->text().toDouble();
    s.tolerance = m_tol->value();
    s.maxIterations = static_cast<std::size_t>(m_maxIter->value());
    s.alphaU = m_alphaU->value();
    s.alphaP = m_alphaP->value();
    s.simplec = m_simplec->isChecked();
    s.scheme = m_scheme->currentIndex();
    s.gradientScheme = m_gradientScheme->currentIndex();
    s.rhieChow = m_rhie->isChecked();
    s.turbulence = m_turb->isChecked();
    s.kInf = 1e-3;
    s.omegaInf = 1.0;
    // Advanced
    s.pressureCorrectors = m_pressureCorrectors->value();
    s.limitPressureCorrection = m_limitPressureCorrection->isChecked();
    s.pressureCorrectionLimit = m_pressureCorrectionLimit->value();
    s.pressureRefCell = static_cast<std::size_t>(m_pressureRefCell->value());
    s.printFrequency = static_cast<std::size_t>(m_printFrequency->value());
    s.residualHistoryWindow = static_cast<std::size_t>(m_residualHistoryWindow->value());
    s.numThreads = m_numThreads->value();

    // Pressure linear solver
    s.pressureUseAMG = m_pressureUseAMG->isChecked();
    s.pressureUseAMGPreconditioner = m_pressureUseAMGPreconditioner->isChecked();
    s.pressureAmgThreshold = static_cast<std::size_t>(m_pressureAmgThreshold->value());
    s.pressureAmg = m_pressureAmgWidget->config();

    // Momentum linear solver
    s.useAMGMomentum = m_useAMGMomentum->isChecked();
    s.momentumAmgThreshold = static_cast<std::size_t>(m_momentumAmgThreshold->value());
    s.momentumAmgCycles = m_momentumAmgCycles->value();
    s.momentumAmgMaxResidual = m_momentumAmgMaxResidual->value();
    s.momentumSolverStats = m_momentumSolverStats->isChecked();
    s.momentumAmg = m_momentumAmgWidget->config();

    // Time mode (steady / transient)
    s.transient.enabled = m_transientRadio->isChecked();
    s.transient.dt = m_dtSpin->value();
    s.transient.tEnd = m_tEndSpin->value();
    s.transient.timeScheme = (m_timeSchemeCombo->currentIndex() == 1)
        ? cfd::TransientSettings::TimeScheme::BDF2
        : cfd::TransientSettings::TimeScheme::BackwardEuler;
    s.transient.adaptiveCfl = m_adaptiveCflCb->isChecked();
    s.transient.cfl = m_cflSpin->value();
    s.transient.minDt = m_minDtSpin->value();
    s.transient.maxDt = m_maxDtSpin->value();
    s.transient.writeInterval = m_writeIntervalSpin->value();
    s.transient.maxOuterCorrectors = m_outerCorrectorsSpin->value();
    s.transient.nonOrthogonalCorrectors = m_nonOrthCorrectorsSpin->value();
    s.transient.pvdFile = m_pvdFileEdit->text().toStdString();

    m_ctrl->setSettings(s);
    m_ctrl->setPatchBCs(m_patchBCs);
}

void CfdCasePage::onEditBCs() {
    const auto& res = m_ctrl->meshResult();
    if (m_patchBCs.empty() && !res.patches.empty()) rebuildPatchTable();
    if (m_patchBCs.empty()) {
        log("Load a mesh first.");
        return;
    }

    std::vector<std::size_t> faceCounts;
    faceCounts.reserve(res.patches.size());
    for (const auto& patch : res.patches) faceCounts.push_back(patch.faceCount);

    CfdBoundaryConditionsDialog dlg(m_patchBCs, faceCounts, m_turb->isChecked(), this);
    if (dlg.exec() != QDialog::Accepted) return;

    m_patchBCs = dlg.patchBCs();
    log(QString("Boundary conditions updated (%1 patches).").arg(m_patchBCs.size()));
}

void CfdCasePage::onRun() {
    if (m_running) return;
    collectSettings();
    // Capture the extraction config BY VALUE on the GUI thread. The render-prep
    // worker then runs a pure function and never reads RenderSettings members
    // while the GUI is publishing frames (that race was the streaming crash).
    const postfields::SnapshotRequest postCfg = S()->postSnapshotRequest();
    m_ctrl->setPostExtractor([postCfg](const RenderMesh& mesh) {
        return postfields::extractSnapshot(mesh, postCfg);
    });
    // Use per-frame min/max during CFD transient streaming so each time step
    // shows its own full range instead of an accumulated union.
    m_savedAnimGlobal = S()->getAnimScaleGlobal();
    S()->setAnimScaleGlobal(false);
    // Reseed the colormap range on the first transient frame instead of
    // inheriting a stale range from a previous run/animation.
    S()->resetAnimationSequence();
    S()->clearPostFields();
    m_lastFrame.reset();
    m_pauseSettledLogged = false;

    if (!m_ctrl->run()) {
        // run() already emitted failed() (which restored the animation scale).
        return;
    }
    setRunning(true);
    m_iterValue->setText(QString::number(m_ctrl->currentStep()));
    m_contValue->setText("--");
    m_momValue->setText("--");
    // Only transient runs publish frames; a steady solve has no frame bus.
    if (m_frameTimer && m_ctrl->settings().transient.enabled) m_frameTimer->start();
    log(m_ctrl->settings().transient.enabled ? "Running (transient)..." : "Running...");
}

void CfdCasePage::onCancel() {
    if (!m_running) return;
    m_ctrl->cancel();
    if (m_cancelBtn) {
        m_cancelBtn->setText("Cancelling...");
        m_cancelBtn->setEnabled(false);
    }
    if (m_pauseBtn) m_pauseBtn->setEnabled(false);
    if (m_stepBtn) m_stepBtn->setEnabled(false);
    log("Cancelling... (the current step finishes first)");
}

void CfdCasePage::onPause() {
    if (!m_running) return;
    if (m_ctrl->isPaused()) {
        m_ctrl->resume();
        m_pauseSettledLogged = false;
        log("Resumed.");
    } else {
        m_ctrl->pause();
        log("Pause requested; stopping at the next step boundary.");
    }
}

void CfdCasePage::onStep() {
    if (!m_running) return;
    if (!m_ctrl->isPaused()) {
        log("Pause the run before stepping.");
        return;
    }
    m_ctrl->step();
    m_pauseSettledLogged = false;
    log(m_ctrl->settings().transient.enabled ? "Stepping one time step." : "Stepping one iteration.");
}

void CfdCasePage::onInitialize() {
    if (m_running) {
        log("Stop the current run before initializing.");
        return;
    }
    if (!m_ctrl->meshResult().renderMesh) {
        log("Load a mesh first.");
        return;
    }
    const int dim = m_ctrl->meshResult().dim;
    collectSettings();

    QDialog dlg(this);
    dlg.setWindowTitle("Initialize Solution");
    auto* lay = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout;
    auto* ux = new QDoubleSpinBox(&dlg); ux->setRange(-1e6, 1e6); ux->setDecimals(4); ux->setValue(0.0);
    auto* uy = new QDoubleSpinBox(&dlg); uy->setRange(-1e6, 1e6); uy->setDecimals(4); uy->setValue(0.0);
    auto* uz = new QDoubleSpinBox(&dlg); uz->setRange(-1e6, 1e6); uz->setDecimals(4); uz->setValue(0.0);
    auto* p  = new QDoubleSpinBox(&dlg); p->setRange(-1e6, 1e6); p->setDecimals(4); p->setValue(0.0);
    auto* k  = new QDoubleSpinBox(&dlg); k->setRange(1e-12, 1e3); k->setDecimals(4); k->setValue(1e-3);
    auto* w  = new QDoubleSpinBox(&dlg); w->setRange(1e-6, 1e6); w->setDecimals(4); w->setValue(1.0);
    form->addRow("U_x", ux);
    form->addRow("U_y", uy);
    if (dim >= 3) form->addRow("U_z", uz);
    form->addRow("p", p);
    form->addRow("k", k);
    form->addRow("omega", w);
    lay->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    cfd::InitConditions ic;
    ic.ux = ux->value();
    ic.uy = uy->value();
    ic.uz = dim >= 3 ? uz->value() : 0.0;
    ic.pressure = p->value();
    ic.k = k->value();
    ic.omega = w->value();

    auto rm = m_ctrl->initialize(ic);
    if (!rm) {
        log(m_ctrl->lastError().isEmpty() ? "No mesh loaded." : m_ctrl->lastError());
        return;
    }
    S()->applyLoadedMesh(rm, "cfd_solution");
    const QString velocity = dim >= 3
        ? QString("U=(%1,%2,%3)").arg(ic.ux).arg(ic.uy).arg(ic.uz)
        : QString("U=(%1,%2)").arg(ic.ux).arg(ic.uy);
    log(QString("Initialized: %1, p=%2.").arg(velocity).arg(ic.pressure));
    m_iterValue->setText("0");
    m_contValue->setText("--");
    m_momValue->setText("--");
}

void CfdCasePage::onExport() {
    if (m_running && !(m_ctrl->isPaused() && m_ctrl->isPausedSettled())) {
        log("Pause the run before exporting.");
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, "Export Solution", "cfd_solution.vtu",
                                                      "VTK (*.vtu)");
    if (path.isEmpty()) return;
    m_ctrl->exportVtu(path);
    // On failure the controller emits failed(), which onFailed() already logs.
    if (m_ctrl->lastError().isEmpty()) log("Exported " + QFileInfo(path).fileName());
}

void CfdCasePage::onSaveCase() {
    if (m_running && !(m_ctrl->isPaused() && m_ctrl->isPausedSettled())) {
        log("Pause the run before saving; the file is written once the solver stops at a step boundary.");
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, "Save Case", "cfd_case.scfd",
                                                      "CFD Case (*.scfd)");
    if (path.isEmpty()) return;
    // While a run is active the inputs may have been edited but not applied:
    // persist the settings the solver is actually using, not the edit buffer.
    if (!m_running) collectSettings();
    if (m_ctrl->saveCase(path)) log("Saved " + QFileInfo(path).fileName());
    else log("Save failed: " + m_ctrl->lastError());
}

void CfdCasePage::onLoadCase() {
    if (m_running) {
        log("Stop the current run before loading a case.");
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, "Load Case", QString(),
                                                      "CFD Case (*.scfd)");
    if (path.isEmpty()) return;
    if (m_ctrl->loadCase(path)) {
        log("Loaded " + QFileInfo(path).fileName());
    } else {
        log("Load failed: " + m_ctrl->lastError());
    }
}

void CfdCasePage::onCaseLoaded() {
    m_meshName = m_ctrl->meshName();
    m_patchBCs = m_ctrl->patchBCs();
    applySettings(m_ctrl->settings());

    // loadCase() deliberately emits only caseLoaded(): apply the mesh exactly
    // once here instead of once per signal.
    auto rm = m_ctrl->resultMesh();
    if (rm) {
        S()->applyLoadedMesh(rm, m_meshName.isEmpty() ? "CFD Result" : m_meshName);
    } else {
        onMeshLoaded();
    }

    const auto& s = m_ctrl->settings();
    if (s.transient.enabled) {
        const int step = m_ctrl->currentStep();
        const double t = m_ctrl->currentTime();
        m_iterValue->setText(QString::number(step));
        if (step > 0 || t > 0.0) {
            log(QString("Loaded step %1, t = %2 s (Run continues from here)").arg(step).arg(t, 0, 'g', 8));
        } else {
            log("Loaded case (fresh transient start at t = 0).");
        }
    } else {
        m_iterValue->setText("0");
        m_contValue->setText("--");
        m_momValue->setText("--");
        log(QString("Loaded case: %1").arg(m_meshName));
    }
    setRunning(false);
}

void CfdCasePage::onProgress(int iteration, double continuity, double momentum) {
    m_iterValue->setText(QString::number(iteration));
    m_contValue->setText(QString::number(continuity, 'e', 3));
    m_momValue->setText(QString::number(momentum, 'e', 3));
}

void CfdCasePage::onTransientFrame(std::shared_ptr<const RenderMesh> mesh, int step, double time) {
    if (m_ctrl && m_ctrl->isCancelled()) return;
    if (!mesh) return;
    // Preserve the user's scalar-color toggle: onAnimationFrame's first-frame
    // block disables it by default (animation UX), which would blank the
    // colormap mid-run.
    const bool hadScalarColor = S()->getMeshUseScalarColor();
    // Light per-frame publish: keeps the camera and the user's field selection.
    S()->onAnimationFrame(mesh, step, time);
    S()->setMeshUseScalarColor(hadScalarColor);
    m_iterValue->setText(QString::number(step));
    log(QString("Transient: step %1, t = %2 s").arg(step).arg(time, 0, 'g', 6));
}

void CfdCasePage::onFinished(const QString& summary) {
    if (m_frameTimer) m_frameTimer->stop();
    m_lastFrame.reset();
    setRunning(false);
    S()->setAnimScaleGlobal(m_savedAnimGlobal);
    // Show where the run actually stopped (a paused/cancelled transient may not
    // have reached the last frame the monitor saw).
    if (m_ctrl->settings().transient.enabled) {
        const int step = m_ctrl->currentStep();
        const double t = m_ctrl->currentTime();
        if (step > 0 || t > 0.0) {
            m_iterValue->setText(QString::number(step));
            log(QString("%1 — step %2, t = %3 s").arg(summary).arg(step).arg(t, 0, 'g', 8));
        } else {
            log(summary);
        }
    } else {
        log(summary);
    }
    const bool cancelled = summary == "Cancelled";
    auto rm = m_ctrl->resultMesh();
    if (!rm) { log("(no result mesh)"); return; }
    try {
        S()->applyLoadedMesh(rm, "cfd_solution", !cancelled);
        log(cancelled ? "Cancelled (visualized)" : "(visualized)");
    } catch (const std::exception& e) {
        log(QString("Visualize failed: %1").arg(e.what()));
    }
}

void CfdCasePage::onFailed(const QString& error) {
    if (m_frameTimer) m_frameTimer->stop();
    m_lastFrame.reset();
    setRunning(false);
    S()->setAnimScaleGlobal(m_savedAnimGlobal);
    log("Failed: " + error);
}

} // namespace sidebar
