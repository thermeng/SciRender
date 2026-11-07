#include "stopping_criteria_dialog.h"

#include <QDoubleValidator>
#include <QIntValidator>
#include <cmath>
#include <limits>

namespace {

// Validator that accepts strictly-positive doubles in decimal or scientific
// notation (e.g. 1e-3, 0.001, 2.5E-6).
class PositiveDoubleValidator : public QValidator {
public:
    explicit PositiveDoubleValidator(QObject* parent = nullptr) : QValidator(parent) {}
    State validate(QString& input, int& /*pos*/) const override {
        if (input.isEmpty()) return Intermediate;
        bool ok = false;
        double v = input.toDouble(&ok);
        if (!ok || !std::isfinite(v) || v <= 0.0) return Invalid;
        return Acceptable;
    }
};

// Validator for strictly-positive integers.
class PositiveIntValidator : public QValidator {
public:
    explicit PositiveIntValidator(QObject* parent = nullptr) : QValidator(parent) {}
    State validate(QString& input, int& /*pos*/) const override {
        if (input.isEmpty()) return Intermediate;
        bool ok = false;
        long v = input.toLong(&ok);
        if (!ok || v <= 0) return Invalid;
        return Acceptable;
    }
};

} // namespace

StoppingCriteriaDialog::StoppingCriteriaDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("Stopping Criteria");
    setModal(true);
    resize(420, 360);

    auto* main = new QVBoxLayout(this);

    // Intro text.
    auto* intro = new QLabel(
        "The solver stops when every enabled equation's scaled residual falls "
        "below its tolerance. Uncheck an equation to exclude it from the "
        "convergence test. If no equation is enabled, the run iterates until "
        "the fixed limit (or until cancelled).");
    intro->setWordWrap(true);
    intro->setStyleSheet("color: palette(text);");
    main->addWidget(intro);

    // Equation grid.
    m_grid = new QGridLayout();
    m_grid->setSpacing(8);
    m_grid->setColumnStretch(2, 1);

    // Header row.
    auto* hdrEnabled = new QLabel("Enable");
    hdrEnabled->setAlignment(Qt::AlignCenter);
    auto* hdrTol = new QLabel("Tolerance");
    hdrTol->setAlignment(Qt::AlignCenter);
    m_grid->addWidget(hdrEnabled, 0, 0);
    m_grid->addWidget(hdrTol, 0, 1);

    const struct {
        const char* name;
        const char* tip;
    } equations[6] = {
        {"Continuity",      "Mass imbalance residual. Uncheck to exclude continuity from the convergence test."},
        {"Momentum U",      "X-momentum linear residual."},
        {"Momentum V",      "Y-momentum linear residual."},
        {"Momentum W",      "Z-momentum linear residual (3D only)."},
        {"Turbulence k",    "Turbulent kinetic energy residual."},
        {"Turbulence omega","Specific dissipation rate residual."},
    };

    for (int i = 0; i < 6; ++i) {
        buildRow(i, equations[i].name, equations[i].tip);
    }
    main->addLayout(m_grid);

    // Fixed iteration limit row.
    auto* iterRow = new QHBoxLayout();
    m_fixedIterCb = new QCheckBox("Fixed iteration limit");
    m_fixedIterCb->setToolTip("When checked, the solver runs exactly this many "
                              "iterations and the residual tolerance gate is bypassed.");
    m_fixedIterEdit = new QLineEdit("500");
    m_fixedIterEdit->setValidator(new PositiveIntValidator(m_fixedIterEdit));
    m_fixedIterEdit->setFixedWidth(80);
    m_fixedIterEdit->setEnabled(false);
    iterRow->addWidget(m_fixedIterCb);
    iterRow->addWidget(new QLabel("iterations:"));
    iterRow->addWidget(m_fixedIterEdit);
    iterRow->addStretch(1);
    main->addLayout(iterRow);

    connect(m_fixedIterCb, &QCheckBox::toggled,
            m_fixedIterEdit, &QLineEdit::setEnabled);
    connect(m_fixedIterCb, &QCheckBox::toggled, this, &StoppingCriteriaDialog::updateOkState);
    connect(m_fixedIterEdit, &QLineEdit::textChanged, this, &StoppingCriteriaDialog::updateOkState);

    // Convergence hold iterations row.
    auto* holdRow = new QHBoxLayout();
    auto* holdLabel = new QLabel("Convergence hold");
    holdLabel->setToolTip("Number of consecutive iterations every gated "
                          "equation must stay below its tolerance before the "
                          "solver declares convergence.");
    m_holdItersSpin = new QSpinBox;
    m_holdItersSpin->setRange(1, 100);
    m_holdItersSpin->setValue(3);
    m_holdItersSpin->setFixedWidth(60);
    holdRow->addWidget(holdLabel);
    holdRow->addWidget(m_holdItersSpin);
    holdRow->addWidget(new QLabel("consecutive iterations below tolerance"));
    holdRow->addStretch(1);
    main->addLayout(holdRow);

    // Buttons.
    m_buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Reset);
    m_buttons->button(QDialogButtonBox::Reset)->setText("Reset to Defaults");
    main->addWidget(m_buttons);

    connect(m_buttons->button(QDialogButtonBox::Ok), &QPushButton::clicked,
            this, &QDialog::accept);
    connect(m_buttons->button(QDialogButtonBox::Cancel), &QPushButton::clicked,
            this, &QDialog::reject);
    connect(m_buttons->button(QDialogButtonBox::Reset), &QPushButton::clicked,
            this, [this]() {
                for (auto& row : m_rows) {
                    row.enabled->setChecked(true);
                    row.tolEdit->setText(fmtTol(1e-4));
                    row.tolEdit->setEnabled(true);
                }
                m_fixedIterCb->setChecked(false);
                m_fixedIterEdit->setText("500");
                m_fixedIterEdit->setEnabled(false);
                m_holdItersSpin->setValue(3);
                updateOkState();
            });

    updateOkState();
}

