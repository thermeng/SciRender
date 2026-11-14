#include "stopping_criteria_model.h"

#include <algorithm>

namespace {

EquationRow makeRow(const cfd::EquationDescriptor& d,
                    const cfd::StoppingCriteria& s) {
    std::size_t idx = static_cast<std::size_t>(d.id);
    EquationRow r;
    r.id = d.id;
    r.name = d.name;
    r.tip = d.tip;
    if (idx < s.equations.size()) {
        const auto& eq = s.equations[idx];
        r.enabled = eq.enabled;
        r.hasOverride = eq.tolerance.has_value();
        r.tolerance = eq.tolerance.value_or(s.defaultTolerance);
    }
    return r;
}

} // namespace

StoppingCriteriaModel::StoppingCriteriaModel(QObject* parent)
    : QObject(parent) {
    draft_ = cfd::defaultStoppingCriteria();
    baseline_ = draft_;
}

void StoppingCriteriaModel::setContext(bool dim3, bool turbulence,
                                       bool transient) {
    if (dim3 == dim3_ && turbulence == turbulence_ && transient == transient_)
        return;
    dim3_ = dim3;
    turbulence_ = turbulence;
    transient_ = transient;
    rebuildRows();
    validateAndNotify();
    emit contextChanged();
}

void StoppingCriteriaModel::load(const cfd::StoppingCriteria& s) {
    draft_ = s;
    baseline_ = s;
    rebuildRows();
    validateAndNotify();
}

void StoppingCriteriaModel::rebuildRows() {
    rows_.clear();
    rows_.reserve(cfd::kResidualEquationCount);
    for (const auto& d : cfd::equationCatalog()) {
        EquationRow r = makeRow(d, draft_);
        r.applicable = [&]() {
            switch (d.availability) {
                case cfd::EquationAvailability::Always: return true;
                case cfd::EquationAvailability::Requires3D: return dim3_;
                case cfd::EquationAvailability::RequiresTurbulence:
                    return turbulence_;
            }
            return false;
        }();
        rows_.push_back(std::move(r));
    }
}

void StoppingCriteriaModel::validateAndNotify() {
    auto rep = validationReport();
    bool valid = rep.ok;
    if (valid != valid_) {
        valid_ = valid;
        emit validationChanged(valid);
    }
}

cfd::ValidationReport StoppingCriteriaModel::validationReport() const {
    return cfd::validateStoppingCriteria(draft_);
}

bool StoppingCriteriaModel::isValid() const { return valid_; }

bool StoppingCriteriaModel::hasChanges() const {
    return draft_ != baseline_;
}

std::optional<cfd::StoppingCriteria> StoppingCriteriaModel::commit() {
    if (!isValid()) return std::nullopt;
    baseline_ = draft_;
    return draft_;
}

void StoppingCriteriaModel::resetToDefaults() {
    draft_ = cfd::defaultStoppingCriteria();
    rebuildRows();
    validateAndNotify();
    emit changed();
}

void StoppingCriteriaModel::setEquationEnabled(cfd::ResidualEquation id,
                                               bool enabled) {
    std::size_t idx = static_cast<std::size_t>(id);
    if (idx >= draft_.equations.size()) return;
    draft_.equations[idx].enabled = enabled;
    for (auto& r : rows_) {
        if (r.id == id) r.enabled = enabled;
    }
    validateAndNotify();
    emit changed();
}

void StoppingCriteriaModel::setEquationOverride(cfd::ResidualEquation id,
                                                std::optional<double> tolerance) {
    std::size_t idx = static_cast<std::size_t>(id);
    if (idx >= draft_.equations.size()) return;
    draft_.equations[idx].tolerance = tolerance;
    for (auto& r : rows_) {
        if (r.id == id) {
            r.hasOverride = tolerance.has_value();
            r.tolerance = tolerance.value_or(draft_.defaultTolerance);
        }
    }
    validateAndNotify();
    emit changed();
}

void StoppingCriteriaModel::setDefaultTolerance(double v) {
    draft_.defaultTolerance = v;
    for (auto& r : rows_) {
        if (!r.hasOverride) r.tolerance = v;
    }
    validateAndNotify();
    emit changed();
}

void StoppingCriteriaModel::setSteadyMaxIterations(std::size_t v) {
    draft_.steady.maxIterations = v;
    validateAndNotify();
    emit changed();
}

void StoppingCriteriaModel::setSteadyFixedIterations(
    std::optional<std::size_t> v) {
    draft_.steady.fixedIterations = v;
    validateAndNotify();
    emit changed();
}

void StoppingCriteriaModel::setSteadyHold(int v) {
    draft_.steady.convergenceHoldIterations = v;
    validateAndNotify();
    emit changed();
}

void StoppingCriteriaModel::setTransientTEnd(double v) {
    draft_.transient.tEnd = v;
    validateAndNotify();
    emit changed();
}

void StoppingCriteriaModel::setTransientMaxOuterCorrectors(int v) {
    draft_.transient.maxOuterCorrectors = v;
    validateAndNotify();
    emit changed();
}

void StoppingCriteriaModel::setTransientMinOuterCorrectors(int v) {
    draft_.transient.minOuterCorrectors = v;
    validateAndNotify();
    emit changed();
}

void StoppingCriteriaModel::setTransientResidualReduction(double v) {
    draft_.transient.residualReduction = v;
    validateAndNotify();
    emit changed();
}
