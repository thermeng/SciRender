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
#include <QButtonGroup>
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
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
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

// Muted explanatory text under a group of controls.
QLabel* dialogNote(const QString& text) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setStyleSheet("color: palette(mid);");
    QFont f = label->font();
    f.setPixelSize(10);
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
    connect(m_ctrl, &CfdController::progress, this, &CfdCasePage::onProgress);
    connect(m_ctrl, &CfdController::residualsUpdated, this, [this](int iter, int dim, double cont,
                                                                   double momU, double momV, double momW,
                                                                   double k, double omega, double maxYPlus,
                                                                   bool hasTurb) {
        emit residualSeriesUpdated(iter, dim, cont, momU, momV, momW, k, omega, maxYPlus, hasTurb);
        if (hasTurb && maxYPlus > 50.0) {
            log(QString("WARNING: max y+ = %1 — wall-adjacent cells are inside the log layer.\n"
                        "Refine the mesh near walls or enable wall functions for reliable turbulence results.")
                .arg(maxYPlus, 0, 'g', 3));
        }
    });
    connect(m_ctrl, &CfdController::finished, this, &CfdCasePage::onFinished);
    connect(m_ctrl, &CfdController::failed, this, &CfdCasePage::onFailed);

    // ── Widgets ──────────────────────────────────────────────────────────────
    m_iterValue = cfdUi.iterValue;
    m_contValue = cfdUi.contValue;
    m_momValue = cfdUi.momValue;

    m_bcEditBtn = cfdUi.bcEditBtn;
    m_advancedBtn = cfdUi.advancedBtn;
    m_stoppingBtn = cfdUi.stoppingBtn;
    m_initBtn = cfdUi.initBtn;
    m_runBtn = cfdUi.runBtn;
    m_cancelBtn = cfdUi.cancelBtn;
    m_pauseBtn = cfdUi.pauseBtn;
    m_stepBtn = cfdUi.stepBtn;

    m_rho = cfdUi.rhoSpin;
    m_nu = cfdUi.nuSpin;
    // Constrain density and viscosity to finite positive numbers.
    auto* posDoubleVal = new QDoubleValidator(1e-12, 1e12, 10, m_rho);
    posDoubleVal->setNotation(QDoubleValidator::StandardNotation);
    m_rho->setValidator(posDoubleVal);
    m_nu->setValidator(posDoubleVal);
    m_alphaU = cfdUi.alphaUSpin;
    m_alphaP = cfdUi.alphaPSpin;
    m_simplec = cfdUi.simplecCb;
    m_scheme = cfdUi.schemeCombo;
    m_gradientScheme = cfdUi.gradientSchemeCombo;
    // Rhie-Chow is always on for collocated grids (no UI toggle).
    m_turb = cfdUi.turbCb;

    m_alphaK = cfdUi.alphaKSpin;
    m_alphaW = cfdUi.alphaWSpin;
    m_wallFuncCb = cfdUi.wallFuncCb;

    // Wall functions are not implemented for the low-Re SST formulation, so the
    // toggle stays hidden: it is a non-functional affordance until a high-Re
    // path exists.
    m_alphaK->setToolTip("Turbulent kinetic energy dissipation under-relaxation.\n"
                         "Typical SST value: 0.7 (range 0.01 - 1.0).");
    m_alphaW->setToolTip("Specific dissipation rate under-relaxation.\n"
                         "Typical SST value: 0.7 (range 0.01 - 1.0).");

    if (m_alphaK && m_alphaK->parentWidget()) m_alphaK->parentWidget()->setVisible(m_turb->isChecked());
    if (m_alphaW && m_alphaW->parentWidget()) m_alphaW->parentWidget()->setVisible(m_turb->isChecked());
    if (m_wallFuncCb && m_wallFuncCb->parentWidget()) m_wallFuncCb->parentWidget()->setVisible(m_turb->isChecked());

    connect(m_turb, &QCheckBox::toggled, this, [this](bool on) {
        if (m_alphaK && m_alphaK->parentWidget()) m_alphaK->parentWidget()->setVisible(on);
        if (m_alphaW && m_alphaW->parentWidget()) m_alphaW->parentWidget()->setVisible(on);
        if (m_wallFuncCb && m_wallFuncCb->parentWidget()) m_wallFuncCb->parentWidget()->setVisible(on);
    });
    // Turbulence freestream values are now edited in the Initialize dialog.

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

    // tEnd and maxOuterCorrectors now live exclusively in the Stopping
    // Criteria dialog (D1); hide their rows here so they are not edited in
    // two places.
    cfdUi.tEndRow->setVisible(false);
    cfdUi.outerCorrectorsRow->setVisible(false);

    // ── Physics defaults ─────────────────────────────────────────────────────
    m_rho->setText("1.0");
    m_nu->setText("0.01");
    // ── Solver defaults ──────────────────────────────────────────────────────
    m_alphaU->setRange(0.01, 1.0); m_alphaU->setDecimals(2); m_alphaU->setValue(0.7);
    m_alphaP->setRange(0.01, 1.0); m_alphaP->setDecimals(2); m_alphaP->setValue(0.3);
    m_scheme->addItems({"Upwind", "Central", "Second-Order Upwind"});
    m_scheme->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_scheme->setMinimumContentsLength(8);
    m_gradientScheme->addItems({"Least Squares", "Green-Gauss Cell", "Green-Gauss Node"});
    m_gradientScheme->setCurrentIndex(0);
    // Rhie-Chow is always on (no toggle).

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
        if (!S() || !m_ctrl || !m_running) return;
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

    // Poll the solver's pause state so the Pause/Step buttons reflect what the
    // solver is actually doing (pause settles asynchronously at a boundary).
    m_stateTimer = new QTimer(this);
    m_stateTimer->setInterval(100);
    connect(m_stateTimer, &QTimer::timeout, this, [this]() {
        if (!m_running) { m_stateTimer->stop(); return; }
        const bool paused = m_ctrl->isPaused();
        const bool settled = m_ctrl->isPausedSettled();
        if (m_pauseBtn)
            m_pauseBtn->setText(paused ? (settled ? "Resume" : "Pausing...") : "Pause");
        if (m_stepBtn) m_stepBtn->setEnabled(settled);
        if (m_cancelBtn && !m_ctrl->isCancelled()) m_cancelBtn->setEnabled(true);
        const bool stoppingEditable = m_ctrl->isPaused() && settled;
        if (m_stoppingBtn) m_stoppingBtn->setEnabled(stoppingEditable);
        if (!paused) {
            m_pauseSettledLogged = false;
        } else if (settled && !m_pauseSettledLogged) {
            onPauseSettled();
        }
    });

    // Solver state changes are driven by explicit controller signals instead of a
    // polling timer. progress() refreshes the paused/settled state at each
    // iteration boundary.
    connect(m_ctrl, &CfdController::pauseRequested, this, [this]() {
        if (m_pauseBtn) m_pauseBtn->setText("Pausing...");
        if (m_stepBtn) m_stepBtn->setEnabled(false);
    });
    connect(m_ctrl, &CfdController::resumed, this, [this]() {
        if (m_pauseBtn) m_pauseBtn->setText("Pause");
        if (m_stepBtn) m_stepBtn->setEnabled(false);
        log("Resumed.");
    });
    // progress() -> onProgress() (line 93) already refreshes pause/step button state.

    // ── Advanced solver settings (dialog) ────────────────────────────────────
    // Built up front and kept hidden. Its fields are read by collectSettings(),
    // so they must exist from the start; creating them as children of the page
    // and reparenting them only on first open would leave them painted over the
    // top-left corner of the panel (a parented widget with no layout floats).
    buildAdvancedDialog();
    connect(m_advancedBtn, &QPushButton::clicked, this, [this] { m_advancedDlg->exec(); });
    connect(m_stoppingBtn, &QPushButton::clicked, this, &CfdCasePage::onStoppingCriteria);

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
    m_advancedDlg->setWindowTitle("Advanced Solver Settings");

    auto* tabs = new QTabWidget(m_advancedDlg);
    auto* outer = new QVBoxLayout(m_advancedDlg);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(6);
    outer->addWidget(tabs);

    // No scroll area: every page is short enough to sit in a plain tab, so the
    // dialog can be sized to its content and never grow a scrollbar. The page
    // layout hugs the top because each page ends with a stretch; without it a
    // short page spreads its rows apart to fill the viewport, which is what
    // opened the large gaps between the pressure fields and their header.
    auto addPage = [&](const QString& title) {
        auto* page = new QWidget;
        auto* lay = new QVBoxLayout(page);
        lay->setContentsMargins(8, 8, 8, 8);
        lay->setSpacing(4);
        tabs->addTab(page, title);
        return lay;
    };
    // A titled group per related set of settings. Titled frames make the
    // structure visible without the floating header labels the old layout had.
    // Returns the box and its form: a page places the box itself, so the caller
    // decides whether the group is stacked or sits beside another one.
    auto makeGroup = [](const QString& title) {
        auto* box = new QGroupBox(title);
        auto* form = new QFormLayout(box);
        form->setContentsMargins(8, 6, 8, 8);
        form->setSpacing(4);
        form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
        form->setRowWrapPolicy(QFormLayout::DontWrapRows);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
        return std::make_pair(box, form);
    };
    // Stacks one group under another in a plain page column.
    auto addGroup = [makeGroup](QVBoxLayout* lay, const QString& title) {
        auto group = makeGroup(title);
        lay->addWidget(group.first);
        return group.second;
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
    auto* pressureLay = addPage("Pressure");
    auto* pressureForm = addGroup(pressureLay, "Pressure Correction");

    m_pressureCorrectors = styleSpin(new QSpinBox);
    m_pressureCorrectors->setRange(1, 100);
    m_pressureCorrectors->setToolTip("Pressure-corrector sweeps per SIMPLE iteration.");
    pressureForm->addRow("Correctors", m_pressureCorrectors);

    m_limitPressureCorrection = new QCheckBox("Limit P' Correction");
    m_limitPressureCorrection->setToolTip(
        "Clamp the pressure correction as a fraction of rho*U^2. Prevents the first\n"
        "iterations from overshooting, at the cost of a weaker pressure-velocity coupling.");
    pressureForm->addRow(m_limitPressureCorrection);

    m_pressureCorrectionLimit = styleSpin(new QDoubleSpinBox);
    m_pressureCorrectionLimit->setRange(0, 1000);
    m_pressureCorrectionLimit->setDecimals(2);
    m_pressureCorrectionLimit->setToolTip("Upper bound on |p'| as a multiple of rho*U_ref^2.");
    pressureForm->addRow("Correction Limit", m_pressureCorrectionLimit);

    m_pressureRefCell = styleSpin(new QSpinBox);
    m_pressureRefCell->setRange(0, 1000000);
    m_pressureRefCell->setSpecialValueText("auto");
    m_pressureRefCell->setToolTip(
        "Cell that pins the pressure datum when no pressure outlet exists. auto = last cell.");
    pressureForm->addRow("Reference Cell", m_pressureRefCell);

    m_pressureSolverStats = new QCheckBox("Pressure Solver Statistics");
    m_pressureSolverStats->setToolTip("Print per-run pressure linear-solver statistics.");
    pressureLay->addWidget(m_pressureSolverStats);

    pressureLay->addStretch(1);

    // ── Pressure ─────────────────────────────────────────────────────────
    // The tab name already scopes these, so no caption is needed.
    auto* pressureAmgLay = addPage("Pressure");
    m_pressureAmgWidget = new AmgConfigWidget(cfd::AmgSettings::pressureDefaults(), nullptr, true);
    pressureAmgLay->addWidget(m_pressureAmgWidget);
    pressureAmgLay->addStretch(1);

    // ── Momentum ─────────────────────────────────────────────────────────────
    auto* momentumLay = addPage("Momentum");

    m_momentumSolverStats = new QCheckBox("Momentum Solver Statistics");
    m_momentumSolverStats->setToolTip("Print per-run momentum linear-solver statistics.");
    momentumLay->addWidget(m_momentumSolverStats);

    auto* hierarchyCaption = dialogSectionHeader("Momentum AMG hierarchy");
    momentumLay->addWidget(hierarchyCaption);
    m_momentumAmgWidget = new AmgConfigWidget(cfd::AmgSettings::momentumDefaults());
    momentumLay->addWidget(m_momentumAmgWidget);
    momentumLay->addStretch(1);

    // ── Turbulence ─────────────────────────────────────────────────────────────
    auto* turbulenceLay = addPage("Turbulence");

    auto* turbHierarchyCaption = dialogSectionHeader("Turbulence AMG hierarchy");
    turbulenceLay->addWidget(turbHierarchyCaption);
    m_turbulenceAmgWidget = new AmgConfigWidget(cfd::AmgSettings::momentumDefaults());
    turbulenceLay->addWidget(m_turbulenceAmgWidget);
    turbulenceLay->addStretch(1);

    // ── Gradients ─────────────────────────────────────────────────────────────
    auto* gradientsLay = addPage("Gradients");
    auto* gradientsForm = addGroup(gradientsLay, "Slope Limiter");

    m_limiterKind = styleSpin(new QComboBox);
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
        lay->addWidget(new QLabel(caption));
        lay->addWidget(field);
        gradientsForm->addRow(row);
        return row;
    };

    m_limiterK = styleSpin(new QDoubleSpinBox);
    m_limiterK->setRange(0.0, 1e6);
    m_limiterK->setDecimals(4);
    m_limiterK->setSingleStep(0.01);
    m_limiterK->setValue(0.1);
    m_limiterK->setToolTip(
        "Venkatakrishnan smoothing: eps = k * (hi - lo), relative to the local "
        "field variation. Only used by the Venkatakrishnan limiter.");
    m_limiterKRow = addLimiterRow("Smoothing Constant (k)", m_limiterK);

    m_limiterCubicTransition = styleSpin(new QDoubleSpinBox);
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
    directionLay->setSpacing(4);
    m_limiterSpanGroup = new QButtonGroup(m_advancedDlg);
    m_limiterFaceCentre = new QRadioButton("Face Centre");
    m_limiterFaceCentre->setChecked(true);
    m_limiterFaceCentre->setToolTip("d = x_face - x_upwind (default).");
    m_limiterSpanGroup->addButton(m_limiterFaceCentre, 0);
    m_limiterCentroidLine = new QRadioButton("Centroid Line");
    m_limiterCentroidLine->setToolTip("d along the cell-centroid line, scaled to "
                                      "the face plane.");
    m_limiterSpanGroup->addButton(m_limiterCentroidLine, 1);
    directionLay->addWidget(m_limiterFaceCentre);
    directionLay->addWidget(m_limiterCentroidLine);
    gradientsForm->addRow("Limiting Span", directionLay);

    auto* aggLay = new QVBoxLayout;
    aggLay->setContentsMargins(0, 0, 0, 0);
    aggLay->setSpacing(4);
    m_limiterAggregationGroup = new QButtonGroup(m_advancedDlg);
    m_limiterCellWide = new QRadioButton("Cell-wide");
    m_limiterCellWide->setChecked(true);
    m_limiterCellWide->setToolTip("One scale factor per cell: the most restrictive "
                                  "face limits every face of the cell.");
    m_limiterAggregationGroup->addButton(m_limiterCellWide, 0);
    m_limiterComponentWise = new QRadioButton("Per Face");
    m_limiterComponentWise->setToolTip("One scale factor per face: only faces that "
                                       "leave the bound set are clipped.");
    m_limiterAggregationGroup->addButton(m_limiterComponentWise, 1);
    aggLay->addWidget(m_limiterCellWide);
    aggLay->addWidget(m_limiterComponentWise);
    gradientsForm->addRow("Limiting Scope", aggLay);

    gradientsLay->addStretch(1);

    // ── Diagnostics ──────────────────────────────────────────────────────────
    auto* diagLay = addPage("Diagnostics");
    auto* diagForm = addGroup(diagLay, "Residual Monitoring");

    m_printFrequency = styleSpin(new QSpinBox);
    m_printFrequency->setRange(1, 100000);
    m_printFrequency->setToolTip("Print a residual row every N outer iterations.");
    diagForm->addRow("Print Frequency", m_printFrequency);

    m_residualHistoryWindow = styleSpin(new QSpinBox);
    m_residualHistoryWindow->setRange(1, 10000);
    m_residualHistoryWindow->setToolTip(
        "Iterations inspected when watching for a continuity residual that keeps growing.");
    diagForm->addRow("Residual Window", m_residualHistoryWindow);

    m_velocityCorrectionMaxUnlimitedSpeed = styleSpin(new QDoubleSpinBox);
    m_velocityCorrectionMaxUnlimitedSpeed->setRange(0.0, 1e6);
    m_velocityCorrectionMaxUnlimitedSpeed->setDecimals(2);
    m_velocityCorrectionMaxUnlimitedSpeed->setValue(20.0);
    m_velocityCorrectionMaxUnlimitedSpeed->setSuffix(" m/s");
    m_velocityCorrectionMaxUnlimitedSpeed->setToolTip(
        "Velocity growth limiting is only applied when cell speed exceeds "
        "this threshold. Below it, no damping is applied.");
    diagForm->addRow("Max Unlimited Speed", m_velocityCorrectionMaxUnlimitedSpeed);

    m_velocityCorrectionMaxGrowthRate = styleSpin(new QDoubleSpinBox);
    m_velocityCorrectionMaxGrowthRate->setRange(0.0, 1.0);
    m_velocityCorrectionMaxGrowthRate->setDecimals(3);
    m_velocityCorrectionMaxGrowthRate->setSingleStep(0.01);
    m_velocityCorrectionMaxGrowthRate->setValue(0.15);
    m_velocityCorrectionMaxGrowthRate->setToolTip(
        "Maximum per-iteration velocity amplitude growth rate from pressure correction.");
    diagForm->addRow("Acceptable Speed Growth Rate", m_velocityCorrectionMaxGrowthRate);

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
    restoreBtn->setText("Restore Defaults");
    restoreBtn->setToolTip("Reset every setting on every tab of this dialog.");
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, m_advancedDlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, m_advancedDlg, &QDialog::reject);
    connect(restoreBtn, &QPushButton::clicked, this, [this] { applyDialogDefaults(); });

    // ── Dependencies ─────────────────────────────────────────────────────────
    connect(m_limitPressureCorrection, &QCheckBox::toggled, this,
            [this](bool) { syncDialogDependencies(); });
    connect(m_limiterKind, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { syncDialogDependencies(); });

    // Seed every control from the shipped defaults, so the values the dialog
    // opens with and the values "Restore defaults" produces cannot drift.
    applyDialogDefaults();

    // Sizing is by content only: the dialog used to be resized to 780x620, which
    // is what made a short tab look empty and a tall one overflow. No scroll area
    // is involved anywhere, so the tab widget simply reports the page's own hint.

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
    m_pressureAmgWidget->resetToDefaults();

    m_momentumSolverStats->setChecked(d.momentumSolverStats);
    m_pressureSolverStats->setChecked(d.pressureSolverStats);
    m_momentumAmgWidget->resetToDefaults();

    m_turbulenceAmgWidget->resetToDefaults();

    m_printFrequency->setValue(static_cast<int>(d.printFrequency));
    m_residualHistoryWindow->setValue(static_cast<int>(d.residualHistoryWindow));
    m_numThreads->setValue(d.numThreads);

    m_limiterKind->setCurrentIndex(d.limiterKind);
    m_limiterFaceCentre->setChecked(d.limiterSpan == 0);
    m_limiterCentroidLine->setChecked(d.limiterSpan == 1);
    m_limiterCellWide->setChecked(d.limiterAggregation == 0);
    m_limiterComponentWise->setChecked(d.limiterAggregation == 1);
    m_limiterK->setValue(d.limiterK);
    m_limiterCubicTransition->setValue(d.cubicTransition);

    // Checkbox signals only fire on an actual change, so sync explicitly.
    syncDialogDependencies();
}

