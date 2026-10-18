#include "ui/pages/cfd_case_page.h"

#include "cfd/cfd_types.h"
#include "render/settings/render_settings.h"
#include "ui/cfd_bc_dialog.h"
#include "ui/controllers/cfd_controller.h"
#include "ui/panel_helpers.h"
#include "ui/widgets/amg_config_widget.h"
#include "ui/widgets/scientific_spin_box.h"
#include "ui/widgets/wheel_guard.h"
#include "ui_cfd_case_page.h"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
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
    connect(m_ctrl, &CfdController::progress, this, &CfdCasePage::onProgress);
    connect(m_ctrl, &CfdController::finished, this, &CfdCasePage::onFinished);
    connect(m_ctrl, &CfdController::failed, this, &CfdCasePage::onFailed);

    // ── Widgets ──────────────────────────────────────────────────────────────
    m_status = cfdUi.statusText;
    m_iterValue = cfdUi.iterValue;
    m_contValue = cfdUi.contValue;
    m_momValue = cfdUi.momValue;

    m_loadBtn = cfdUi.loadBtn;
    m_bcEditBtn = cfdUi.bcEditBtn;
    m_advancedBtn = cfdUi.advancedBtn;
    m_initBtn = cfdUi.initBtn;
    m_runBtn = cfdUi.runBtn;
    m_cancelBtn = cfdUi.cancelBtn;
    m_exportBtn = cfdUi.exportBtn;

    m_rho = cfdUi.rhoSpin;
    m_nu = cfdUi.nuSpin;
    m_tol = cfdUi.tolSpin;
    m_maxIter = cfdUi.maxIterSpin;
    m_alphaU = cfdUi.alphaUSpin;
    m_alphaP = cfdUi.alphaPSpin;
    m_simplec = cfdUi.simplecCb;
    m_scheme = cfdUi.schemeCombo;
    m_rhie = cfdUi.rhieCb;
    m_turb = cfdUi.turbCb;
    m_kInf = cfdUi.kInfSpin;
    m_wInf = cfdUi.wInfSpin;

    // ── Physics defaults ─────────────────────────────────────────────────────
    m_rho->setRange(1e-6, 1e6);    m_rho->setDecimals(4); m_rho->setValue(1.0);
    m_nu->setRange(1e-12, 1e3);    m_nu->setDecimals(4);  m_nu->setValue(0.01);
    m_kInf->setRange(1e-12, 1e3);  m_kInf->setDecimals(4); m_kInf->setValue(1e-3);
    m_wInf->setRange(1e-6, 1e6);   m_wInf->setDecimals(4); m_wInf->setValue(1.0);

    // ── Solver defaults ──────────────────────────────────────────────────────
    // The tolerance spin box displays scientific notation (see
    // ScientificDoubleSpinBox) but keeps a high decimal precision so values
    // below 1e-3 stay representable and the 1e-6 arrow step stays visible.
    m_tol->setRange(1e-7, 1e-1);   m_tol->setDecimals(9);  m_tol->setValue(1e-3);
    m_tol->setSingleStep(1e-6);
    m_tol->setCorrectionMode(QDoubleSpinBox::CorrectToPreviousValue);
    m_maxIter->setRange(1, 100000); m_maxIter->setValue(2000);
    m_alphaU->setRange(0.01, 1.0); m_alphaU->setDecimals(2); m_alphaU->setValue(0.7);
    m_alphaP->setRange(0.01, 1.0); m_alphaP->setDecimals(2); m_alphaP->setValue(0.3);
    m_scheme->addItems({"Upwind", "Central", "Second-Order Upwind"});
    m_scheme->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_scheme->setMinimumContentsLength(8);
    m_rhie->setChecked(true);

    // k / omega are only meaningful with turbulence enabled.
    auto syncTurbulence = [this]() {
        const bool on = m_turb->isChecked();
        m_kInf->setEnabled(on);
        m_wInf->setEnabled(on);
    };
    connect(m_turb, &QCheckBox::toggled, this, [syncTurbulence](bool) { syncTurbulence(); });
    syncTurbulence();

    // ── Advanced solver settings (dialog) ────────────────────────────────────
    // Built up front and kept hidden. Its fields are read by collectSettings(),
    // so they must exist from the start; creating them as children of the page
    // and reparenting them only on first open would leave them painted over the
    // top-left corner of the panel (a parented widget with no layout floats).
    buildAdvancedDialog();
    connect(m_advancedBtn, &QPushButton::clicked, this, [this] { m_advancedDlg->exec(); });
    // ── Read-out boxes ───────────────────────────────────────────────────────
    m_status->setStyleSheet(QString("QLabel { %1 padding: 6px 8px; }").arg(readoutStyle()));

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
    connect(m_loadBtn, &QPushButton::clicked, this, &CfdCasePage::onLoadMesh);
    connect(m_bcEditBtn, &QPushButton::clicked, this, &CfdCasePage::onEditBCs);
    connect(m_runBtn, &QPushButton::clicked, this, &CfdCasePage::onRun);
    connect(m_cancelBtn, &QPushButton::clicked, this, &CfdCasePage::onCancel);
    connect(m_initBtn, &QPushButton::clicked, this, &CfdCasePage::onInitialize);
    connect(m_exportBtn, &QPushButton::clicked, this, &CfdCasePage::onExport);

    m_status->setText("No mesh loaded.");
    setRunning(false);

    qobject_cast<QVBoxLayout*>(content->layout())->addStretch();
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
}

