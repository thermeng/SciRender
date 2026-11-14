#pragma once

#include "cfd/stopping_criteria.h"
#include "ui/widgets/stopping_criteria_model.h"

#include <QDialog>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QSpinBox>
#include <memory>

namespace cfd {

// Per-equation row of widgets owned by the dialog's equation grid.
struct RowWidgets {
    QCheckBox* enable = nullptr;
    QCheckBox* override_ = nullptr;
    QLineEdit* tolerance = nullptr;
};

// Stopping-criteria editor. A thin Qt view over StoppingCriteriaModel: the
// model owns the draft, validation and context; the dialog lays out controls
// and translates gestures into model mutations. On Accept the dialog returns
// the validated StoppingCriteria via result(); on Cancel nothing is applied.
class StoppingCriteriaDialog : public QDialog {
    Q_OBJECT
public:
    explicit StoppingCriteriaDialog(QWidget* parent = nullptr);
    ~StoppingCriteriaDialog() override;

    // (Re)populate from the applied settings and the current solve context,
    // then open modally. Returns true when the user accepted.
    bool edit(const cfd::StoppingCriteria& s, bool dim3, bool turbulence,
              bool transient);

    // The validated criteria from the last accepted exec(); nullopt if the
    // dialog was cancelled or the draft was invalid.
    std::optional<cfd::StoppingCriteria> result() const { return result_; }

    bool dim3() const { return dim3_; }

private:
    void rebuildEquations();
    void rebuildPolicy();
    void updateOkState();
    void refreshToleranceTexts();

    std::unique_ptr<StoppingCriteriaModel> model_;

    QGroupBox* eqGroup_ = nullptr;
    QGridLayout* eqGrid_ = nullptr;

    // Steady controls.
    QGroupBox* steadyGroup_ = nullptr;
    QDoubleSpinBox* defaultTolSpin_ = nullptr;
    QSpinBox* maxIterSpin_ = nullptr;
    QCheckBox* fixedIterCb_ = nullptr;
    QSpinBox* fixedIterSpin_ = nullptr;
    QSpinBox* holdSpin_ = nullptr;

    // Transient controls.
    QGroupBox* transientGroup_ = nullptr;
    QDoubleSpinBox* tEndSpin_ = nullptr;
    QSpinBox* maxOuterSpin_ = nullptr;
    QSpinBox* minOuterSpin_ = nullptr;
    QDoubleSpinBox* reductionSpin_ = nullptr;

    QDialogButtonBox* buttons_ = nullptr;
    QPushButton* okBtn_ = nullptr;

    std::optional<cfd::StoppingCriteria> result_;

    // Solve context captured at edit() time (used for the context banner).
    bool dim3_ = false;
    bool turbulence_ = false;
    bool transient_ = false;

    QMap<cfd::ResidualEquation, RowWidgets> rowWidgets_;
};

} // namespace cfd