void StoppingCriteriaDialog::buildRow(int idx, const QString& name,
                                       const QString& tip) {
    auto& row = m_rows[idx];
    int r = idx + 1; // skip header row

    row.enabled = new QCheckBox();
    row.enabled->setChecked(true);
    row.enabled->setToolTip(tip);

    row.tolEdit = new QLineEdit(fmtTol(1e-4)); 
    row.tolEdit->setValidator(new PositiveDoubleValidator(row.tolEdit));
    row.tolEdit->setFixedWidth(100);
    row.tolEdit->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    row.nameLabel = new QLabel(name);
    row.nameLabel->setToolTip(tip);

    m_grid->addWidget(row.enabled, r, 0, Qt::AlignCenter);
    m_grid->addWidget(row.tolEdit, r, 1);
    m_grid->addWidget(row.nameLabel, r, 2);

    connect(row.enabled, &QCheckBox::toggled,
            row.tolEdit, &QLineEdit::setEnabled);
    connect(row.enabled, &QCheckBox::toggled, this, &StoppingCriteriaDialog::updateOkState);
    connect(row.tolEdit, &QLineEdit::textChanged, this, &StoppingCriteriaDialog::updateOkState);
}

QString StoppingCriteriaDialog::fmtTol(double v) {
    // Scientific notation with 3 significant digits: 1.00e-03
    return QString::number(v, 'e', 3);
}

double StoppingCriteriaDialog::parseTol(const QString& text, bool* ok) {
    double v = text.toDouble(ok);
    if (ok && *ok && (!std::isfinite(v) || v <= 0.0)) *ok = false;
    return v;
}

void StoppingCriteriaDialog::updateOkState() {
    bool anyValid = false;
    bool allValid = true;
    for (auto& row : m_rows) {
        if (!row.enabled->isChecked()) continue;
        bool ok = false;
        parseTol(row.tolEdit->text(), &ok);
        row.tolEdit->setStyleSheet(ok ? "" : "border: 1px solid red;");
        if (ok) anyValid = true;
        else allValid = false;
    }
    if (m_fixedIterCb->isChecked()) {
        bool ok = false;
        m_fixedIterEdit->text().toLong(&ok);
        m_fixedIterEdit->setStyleSheet(ok ? "" : "border: 1px solid red;");
        if (ok) anyValid = true;
        else allValid = false;
    }
    auto* okBtn = m_buttons->button(QDialogButtonBox::Ok);
    okBtn->setEnabled((anyValid && allValid) || isConfigured());
}

void StoppingCriteriaDialog::applyTo(cfd::CaseSettings& s) const {
    const char* names[6] = {
        "contTol", "momUTol", "momVTol", "momWTol",
        "turbKTol", "turbOmegaTol"
    };
    std::optional<double>* tols[6] = {
        &s.contTol, &s.momUTol, &s.momVTol, &s.momWTol,
        &s.turbKTol, &s.turbOmegaTol
    };
    bool* gates[6] = {
        &s.gateContinuity, &s.gateMomU, &s.gateMomV, &s.gateMomW,
        &s.gateTurbK, &s.gateTurbOmega
    };

    for (int i = 0; i < 6; ++i) {
        *gates[i] = m_rows[i].enabled->isChecked();
        if (m_rows[i].enabled->isChecked()) {
            bool ok = false;
            double v = parseTol(m_rows[i].tolEdit->text(), &ok);
            *tols[i] = ok ? std::make_optional(v) : std::nullopt;
        } else {
            *tols[i] = std::nullopt;
        }
    }

    if (m_fixedIterCb->isChecked()) {
        bool ok = false;
        long v = m_fixedIterEdit->text().toLong(&ok);
        if (ok && v > 0) {
            s.fixedIterations = static_cast<std::size_t>(v);
            s.runFixedIterations = true;
        } else {
            s.fixedIterations = std::nullopt;
            s.runFixedIterations = false;
        }
    } else {
        s.fixedIterations = std::nullopt;
        s.runFixedIterations = false;
    }
    s.convergenceHoldIterations = m_holdItersSpin->value();
}

void StoppingCriteriaDialog::setFrom(const cfd::CaseSettings& s) {
    std::optional<double> tols[6] = {
        s.contTol, s.momUTol, s.momVTol, s.momWTol,
        s.turbKTol, s.turbOmegaTol
    };
    bool gates[6] = {
        s.gateContinuity, s.gateMomU, s.gateMomV, s.gateMomW,
        s.gateTurbK, s.gateTurbOmega
    };

    for (int i = 0; i < 6; ++i) {
        m_rows[i].enabled->setChecked(gates[i]);
        m_rows[i].tolEdit->setEnabled(gates[i]);
        if (tols[i]) {
            m_rows[i].tolEdit->setText(fmtTol(*tols[i]));
        } else {
            m_rows[i].tolEdit->setText(fmtTol(cfd::kDefaultResidualTolerance));
        }
    }

    m_fixedIterCb->setChecked(s.runFixedIterations);
    m_fixedIterEdit->setEnabled(s.runFixedIterations);
    if (s.fixedIterations) {
        m_fixedIterEdit->setText(QString::number(*s.fixedIterations));
    } else {
        m_fixedIterEdit->setText("500");
    }
    m_holdItersSpin->setValue(static_cast<int>(s.convergenceHoldIterations));
    updateOkState();
}

bool StoppingCriteriaDialog::isConfigured() const {
    for (auto& row : m_rows) {
        if (row.enabled->isChecked()) return true;
    }
    return m_fixedIterCb->isChecked();
}
