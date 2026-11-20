#include "doctest/doctest.h"
#include "cfd/convergence_policy.h"

#include <optional>

TEST_SUITE("cfd") {

TEST_CASE("cfd.convergence.transient_gate_uses_one_iteration_hold") {
    cfd::TransientStoppingPolicy p;
    p.activeEquations = {cfd::ResidualEquation::Continuity, cfd::ResidualEquation::MomentumX};
    p.effectiveTolerances = {1e-3, 1e-3};

    const cfd::SteadyStoppingPolicy steady = cfd::steadyPolicyFromTransient(p);
    CHECK(steady.convergenceHoldIterations == 1);
    CHECK(steady.residualGateEnabled);
    CHECK(steady.activeEquations == p.activeEquations);
    CHECK(steady.effectiveTolerances == p.effectiveTolerances);

    cfd::ConvergenceMonitor monitor(steady);
    CHECK(monitor.update(1e-4, 1e-4, 1e-4, std::nullopt, std::nullopt, std::nullopt));
}

} // TEST_SUITE("cfd")