void CfdCasePage::syncDialogDependencies() {
    m_pressureCorrectionLimit->setEnabled(m_limitPressureCorrection->isChecked());

    const bool turbulenceAmg = true; // turbulence AMG is always available
    m_turbulenceAmgWidget->setEnabled(turbulenceAmg);

    // "Correctors" is usable in both steady (SIMPLE) and transient (PISO) runs.
    m_pressureCorrectors->setEnabled(true);
    m_pressureCorrectors->setToolTip("Pressure-corrector sweeps per iteration.");

    // Each limiter reads exactly one of the two tuning fields, so the other one
    // is hidden rather than left as an inert box that looks adjustable.
    const int limiter = m_limiterKind->currentIndex();
    m_limiterKRow->setVisible(limiter == 1);
    m_limiterCubicRow->setVisible(limiter == 2);

    // The Venkatakrishnan smoothing factor is a share of the local field
    // variation, so the "Cubic" and "MinMod" cases have nothing to say about it.
    m_limiterK->setEnabled(limiter == 1);
    m_limiterCubicTransition->setEnabled(limiter == 2);
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
    if (transient) {
        m_alphaU->setToolTip("Forced to 1.0 by PISO in transient mode.");
        m_alphaP->setToolTip("Forced to 1.0 by PISO in transient mode.");
    } else {
        m_alphaU->setToolTip("");
        m_alphaP->setToolTip("");
    }

    // Keep the dialog's gating in sync too.
    syncDialogDependencies();
}

