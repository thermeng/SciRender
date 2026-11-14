#pragma once

#include "cfd/convergence_policy.h"
#include "cfd/stopping_criteria.h"

#include <QObject>
#include <optional>
#include <QString>
#include <vector>

// Lightweight per-equation row exposed to the stopping-criteria dialog.
struct EquationRow {
    cfd::ResidualEquation id;
    QString name;
    QString tip;
    bool applicable = false;   // usable in the current dimension/mode/turbulence
    bool enabled = true;       // gated in the convergence test
    bool hasOverride = false;  // true = explicit tolerance, false = inherit default
    double tolerance = 1e-4;   // the override value (only meaningful if hasOverride)
};

// Editor model for the Stopping Criteria dialog. Owns a draft
// cfd::StoppingCriteria plus the solve context, performs validation, and
// emits signals so the view can enable/disable OK and show inline errors.
class StoppingCriteriaModel : public QObject {
    Q_OBJECT
public:
    explicit StoppingCriteriaModel(QObject* parent = nullptr);

    // Solve context: dimension (3D?), turbulence enabled, transient mode.
    // Changing the context re-filters which equations are applicable.
    void setContext(bool dim3, bool turbulence, bool transient);
    bool dim3() const { return dim3_; }
    bool turbulence() const { return turbulence_; }
    bool transient() const { return transient_; }

    // Load the draft from the applied settings (called when the dialog opens
    // or when settings change externally).
    void load(const cfd::StoppingCriteria& s);
    cfd::StoppingCriteria draft() const { return draft_; }
    bool hasChanges() const;

    // Validation. isValid() is the single gate for the OK button.
    cfd::ValidationReport validationReport() const;
    bool isValid() const;

    // Commit: returns the draft if valid, nullopt otherwise.
    std::optional<cfd::StoppingCriteria> commit();

    void resetToDefaults();

    // Per-equation mutations.
    void setEquationEnabled(cfd::ResidualEquation id, bool enabled);
    void setEquationOverride(cfd::ResidualEquation id, std::optional<double> tolerance);

    // Global / policy mutations.
    void setDefaultTolerance(double v);
    void setSteadyMaxIterations(std::size_t v);
    void setSteadyFixedIterations(std::optional<std::size_t> v);
    void setSteadyHold(int v);
    void setTransientTEnd(double v);
    void setTransientMaxOuterCorrectors(int v);
    void setTransientMinOuterCorrectors(int v);
    void setTransientResidualReduction(double v);

    const std::vector<EquationRow>& equations() const { return rows_; }

signals:
    void changed();
    void validationChanged(bool valid);
    void contextChanged();

private:
    void rebuildRows();
    void validateAndNotify();

    cfd::StoppingCriteria draft_;
    cfd::StoppingCriteria baseline_;
    bool dim3_ = false;
    bool turbulence_ = false;
    bool transient_ = false;
    std::vector<EquationRow> rows_;
    bool valid_ = false;
};