void CfdCasePage::setRunning(bool running) {
    m_running = running;
    if (m_runBtn) m_runBtn->setEnabled(!running);
    if (m_initBtn) m_initBtn->setEnabled(!running);
    if (m_cancelBtn) m_cancelBtn->setEnabled(running);
    if (m_loadBtn) m_loadBtn->setEnabled(!running);
    if (m_advancedBtn) m_advancedBtn->setEnabled(!running);
    if (m_bcEditBtn) m_bcEditBtn->setEnabled(!running && !m_patchBCs.empty());
}

void CfdCasePage::onLoadMesh() {
    const QString path = QFileDialog::getOpenFileName(
        this, "Load CFD Mesh", QString(), "Mesh (*.msh *.vtk *.vtu);;All files (*)");
    if (path.isEmpty()) return;
    m_meshName = QFileInfo(path).fileName();
    m_status->setText("Loading " + m_meshName + "...");
    m_ctrl->loadMesh(path); // emits meshLoaded() (or failed()) synchronously
}

void CfdCasePage::onMeshLoaded() {
    rebuildPatchTable();
    const auto& res = m_ctrl->meshResult();
    if (res.renderMesh) {
        S()->applyLoadedMesh(res.renderMesh, m_meshName.isEmpty() ? "CFD Mesh" : m_meshName);
    }
    m_status->setText(QString("%1 — %2 patches")
                          .arg(m_meshName.isEmpty() ? "Mesh" : m_meshName)
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
    s.rho = m_rho->value();
    s.nu = m_nu->value();
    s.tolerance = m_tol->value();
    s.maxIterations = static_cast<std::size_t>(m_maxIter->value());
    s.alphaU = m_alphaU->value();
    s.alphaP = m_alphaP->value();
    s.simplec = m_simplec->isChecked();
    s.scheme = m_scheme->currentIndex();
    s.rhieChow = m_rhie->isChecked();
    s.turbulence = m_turb->isChecked();
    s.kInf = m_kInf->value();
    s.omegaInf = m_wInf->value();
    // Advanced
    s.pressureCorrectors = m_pressureCorrectors->value();
    s.limitPressureCorrection = m_limitPressureCorrection->isChecked();
    s.pressureCorrectionLimit = m_pressureCorrectionLimit->value();
    s.pressureRefCell = static_cast<std::size_t>(m_pressureRefCell->value());
    s.printFrequency = static_cast<std::size_t>(m_printFrequency->value());
    s.residualHistoryWindow = static_cast<std::size_t>(m_residualHistoryWindow->value());

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

    m_ctrl->setSettings(s);
    m_ctrl->setPatchBCs(m_patchBCs);
}

void CfdCasePage::onEditBCs() {
    const auto& res = m_ctrl->meshResult();
    if (m_patchBCs.empty() && !res.patches.empty()) rebuildPatchTable();
    if (m_patchBCs.empty()) {
        m_status->setText("Load a mesh first.");
        return;
    }

    std::vector<std::size_t> faceCounts;
    faceCounts.reserve(res.patches.size());
    for (const auto& patch : res.patches) faceCounts.push_back(patch.faceCount);

    CfdBoundaryConditionsDialog dlg(m_patchBCs, faceCounts, this);
    if (dlg.exec() != QDialog::Accepted) return;

    m_patchBCs = dlg.patchBCs();
    m_status->setText(QString("Boundary conditions updated (%1 patches).").arg(m_patchBCs.size()));
}

void CfdCasePage::onRun() {
    if (m_running) return;
    collectSettings();
    setRunning(true);
    m_status->setText("Running...");
    m_iterValue->setText("0");
    m_contValue->setText("--");
    m_momValue->setText("--");
    m_ctrl->run();
}

void CfdCasePage::onCancel() {
    if (!m_running) return;
    m_ctrl->cancel();
    m_status->setText("Cancelling...");
}

void CfdCasePage::onInitialize() {
    if (m_running) {
        m_status->setText("Stop the current run before initializing.");
        return;
    }
    if (!m_ctrl->meshResult().mesh2) {
        m_status->setText("Load a mesh first.");
        return;
    }
    collectSettings();
    auto rm = m_ctrl->initialize();
    if (!rm) {
        m_status->setText("No mesh loaded.");
        return;
    }
    S()->applyLoadedMesh(rm, "cfd_solution");
    m_status->setText("Initialized: U = 0, p = 0.");
    m_iterValue->setText("0");
    m_contValue->setText("--");
    m_momValue->setText("--");
}

void CfdCasePage::onExport() {
    const QString path = QFileDialog::getSaveFileName(this, "Export Solution", "cfd_solution.vtu",
                                                      "VTK (*.vtu)");
    if (path.isEmpty()) return;
    m_ctrl->exportVtu(path);
    m_status->setText("Exported " + QFileInfo(path).fileName());
}

void CfdCasePage::onProgress(int iteration, double continuity, double momentum) {
    m_iterValue->setText(QString::number(iteration));
    m_contValue->setText(QString::number(continuity, 'e', 3));
    m_momValue->setText(QString::number(momentum, 'e', 3));
}

void CfdCasePage::onFinished(const QString& summary) {
    setRunning(false);
    m_status->setText(summary);
    auto rm = m_ctrl->resultMesh();
    if (rm) S()->applyLoadedMesh(rm, "cfd_solution");
}

void CfdCasePage::onFailed(const QString& error) {
    setRunning(false);
    m_status->setText("Failed: " + error);
}

} // namespace sidebar
