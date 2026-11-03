#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QDialogButtonBox>
#include <QRegularExpressionValidator>
#include <QRegularExpression>
#include <optional>
#include <string>
#include <vector>

#include "cfd/cfd_types.h"

// Dialog for configuring per-equation stopping criteria.
//
// Each equation row has a checkbox (enable/disable gating on that equation)
// and a QLineEdit for the residual tolerance (scientific notation accepted).
// A "Fixed iteration limit" row overrides maxIterations when checked.
//
// Results are written into cfd::CaseSettings via settings().
class StoppingCriteriaDialog : public QDialog {
    Q_OBJECT
public:
    explicit StoppingCriteriaDialog(QWidget* parent = nullptr);

    // Apply the current settings into s (called on Accept).
    void applyTo(cfd::CaseSettings& s) const;
    // Populate the dialog from s (called before exec()).
    void setFrom(const cfd::CaseSettings& s);

    // True if any equation is gated or fixed iteration is enabled.
    bool isConfigured() const;

private:
    struct EquationRow {
        QCheckBox*  enabled = nullptr;
        QLineEdit*  tolEdit = nullptr;
        QLabel*     nameLabel = nullptr;
    };

    QGridLayout* m_grid = nullptr;
    EquationRow  m_rows[6];

    QCheckBox*   m_fixedIterCb = nullptr;
    QLineEdit*   m_fixedIterEdit = nullptr;
    QSpinBox*    m_holdItersSpin = nullptr;

    QDialogButtonBox* m_buttons = nullptr;

    void buildRow(int idx, const QString& name, const QString& tip);
    void updateOkState();
    static QString fmtTol(double v);
    static double parseTol(const QString& text, bool* ok = nullptr);
};
