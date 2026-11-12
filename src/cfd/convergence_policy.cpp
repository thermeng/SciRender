#include "cfd/convergence_policy.h"

#include <algorithm>

namespace cfd {

namespace {

bool equationApplicable(ResidualEquation id, bool dim3, bool turbulence) {
    const auto& desc = equationCatalog()[static_cast<std::size_t>(id)];
    switch (desc.availability) {
        case EquationAvailability::Always: return true;
        case EquationAvailability::Requires3D: return dim3;
        case EquationAvailability::RequiresTurbulence: return turbulence;
    }
    return false;
}

} // namespace

std::optional<ResolvedStoppingPolicy> resolveStoppingPolicy(
    const StoppingCriteria& s, bool dim3, bool turbulence, bool transient) {

    ValidationReport report = validateStoppingCriteria(s);
    if (!report.ok) return std::nullopt;

    ResolvedStoppingPolicy policy;
    policy.transient = transient;

    std::vector<ResidualEquation> active;
    std::vector<double> effTols;
    active.reserve(kResidualEquationCount);
    effTols.reserve(kResidualEquationCount);
    for (std::size_t i = 0; i < kResidualEquationCount; ++i) {
        ResidualEquation id = static_cast<ResidualEquation>(i);
        if (!equationApplicable(id, dim3, turbulence)) continue;
        if (!s.equations[i].enabled) continue;
        auto tol = effectiveTolerance(s, id);
        if (!tol) continue; // should not happen: enabled implies a value
        active.push_back(id);
        effTols.push_back(*tol);
    }

    if (transient) {
        policy.transientPolicy.activeEquations = std::move(active);
        policy.transientPolicy.effectiveTolerances = std::move(effTols);
        policy.transientPolicy.tEnd = s.transient.tEnd;
        policy.transientPolicy.maxOuterCorrectors = s.transient.maxOuterCorrectors;
    } else {
        if (active.empty() && !s.steady.fixedIterations) {
            // No applicable equation is gated and there is no fixed budget:
            // the run can never converge and has no hard cap. Refuse.
            return std::nullopt;
        }
        policy.steady.activeEquations = std::move(active);
        policy.steady.effectiveTolerances = std::move(effTols);
        if (s.steady.fixedIterations) {
            policy.steady.fixedIterations = *s.steady.fixedIterations;
            policy.steady.iterationLimit = *s.steady.fixedIterations;
            policy.steady.residualGateEnabled = false;
        } else {
            policy.steady.fixedIterations = std::nullopt;
            policy.steady.iterationLimit = s.steady.maxIterations;
            policy.steady.residualGateEnabled = true;
        }
        policy.steady.convergenceHoldIterations = s.steady.convergenceHoldIterations;
    }

    return policy;
}

ConvergenceMonitor::ConvergenceMonitor(const SteadyStoppingPolicy& policy)
    : policy_(policy) {}

bool ConvergenceMonitor::update(double contResid, double momU, double momV,
                                std::optional<double> momW,
                                std::optional<double> kResid,
                                std::optional<double> omegaResid) {
    if (!policy_.residualGateEnabled) {
        converged_ = false;
        streak_ = 0;
        return false;
    }

    auto below = [](double v, double t) { return v < t; };

    bool ok = true;
    // activeEquations order matches effectiveTolerances order.
    for (std::size_t i = 0; i < policy_.activeEquations.size(); ++i) {
        double t = policy_.effectiveTolerances[i];
        switch (policy_.activeEquations[i]) {
            case ResidualEquation::Continuity:    ok &= below(contResid, t); break;
            case ResidualEquation::MomentumX:     ok &= below(momU, t); break;
            case ResidualEquation::MomentumY:     ok &= below(momV, t); break;
            case ResidualEquation::MomentumW:     ok &= momW.has_value() && below(*momW, t); break;
            case ResidualEquation::TurbulenceK:   ok &= kResid.has_value() && below(*kResid, t); break;
            case ResidualEquation::TurbulenceOmega:
                ok &= omegaResid.has_value() && below(*omegaResid, t); break;
        }
    }

    if (ok) {
        ++streak_;
        if (streak_ >= std::max(1, policy_.convergenceHoldIterations)) {
            converged_ = true;
            return true;
        }
    } else {
        streak_ = 0;
    }
    return false;
}

} // namespace cfd
