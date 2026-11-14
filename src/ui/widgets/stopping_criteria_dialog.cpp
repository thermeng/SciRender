#include "stopping_criteria_dialog.h"

#include "ui/uikit.h"

#include <QFormLayout>
#include <QGridLayout>
#include <QLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>

namespace {

// Validator that accepts a positive decimal or scientific value but leaves
// intermediate input (a leading "-", a lone "e", etc.) as Intermediate so the
// user can finish typing. Final validation is owned by the model.
QRegularExpressionValidator* positiveNumberValidator() {
    static const QRegularExpression re(
        R"(([+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?)?)");
    return new QRegularExpressionValidator(re);
}

} // namespace

namespace cfd {

StoppingCriteriaDialog::StoppingCriteriaDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("Stopping Criteria");

    model_ = std::make_unique<StoppingCriteriaModel>(this);

    auto* main = new QVBoxLayout(this);

    // ── Equation table ──────────────────────────────────────────────
    eqGroup_ = new QGroupBox("Residual Equations");
    auto* eqLay = new QVBoxLayout(eqGroup_);
    eqLay->setContentsMargins(uikit::kGroupMargin, uikit::kGroupMargin,
                              uikit::kGroupMargin, uikit::kGroupMargin);
    eqLay->setSpacing(uikit::kGroupSpacing);
    eqGrid_ = new QGridLayout();
    eqGrid_->setColumnStretch(0, 1);
    eqGrid_->setHorizontalSpacing(12);
    eqGrid_->setVerticalSpacing(6);
    auto* hName = new QLabel("Equation");
    hName->setAlignment(Qt::AlignCenter | Qt::AlignVCenter);
    auto* hEnable = new QLabel("Gate");
    hEnable->setAlignment(Qt::AlignCenter);
    auto* hOverride = new QLabel("Override");
    hOverride->setAlignment(Qt::AlignCenter);
    auto* hTol = new QLabel("Tolerance");
    hTol->setAlignment(Qt::AlignCenter);
    eqGrid_->addWidget(hName, 0, 0);
    eqGrid_->addWidget(hEnable, 0, 1, Qt::AlignCenter);
    eqGrid_->addWidget(hOverride, 0, 2, Qt::AlignCenter);
    eqGrid_->addWidget(hTol, 0, 3, Qt::AlignCenter);
    eqLay->addLayout(eqGrid_);
    eqLay->addStretch();
    main->addWidget(eqGroup_);

    // ── Steady policy ───────────────────────────────────────────────
    QFormLayout* sLay = nullptr;
    steadyGroup_ = uikit::group("Steady Stopping Policy", &sLay);
    sLay->setRowWrapPolicy(QFormLayout::WrapLongRows);
    sLay->setFormAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    defaultTolSpin_ = uikit::numeric(new QDoubleSpinBox);
    defaultTolSpin_->setDecimals(6);
    defaultTolSpin_->setRange(cfd::StoppingCriteriaLimits::kTolMin,
                              cfd::StoppingCriteriaLimits::kTolMax);
    defaultTolSpin_->setValue(cfd::defaultStoppingCriteria().defaultTolerance);
    defaultTolSpin_->setToolTip(
        "Default residual tolerance applied to every equation that inherits.");
    sLay->addRow("Default Tolerance", defaultTolSpin_);

    maxIterSpin_ = uikit::numeric(new QSpinBox);
    maxIterSpin_->setRange(
        (int)cfd::StoppingCriteriaLimits::kIterMin,
        (int)cfd::StoppingCriteriaLimits::kIterMax);
    maxIterSpin_->setValue(cfd::defaultStoppingCriteria().steady.maxIterations);
    maxIterSpin_->setToolTip(
        "Maximum iterations for residual mode. If the residual gate never "
        "fires the run stops here and reports \"max iterations reached\".");
    sLay->addRow("Max Iterations", maxIterSpin_);

    fixedIterCb_ = new QCheckBox("Fixed Iteration Limit");
    fixedIterCb_->setToolTip(
        "When on, the solver runs exactly this many iterations and the "
        "residual tolerance gate is bypassed. Max iterations is ignored.");
    sLay->addRow(fixedIterCb_);

    fixedIterSpin_ = uikit::numeric(new QSpinBox);
    fixedIterSpin_->setRange(
        (int)cfd::StoppingCriteriaLimits::kIterMin,
        (int)cfd::StoppingCriteriaLimits::kIterMax);
    fixedIterSpin_->setValue(cfd::defaultStoppingCriteria().steady.maxIterations);
    fixedIterSpin_->setEnabled(false);
    fixedIterSpin_->setToolTip("Exact iteration budget (fixed mode only).");
    sLay->addRow("Fixed Iterations", fixedIterSpin_);

    holdSpin_ = uikit::numeric(new QSpinBox);
    holdSpin_->setRange(cfd::StoppingCriteriaLimits::kHoldMin,
                        cfd::StoppingCriteriaLimits::kHoldMax);
    holdSpin_->setValue(cfd::defaultStoppingCriteria().steady.convergenceHoldIterations);
    holdSpin_->setToolTip(
        "Consecutive iterations every gated equation must stay below its "
        "tolerance before the run is declared converged.");
    sLay->addRow("Convergence Hold", holdSpin_);

