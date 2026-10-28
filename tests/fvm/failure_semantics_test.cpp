// Failure semantics: an aborted run must say *why* it aborted, and a linear
// solve that returns non-finite values must count as a hard failure instead of
// being silently repaired and then reported as "not converged".
#include "doctest/doctest.h"

#include "fvm/SIMPLE.h"

#include <array>
#include <limits>
#include <string>

TEST_CASE("fvm.simple.non_finite_momentum_solve_aborts_with_a_reason") {
    fvm::Mesh<2> mesh(std::array<std::size_t, 2>{4, 4},
                      std::array<double, 2>{1.0, 1.0});

    fvm::SIMPLE<2>::Config cfg;
    cfg.maxIterations = 3;
    cfg.printFrequency = 1000000;
    cfg.tolerance = 1e-6;
    fvm::SIMPLE<2> solver(mesh, 1.0, 1e-2, cfg);

    fvm::VectorField<2> velocity(mesh, 0.0);
    fvm::Field<2> pressure(mesh, 0.0);
    // A poisoned cell makes every momentum right-hand side non-finite. The
    // solver must notice the non-finite solution, not clamp it away and carry
    // on towards a bogus convergence verdict.
    velocity[0][5] = std::numeric_limits<double>::quiet_NaN();

    solver.solve(velocity, pressure);

    CHECK(solver.status() == fvm::SIMPLE<2>::Status::Aborted);
    CHECK(solver.failure() == fvm::SIMPLE<2>::Failure::MomentumSolveFailed);
    CHECK(std::string(solver.failureText()).find("momentum") != std::string::npos);
}

TEST_CASE("fvm.simple.healthy_run_has_no_failure_reason") {
    fvm::Mesh<2> mesh(std::array<std::size_t, 2>{4, 4},
                      std::array<double, 2>{1.0, 1.0});

    fvm::SIMPLE<2>::Config cfg;
    cfg.maxIterations = 2;
    cfg.printFrequency = 1000000;
    fvm::SIMPLE<2> solver(mesh, 1.0, 1e-2, cfg);

    fvm::VectorField<2> velocity(mesh, 0.0);
    fvm::Field<2> pressure(mesh, 0.0);
    solver.solve(velocity, pressure);

    CHECK(solver.status() != fvm::SIMPLE<2>::Status::Aborted);
    CHECK(solver.failure() == fvm::SIMPLE<2>::Failure::None);
    CHECK(std::string(solver.failureText()).empty());
}