void CfdCasePage::setRunning(bool running) {
    m_running = running;
    if (m_runBtn) m_runBtn->setEnabled(!running);
    if (m_initBtn) m_initBtn->setEnabled(!running);
    if (m_pauseBtn) {
        m_pauseBtn->setEnabled(running);
        if (running) m_pauseBtn->setText("Pause");
    }
    if (m_stepBtn) m_stepBtn->setEnabled(false);
    if (m_cancelBtn) {
        m_cancelBtn->setText("Cancel");
        m_cancelBtn->setEnabled(running);
    }
    if (m_advancedBtn) m_advancedBtn->setEnabled(!running);
    if (m_bcEditBtn) m_bcEditBtn->setEnabled(!running && !m_patchBCs.empty());
    // Stopping criteria may be edited when idle, or when paused at a settled
    // boundary (live edit). Disabled while actively running or pausing.
    const bool stoppingEditable = !running ||
        (m_ctrl && m_ctrl->isPaused() && m_ctrl->isPausedSettled());
    if (m_stoppingBtn) m_stoppingBtn->setEnabled(stoppingEditable);
    if (!running) {
        m_pauseSettledLogged = false;
        if (m_pauseBtn) m_pauseBtn->setText("Pause");
        if (m_stateTimer) m_stateTimer->stop();
        m_lastFrame.reset();
    } else {
        if (m_stateTimer) m_stateTimer->start();
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
    if (m_ctrl->loadMesh(path)) {
        if (S()) S()->addRecentFile(path);
    }
}

void CfdCasePage::log(const QString& msg) {
    std::cout << msg.toStdString() << std::endl;
}

void CfdCasePage::applySettings(const cfd::CaseSettings& s) {
    m_rho->setText(QString::number(s.rho));
    m_nu->setText(QString::number(s.nu));
    m_alphaU->setValue(s.alphaU);
    if (m_alphaK) m_alphaK->setValue(s.alphaK);
    if (m_alphaW) m_alphaW->setValue(s.alphaOmega);
    if (m_wallFuncCb) m_wallFuncCb->setChecked(s.wallFunction);
    m_simplec->setChecked(s.simplec);
    m_scheme->setCurrentIndex(s.scheme);
    m_gradientScheme->setCurrentIndex(s.gradientScheme);
    // Rhie-Chow is always on (no toggle).
    m_turb->setChecked(s.turbulence);
    if (m_kInf) m_kInf->setValue(s.kInf);
    if (m_wInf) m_wInf->setValue(s.omegaInf);

    m_pressureCorrectors->setValue(s.pressureCorrectors);
    m_limitPressureCorrection->setChecked(s.limitPressureCorrection);
    m_pressureCorrectionLimit->setValue(s.pressureCorrectionLimit);
    m_pressureRefCell->setValue((int)s.pressureRefCell);
    m_printFrequency->setValue((int)s.printFrequency);
    m_residualHistoryWindow->setValue((int)s.residualHistoryWindow);
    m_velocityCorrectionMaxUnlimitedSpeed->setValue(s.velocityCorrectionMaxUnlimitedSpeed);
    m_velocityCorrectionMaxGrowthRate->setValue(s.velocityCorrectionMaxGrowthRate);
    m_numThreads->setValue(s.numThreads);

    m_limiterKind->setCurrentIndex(s.limiterKind);
    m_limiterFaceCentre->setChecked(s.limiterSpan == 0);
    m_limiterCentroidLine->setChecked(s.limiterSpan == 1);
    m_limiterCellWide->setChecked(s.limiterAggregation == 0);
    m_limiterComponentWise->setChecked(s.limiterAggregation == 1);
    m_limiterK->setValue(s.limiterK);
    m_limiterCubicTransition->setValue(s.cubicTransition);

    m_pressureAmgWidget->setConfig(s.pressureAmg);

    m_momentumSolverStats->setChecked(s.momentumSolverStats);
    m_pressureSolverStats->setChecked(s.pressureSolverStats);
    m_momentumAmgWidget->setConfig(s.momentumAmg);

    m_transientRadio->setChecked(s.transient.enabled);
    m_dtSpin->setValue(s.transient.dt);
    m_timeSchemeCombo->setCurrentIndex(s.transient.timeScheme == fvm::TimeScheme::BDF2 ? 1 : 0);
    m_adaptiveCflCb->setChecked(s.transient.adaptiveCfl);
    m_cflSpin->setValue(s.transient.cfl);
    m_minDtSpin->setValue(s.transient.minDt);
    m_maxDtSpin->setValue(s.transient.maxDt);
    m_writeIntervalSpin->setValue(s.transient.writeInterval);
    m_nonOrthCorrectorsSpin->setValue(s.transient.nonOrthogonalCorrectors);
    m_pvdFileEdit->setText(QString::fromStdString(s.transient.pvdFile));

    syncDialogDependencies();
    syncTimeDependencies();
}

void CfdCasePage::onMeshLoaded() {
    if (!S() || !m_ctrl) return;
    if (!m_ctrl->meshName().isEmpty()) m_meshName = m_ctrl->meshName();
    const auto& res = m_ctrl->meshResult();

    // Populate the page's patch BCs from the loaded mesh so the
    // "Edit Boundary Conditions" button becomes enabled.
    rebuildPatchTable();

    if (res.renderMesh) {
        // If a solved result mesh (with scalars) is already loaded, don't
        // overwrite it with the geometry-only base mesh. The base mesh has no
        // scalar attributes, which would empty the Scalar Field list and make
        // hasVolumeData() return false for volume/slice rendering.
        auto rm = m_ctrl->resultMesh();
        if (rm && !rm->scalars.empty()) return;
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
    cfd::CaseSettings s = m_ctrl->settings();
    bool okRho, okNu;
    s.rho = m_rho->text().toDouble(&okRho);
    s.nu = m_nu->text().toDouble(&okNu);
    if (!okRho || !okNu || s.rho <= 0.0 || s.nu <= 0.0) {
        log("Density and viscosity must be positive numbers.");
        return;
    }
    s.alphaU = m_alphaU->value();
    s.alphaP = m_alphaP->value();
    s.simplec = m_simplec->isChecked();
    s.scheme = m_scheme->currentIndex();
    s.gradientScheme = m_gradientScheme->currentIndex();
    s.rhieChow = true; // Rhie-Chow is always on for collocated grids.
    s.turbulence = m_turb->isChecked();
    // Only read turbulence sub-values when turbulence is enabled; otherwise
    // preserve the controller's defaults so stale widget values are not
    // leaked into the settings object.
    if (m_turb->isChecked()) {
        s.kInf = m_kInf ? m_kInf->value() : m_ctrl->settings().kInf;
        s.omegaInf = m_wInf ? m_wInf->value() : m_ctrl->settings().omegaInf;
        s.alphaK = m_alphaK ? m_alphaK->value() : m_ctrl->settings().alphaK;
        s.alphaOmega = m_alphaW ? m_alphaW->value() : m_ctrl->settings().alphaOmega;
        s.wallFunction = m_wallFuncCb ? m_wallFuncCb->isChecked() : m_ctrl->settings().wallFunction;
    } else {
        s.kInf = m_ctrl->settings().kInf;
        s.omegaInf = m_ctrl->settings().omegaInf;
        s.alphaK = m_ctrl->settings().alphaK;
        s.alphaOmega = m_ctrl->settings().alphaOmega;
        s.wallFunction = m_ctrl->settings().wallFunction;
    }
    // Advanced
    s.pressureCorrectors = m_pressureCorrectors->value();
    s.limitPressureCorrection = m_limitPressureCorrection->isChecked();
    s.pressureCorrectionLimit = m_pressureCorrectionLimit->value();
    s.pressureRefCell = static_cast<std::size_t>(m_pressureRefCell->value());
    s.printFrequency = static_cast<std::size_t>(m_printFrequency->value());
    s.residualHistoryWindow = static_cast<std::size_t>(m_residualHistoryWindow->value());
    s.velocityCorrectionMaxUnlimitedSpeed = m_velocityCorrectionMaxUnlimitedSpeed->value();
    s.velocityCorrectionMaxGrowthRate = m_velocityCorrectionMaxGrowthRate->value();
    s.numThreads = m_numThreads->value();

    s.limiterKind = m_limiterKind->currentIndex();
    s.limiterAggregation = m_limiterAggregationGroup->checkedId();
    if (s.limiterAggregation < 0) s.limiterAggregation = 0;
    s.limiterSpan = m_limiterSpanGroup->checkedId();
    if (s.limiterSpan < 0) s.limiterSpan = 0;
    s.limiterK = m_limiterK->value();
    s.cubicTransition = m_limiterCubicTransition->value();

    // Pressure linear solver
    s.pressureAmg = m_pressureAmgWidget->config();

    // Momentum linear solver
    s.momentumSolverStats = m_momentumSolverStats->isChecked();
    s.pressureSolverStats = m_pressureSolverStats->isChecked();
    s.momentumAmg = m_momentumAmgWidget->config();

    // Turbulence AMG
    s.turbulenceAmg = m_turbulenceAmgWidget->config();

    // Time mode (steady / transient)
    s.transient.enabled = m_transientRadio->isChecked();
    s.transient.dt = m_dtSpin->value();
    s.transient.timeScheme = (m_timeSchemeCombo->currentIndex() == 1)
        ? fvm::TimeScheme::BDF2
        : fvm::TimeScheme::Euler;
    s.transient.adaptiveCfl = m_adaptiveCflCb->isChecked();
    s.transient.cfl = m_cflSpin->value();
    s.transient.minDt = m_minDtSpin->value();
    s.transient.maxDt = m_maxDtSpin->value();
    s.transient.writeInterval = m_writeIntervalSpin->value();
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
    emit residualsCleared();
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
    m_lastFrame.reset();
    log("Cancelling... (the current step finishes first)");
}

void CfdCasePage::onPause() {
    if (!m_running) return;
    if (m_ctrl->isPausedSettled()) {
        m_ctrl->resume();
        m_pauseSettledLogged = false;
        log("Resumed.");
    } else if (!m_ctrl->isPaused()) {
        m_ctrl->pause();
        log("Pause requested; stopping at the next step boundary.");
    }
    // In-flight pause: ignore the click.
}

void CfdCasePage::onStep() {
    if (!m_running) return;
    if (!m_ctrl->isPausedSettled()) {
        log("Pause the run before stepping.");
        return;
    }
    m_ctrl->step();
    m_pauseSettledLogged = false;
    log(m_ctrl->settings().transient.enabled ? "Stepping one time step." : "Stepping one iteration.");
}

void CfdCasePage::onStoppingCriteria() {
    if (!m_stoppingDlg) {
        m_stoppingDlg = new cfd::StoppingCriteriaDialog(this);
        m_stoppingDlg->setWindowModality(Qt::ApplicationModal);
    }
    const bool dim3 = m_ctrl->meshResult().dim == 3;
    const bool turbulence = m_ctrl->settings().turbulence;
    const bool transient = m_ctrl->settings().transient.enabled;
    if (!m_stoppingDlg->edit(m_ctrl->settings().stopping, dim3, turbulence,
                             transient)) {
        return; // Cancel: nothing changes.
    }
    auto result = m_stoppingDlg->result();
    if (!result) return;

    // Apply live to a paused, settled run; otherwise store for the next run.
    const bool applyLive = m_running && m_ctrl->isPaused() &&
                           m_ctrl->isPausedSettled();
    if (!m_ctrl->setStoppingCriteria(*result, applyLive)) {
        // setStoppingCriteria already emitted failed() with the reason.
        return;
    }
    log(QString("Stopping criteria %1.")
            .arg(applyLive ? "applied to the active run"
                           : "stored for the next run"));
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
    // (residuals are cleared only after the user confirms initialization)

    QDialog dlg(this);
    dlg.setWindowTitle("Initialize Solution");
    auto* lay = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout;
    auto* ux = new QDoubleSpinBox(&dlg); ux->setRange(-1e6, 1e6); ux->setDecimals(4); ux->setValue(0.0);
    auto* uy = new QDoubleSpinBox(&dlg); uy->setRange(-1e6, 1e6); uy->setDecimals(4); uy->setValue(0.0);
    QDoubleSpinBox* uz = nullptr;
    if (dim >= 3) { uz = new QDoubleSpinBox(&dlg); uz->setRange(-1e6, 1e6); uz->setDecimals(4); uz->setValue(0.0); }
    auto* p  = new QDoubleSpinBox(&dlg); p->setRange(-1e6, 1e6); p->setDecimals(4); p->setValue(0.0);
    QDoubleSpinBox* k = nullptr;
    QDoubleSpinBox* w = nullptr;
    const bool turb = m_ctrl->settings().turbulence;
    if (turb) {
            k = new QDoubleSpinBox(&dlg); k->setRange(0.0, 1e3); k->setDecimals(4); k->setValue(0.0);
            k->setSpecialValueText("auto");
            w = new QDoubleSpinBox(&dlg); w->setRange(0.0, 1e6); w->setDecimals(4); w->setValue(0.0);
            w->setSpecialValueText("auto");
    }
    form->addRow("U_x", ux);
    form->addRow("U_y", uy);
    if (dim >= 3) form->addRow("U_z", uz);
    form->addRow("p", p);
    if (turb) {
        form->addRow("k", k);
        form->addRow("omega", w);
    }
    lay->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    emit residualsCleared();

    cfd::InitConditions ic;
    ic.ux = ux->value();
    ic.uy = uy->value();
    ic.uz = (dim >= 3 && uz) ? uz->value() : 0.0;
    ic.pressure = p->value();
    ic.k = (turb && k) ? k->value() : m_ctrl->settings().kInf;
    ic.omega = (turb && w) ? w->value() : m_ctrl->settings().omegaInf;

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
    if (m_ctrl->saveCase(path)) {
        log("Saved " + QFileInfo(path).fileName());
        if (S()) S()->addRecentFile(path);
    }
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
        if (S()) S()->addRecentFile(path);
    } else {
        log("Load failed: " + m_ctrl->lastError());
    }
}

void CfdCasePage::loadMeshFromPath(const QString& path) {
    if (!m_ctrl || path.isEmpty()) return;
    if (m_ctrl->loadMesh(path)) {
        if (S()) S()->addRecentFile(path);
    }
}

void CfdCasePage::loadCaseFromPath(const QString& path) {
    if (!m_ctrl || path.isEmpty()) return;
    if (m_ctrl->loadCase(path)) {
        // onCaseLoaded() is invoked via the caseLoaded() signal — do NOT call it directly.
        if (S()) S()->addRecentFile(path);
    }
}
void CfdCasePage::onCaseLoaded() {
    if (!S() || !m_ctrl) return;
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
    emit monitorHistoryRestored();
    setRunning(false);
}

void CfdCasePage::onProgress(int iteration, double continuity, double momentum) {
    m_iterValue->setText(QString::number(iteration));
    m_contValue->setText(QString::number(continuity, 'e', 3));
    m_momValue->setText(QString::number(momentum, 'e', 3));
    emit residualPoint(iteration, continuity, momentum);
    // Also refresh the pause/step button state on each iteration boundary so the
    // UI stays in sync with the solver's paused/settled state.
    if (!m_running) return;
    const bool paused = m_ctrl->isPaused();
    const bool settled = m_ctrl->isPausedSettled();
    if (m_pauseBtn)
        m_pauseBtn->setText(paused ? (settled ? "Resume" : "Pausing...") : "Pause");
    if (m_stepBtn) m_stepBtn->setEnabled(settled);
}

void CfdCasePage::onTransientFrame(std::shared_ptr<const RenderMesh> mesh, int step, double time) {
    if (!S() || !m_ctrl || m_ctrl->isCancelled()) return;
    if (!mesh) return;
    // Preserve the user's scalar-color toggle: onAnimationFrame's first-frame
    // block disables it by default (animation UX), which would blank the
    // colormap mid-run.
    const bool hadScalarColor = S()->getMeshUseScalarColor();
    // Light per-frame publish: keeps the camera and the user's field selection.
    S()->onAnimationFrame(mesh, step, time);
    S()->setMeshUseScalarColor(hadScalarColor);
    m_iterValue->setText(QString::number(step));
}

void CfdCasePage::onPauseSettled() {
    if (!S() || !m_running || !m_ctrl) return;
    if (!m_ctrl->isPausedSettled()) return;
    if (m_pauseSettledLogged) return;
    m_pauseSettledLogged = true;

    const int step = m_ctrl->currentStep();
    const double t = m_ctrl->currentTime();

    if (m_ctrl->settings().transient.enabled) {
        m_iterValue->setText(QString::number(step));
        log(QString("Paused at step %1, t = %2 s").arg(step).arg(t, 0, 'g', 8));
        if (auto slot = m_ctrl->latestFrameSlot()) {
            if (slot->mesh && slot->mesh != m_lastFrame) {
                m_lastFrame = slot->mesh;
                S()->setComputedSnapshot(slot->computed);
                const bool hadScalarColor = S()->getMeshUseScalarColor();
                S()->onAnimationFrame(slot->mesh, slot->step, slot->time);
                S()->setMeshUseScalarColor(hadScalarColor);
            }
        } else {
            auto rm = m_ctrl->resultMesh();
            if (rm) S()->applyLoadedMesh(rm, "cfd_solution", false);
        }
    } else {
        m_iterValue->setText(QString::number(step));
        log(QString("Paused at iteration %1").arg(m_iterValue->text()));
        auto rm = m_ctrl->resultMesh();
        if (rm) S()->applyLoadedMesh(rm, "cfd_solution", false);
    }
}

void CfdCasePage::onFinished(const QString& summary) {
    if (!S() || !m_ctrl) return;
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
    if (!S() || !m_ctrl) return;
    if (m_frameTimer) m_frameTimer->stop();
    m_lastFrame.reset();
    setRunning(false);
    S()->setAnimScaleGlobal(m_savedAnimGlobal);
    log("Failed: " + error);
}

} // namespace sidebar