    main->addWidget(steadyGroup_);

    // ── Transient policy ────────────────────────────────────────────
    QFormLayout* tLay = nullptr;
    transientGroup_ = uikit::group("Transient Stopping Policy", &tLay);
    tLay->setRowWrapPolicy(QFormLayout::WrapLongRows);
    tLay->setFormAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    tEndSpin_ = uikit::numeric(new QDoubleSpinBox);
    tEndSpin_->setDecimals(6);
    tEndSpin_->setRange(cfd::StoppingCriteriaLimits::kTEndMin,
                        cfd::StoppingCriteriaLimits::kTEndMax);
    tEndSpin_->setValue(cfd::defaultStoppingCriteria().transient.tEnd);
    tEndSpin_->setToolTip("Global end time; the run stops here regardless of "
                          "residuals.");
    tEndSpin_->setSuffix(" s");
    tLay->addRow("End Time", tEndSpin_);

    maxOuterSpin_ = uikit::numeric(new QSpinBox);
    maxOuterSpin_->setRange(cfd::StoppingCriteriaLimits::kOuterMin,
                            cfd::StoppingCriteriaLimits::kOuterMax);
    maxOuterSpin_->setValue(cfd::defaultStoppingCriteria().transient
                                .maxOuterCorrectors);
    maxOuterSpin_->setToolTip(
        "Maximum pressure-corrector sweeps per time step.");
    tLay->addRow("Max Outer Correctors", maxOuterSpin_);

    minOuterSpin_ = uikit::numeric(new QSpinBox);
    minOuterSpin_->setRange(cfd::StoppingCriteriaLimits::kOuterMin,
                            cfd::StoppingCriteriaLimits::kOuterMax);
    minOuterSpin_->setValue(cfd::defaultStoppingCriteria().transient
                                .minOuterCorrectors);
    minOuterSpin_->setToolTip(
        "Minimum pressure-corrector sweeps per time step, even when the "
        "residual target is already met.");
    tLay->addRow("Minimum Correctors", minOuterSpin_);

    reductionSpin_ = uikit::numeric(new QDoubleSpinBox);
    reductionSpin_->setRange(cfd::StoppingCriteriaLimits::kReductionMin,
                             cfd::StoppingCriteriaLimits::kReductionMax);
    reductionSpin_->setDecimals(2);
    reductionSpin_->setSingleStep(0.05);
    reductionSpin_->setValue(cfd::defaultStoppingCriteria().transient
                                 .residualReduction);
    reductionSpin_->setToolTip(
        "Stop the corrector loop once the continuity residual is this fraction "
        "of its value at the start of the time step. 0 disables the test.");
    tLay->addRow("Residual Reduction", reductionSpin_);

    transientGroup_->setVisible(false);
    main->addWidget(transientGroup_);

    // ── Buttons ─────────────────────────────────────────────────────
    buttons_ = uikit::buttons(this, true, [this]() {
        // Sync every tolerance field before accepting. editingFinished only
        // fires on focus loss / Enter, so a user who types a new tolerance and
        // clicks OK directly would otherwise commit a stale value.
        for (auto it = rowWidgets_.cbegin(); it != rowWidgets_.cend(); ++it) {
            auto& rw = it.value();
            if (rw.override_->isChecked() && rw.tolerance->isEnabled()) {
                model_->setEquationOverride(
                    it.key(), cfd::parseTolerance(
                        rw.tolerance->text().toStdString()));
            }
        }
        return true;
    });
    main->addWidget(buttons_);

    okBtn_ = buttons_->button(QDialogButtonBox::Ok);

    // Wiring.
    connect(model_.get(), &StoppingCriteriaModel::validationChanged, this,
            &StoppingCriteriaDialog::updateOkState);
    connect(model_.get(), &StoppingCriteriaModel::changed, this,
            &StoppingCriteriaDialog::updateOkState);

    connect(buttons_->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked,
            this, [this]() {
                model_->resetToDefaults();
                rebuildEquations();
                rebuildPolicy();
                updateOkState();
            });

    connect(defaultTolSpin_, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                model_->setDefaultTolerance(v);
                refreshToleranceTexts();
            });
    connect(maxIterSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int v) { model_->setSteadyMaxIterations((std::size_t)v); });
    connect(fixedIterCb_, &QCheckBox::toggled, this,
            [this](bool on) {
                fixedIterSpin_->setEnabled(on);
                model_->setSteadyFixedIterations(
                    on ? std::optional<std::size_t>(
                             (std::size_t)fixedIterSpin_->value())
                       : std::nullopt);
            });
    connect(fixedIterSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int v) {
                if (fixedIterCb_->isChecked())
                    model_->setSteadyFixedIterations((std::size_t)v);
            });
    connect(holdSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int v) { model_->setSteadyHold(v); });

    connect(tEndSpin_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double v) { model_->setTransientTEnd(v); });
    connect(maxOuterSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int v) { model_->setTransientMaxOuterCorrectors(v); });
    connect(minOuterSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int v) { model_->setTransientMinOuterCorrectors(v); });
    connect(reductionSpin_, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                model_->setTransientResidualReduction(v);
            });

    updateOkState();
    uikit::prepareDialog(this, 520);
}

