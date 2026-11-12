#pragma once

#include "cfd/stopping_criteria.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace cfd {

// Resolved stopping policy for a single run context. Computed once at run
// start (and recomputed when settings are changed at a pause boundary).
//
// The solver reads this immutable snapshot; it never parses UI text or raw
// optional fields.
struct SteadyStoppingPolicy {
    // Equations that are both applicable (dimension/turbulence) and enabled.
    std::vector<ResidualEquation> activeEquations;
    // Effective tolerance per active equation (index matches activeEquations).
    std::vector<double> effectiveTolerances;
    // Iteration budget: fixedIterations if set, else maxIterations.
    std::size_t iterationLimit = 0;
    // Exact fixed budget (nullopt means use maxIterations as the cap).
    std::optional<std::size_t> fixedIterations;
    int convergenceHoldIterations = 3;
    // Residual gate is active (false when fixed mode bypasses it).
    bool residualGateEnabled = true;
};

struct TransientStoppingPolicy {
    // Equations that are both applicable and enabled (control corrector
    // early-exit only; the global run is bounded by tEnd).
    std::vector<ResidualEquation> activeEquations;
    // Effective tolerance per active equation.
    std::vector<double> effectiveTolerances;
    double tEnd = 1.0;
    int maxOuterCorrectors = 3;
};

struct ResolvedStoppingPolicy {
    bool transient = false;
    SteadyStoppingPolicy steady;
    TransientStoppingPolicy transientPolicy;
};

// Resolve StoppingCriteria against a solve context. Returns std::nullopt when
// the configuration is invalid (e.g. steady residual mode with no applicable
// active equation).
std::optional<ResolvedStoppingPolicy> resolveStoppingPolicy(
    const StoppingCriteria& s, bool dim3, bool turbulence, bool transient);

// Convergence monitor for steady SIMPLE. Tracks whether every active equation
// has stayed below its tolerance for `hold` consecutive iterations.
class ConvergenceMonitor {
public:
    explicit ConvergenceMonitor(const SteadyStoppingPolicy& policy);

    // Update with the current residual snapshot. Returns true when the
    // convergence criterion has held for `hold` consecutive iterations.
    // Returns false otherwise. A non-finite residual does not trip the monitor
    // itself (the solver aborts separately); it simply prevents convergence.
    bool update(double contResid, double momU, double momV,
                std::optional<double> momW,
                std::optional<double> kResid,
                std::optional<double> omegaResid);

    bool converged() const { return converged_; }
    int streak() const { return streak_; }

private:
    SteadyStoppingPolicy policy_;
    int streak_ = 0;
    bool converged_ = false;
};

} // namespace cfd
