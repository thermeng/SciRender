// Residual reporting: outcomes, not formula bookkeeping.
//
// Contract pinned here:
//   - a compatible uniform flow has no mass imbalance: the continuity
//     residual is (near) zero;
//   - an unchanged velocity has no momentum change;
//   - a larger velocity change gives a larger momentum residual: the
//     residual reflects how much the flow moved, so it can be used to judge
//     convergence. The exact normalization constant is a reporting choice
//     and is not pinned.
#include "doctest/doctest.h"
#include "fvm/Residual.h"
#include "support/fvm_fixtures.h"

#include <array>
#include <cmath>
#include <vector>

using namespace test_support;

TEST_SUITE("cfd") {

// A uniform flow with matching wall conditions has no mass imbalance: the
// continuity residual must be zero.
TEST_CASE("cfd.residual.uniform_flow_continuity_zero") {
    const fvm::Mesh<2> mesh = uniform2D(6);
    fvm::VectorField<2> velocity(mesh, 0.0);
    velocity[0].assign(1.0);
    fvm::VectorField<2> velocityOld(mesh, 0.0);
    velocityOld[0].assign(1.0);

    // The walls prescribe the same velocity as the interior, so the flow is
    // compatible with its boundary conditions and carries no mass imbalance.
    std::vector<std::vector<char>> mask(2, std::vector<char>(mesh.nCells(), 0));
    std::vector<std::vector<double>> values(2, std::vector<double>(mesh.nCells(), 0.0));
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (!mesh.isBoundary(i))
            continue;
        mask[0][i] = 1;
        mask[1][i] = 1;
        values[0][i] = 1.0;
        values[1][i] = 0.0;
    }

    std::array<fvm::Field<2>, 2> diag{fvm::Field<2>(mesh, 1.0), fvm::Field<2>(mesh, 1.0)};
    fvm::Field<2> divBuffer(mesh, 0.0);

    const fvm::ResidualResult res = fvm::computeResiduals(
        mesh, velocity, velocityOld, diag, divBuffer, mask, values);

    MESSAGE("continuity residual = ", res.continuity);
    CHECK_MESSAGE(res.continuity < 1e-12, "a uniform flow has no mass imbalance");
    CHECK_MESSAGE(std::abs(res.momU) < 1e-15, "an unchanged velocity has no momentum change");
}

// The momentum residual must rank velocity changes: no change < small
// change < large change. Pinning the ordering (not an exact normalization)
// keeps the test useful for convergence judgment across reporting choices.
TEST_CASE("cfd.residual.momentum_residual_ranks_velocity_changes") {
    const fvm::Mesh<2> mesh = uniform2D(6);
    std::vector<std::vector<char>> mask(2, std::vector<char>(mesh.nCells(), 0));
    std::vector<std::vector<double>> values(2, std::vector<double>(mesh.nCells(), 0.0));
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (!mesh.isBoundary(i))
            continue;
        mask[0][i] = 1;
        mask[1][i] = 1;
    }
    std::array<fvm::Field<2>, 2> diag{fvm::Field<2>(mesh, 1.0), fvm::Field<2>(mesh, 1.0)};

    const auto momentumResidualForPerturbation = [&](double factor) {
        fvm::VectorField<2> velocity(mesh, 0.0);
        velocity[0].assign(1.0);
        fvm::VectorField<2> velocityOld(mesh, 0.0);
        velocityOld[0].assign(1.0);
        const std::size_t P = mesh.cellIndex(2, 2);
        velocity[0][P] = 1.0 * factor;
        fvm::Field<2> divBuffer(mesh, 0.0);
        const fvm::ResidualResult res = fvm::computeResiduals(
            mesh, velocity, velocityOld, diag, divBuffer, mask, values);
        return std::abs(res.momU);
    };

    const double rUnchanged = momentumResidualForPerturbation(1.0);
    const double rSmall = momentumResidualForPerturbation(1.01);
    const double rLarge = momentumResidualForPerturbation(1.10);
    MESSAGE("momentum residual: unchanged ", rUnchanged, " small ", rSmall, " large ", rLarge);
    CHECK_MESSAGE(rUnchanged < 1e-15, "an unchanged velocity must give a near-zero residual");
    CHECK_MESSAGE(rSmall > rUnchanged, "a small change must give a larger residual than no change");
    CHECK_MESSAGE(rLarge > rSmall, "a larger change must give a larger residual than a small change");
    CHECK_MESSAGE(rSmall > 1e-4, "a 1% change must give a visible residual");
    CHECK_MESSAGE(rSmall < 1.0, "a 1% change must give a small residual");
}

} // TEST_SUITE("cfd")