StoppingCriteriaDialog::~StoppingCriteriaDialog() = default;

bool StoppingCriteriaDialog::edit(const cfd::StoppingCriteria& s, bool dim3,
                                  bool turbulence, bool transient) {
    dim3_ = dim3;
    turbulence_ = turbulence;
    transient_ = transient;
    model_->setContext(dim3, turbulence, transient);
    model_->load(s);
    rebuildEquations();
    rebuildPolicy();
    updateOkState();
    result_ = std::nullopt;
    const bool accepted = exec() == QDialog::Accepted;
    if (accepted) result_ = model_->commit();
    return accepted;
}

void StoppingCriteriaDialog::rebuildEquations() {
    // Remove all non-header items from the grid (Qt 6: removeRow removed).
    while (eqGrid_->count() > 4) {
        QLayoutItem* item = eqGrid_->takeAt(4);
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }
    rowWidgets_.clear();

    int row = 1;
    for (const auto& er : model_->equations()) {
        if (!er.applicable) continue;

        auto* nameLbl = new QLabel(er.name);
        nameLbl->setToolTip(er.tip);

        auto* enableCb = new QCheckBox;
        enableCb->setChecked(er.enabled);
        enableCb->setToolTip(er.tip);

        auto* overrideCb = new QCheckBox;
        overrideCb->setChecked(er.hasOverride);
        overrideCb->setToolTip("When off, the equation inherits the default "
                               "tolerance above.");

        auto* tolEdit = new QLineEdit;
        tolEdit->setValidator(positiveNumberValidator());
        tolEdit->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        tolEdit->setFixedWidth(110);
        tolEdit->setText(cfd::formatTolerance(er.tolerance).c_str());
        tolEdit->setEnabled(er.hasOverride && er.enabled);
        tolEdit->setToolTip(er.tip);

        RowWidgets rw{enableCb, overrideCb, tolEdit};
        rowWidgets_[er.id] = rw;

        connect(enableCb, &QCheckBox::toggled, this,
                [this, id = er.id, tolEdit, overrideCb](bool on) {
                    model_->setEquationEnabled(id, on);
                    tolEdit->setEnabled(on && overrideCb->isChecked());
                    updateOkState();
                });
        connect(overrideCb, &QCheckBox::toggled, this,
                [this, id = er.id, tolEdit, enableCb](bool on) {
                    if (on) {
                        model_->setEquationOverride(
                            id, cfd::parseTolerance(
                                tolEdit->text().toStdString()));
                    } else {
                        model_->setEquationOverride(id, std::nullopt);
                    }
                    tolEdit->setEnabled(on && enableCb->isChecked());
                    updateOkState();
                });
        connect(tolEdit, &QLineEdit::editingFinished, this,
                [this, id = er.id, tolEdit]() {
                    model_->setEquationOverride(
                        id, cfd::parseTolerance(
                            tolEdit->text().toStdString()));
                    updateOkState();
                });

    eqGrid_->addWidget(nameLbl, row, 0);
    eqGrid_->addWidget(enableCb, row, 1, Qt::AlignCenter);
    eqGrid_->addWidget(overrideCb, row, 2, Qt::AlignCenter);
    eqGrid_->addWidget(tolEdit, row, 3, Qt::AlignRight);
    ++row;
    }
}

void StoppingCriteriaDialog::refreshToleranceTexts() {
    const double def = model_->draft().defaultTolerance;
    for (auto it = rowWidgets_.cbegin(); it != rowWidgets_.cend(); ++it) {
        auto& rw = it.value();
        if (!rw.override_->isChecked()) {
            rw.tolerance->setText(cfd::formatTolerance(def).c_str());
        }
    }
}

void StoppingCriteriaDialog::rebuildPolicy() {
    const bool trans = model_->transient();
    steadyGroup_->setVisible(!trans);
    transientGroup_->setVisible(trans);

    defaultTolSpin_->setValue(model_->draft().defaultTolerance);
    maxIterSpin_->setValue((int)model_->draft().steady.maxIterations);
    const bool fixed = model_->draft().steady.fixedIterations.has_value();
    fixedIterCb_->setChecked(fixed);
    fixedIterSpin_->setEnabled(fixed);
    fixedIterSpin_->setValue(
        fixed ? (int)*model_->draft().steady.fixedIterations
              : (int)model_->draft().steady.maxIterations);
    holdSpin_->setValue(model_->draft().steady.convergenceHoldIterations);

    tEndSpin_->setValue(model_->draft().transient.tEnd);
    maxOuterSpin_->setValue(model_->draft().transient.maxOuterCorrectors);
    minOuterSpin_->setValue(model_->draft().transient.minOuterCorrectors);
    reductionSpin_->setValue(model_->draft().transient.residualReduction);
}

void StoppingCriteriaDialog::updateOkState() {
    okBtn_->setEnabled(model_->isValid());
}

} // namespace cfd
