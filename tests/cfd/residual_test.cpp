// Residual reporting.
//
// Contract pinned here:
//   - the continuity residual is the mass imbalance normalised by the total
//     flux, and it covers the boundary cells as well (a wall cell's imbalance
//     is a genuine solver error);
//   - the momentum residual is the relative per-iteration CHANGE of the
//     velocity, not a linear residual: momU = max|delta u| / max|u|.
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

// The momentum residual is a relative CHANGE, so it is fully determined by
// the difference between the two velocity fields.
TEST_CASE("cfd.residual.momentum_residual_is_a_change") {
    const fvm::Mesh<2> mesh = uniform2D(6);
    fvm::VectorField<2> velocity(mesh, 0.0);
    velocity[0].assign(1.0);

    fvm::VectorField<2> velocityOld(mesh, 0.0);
    velocityOld[0].assign(1.0);

    // One interior cell changed by 1%.
    const std::size_t P = mesh.cellIndex(2, 2);
    velocity[0][P] = 1.01;

    std::vector<std::vector<char>> mask(2, std::vector<char>(mesh.nCells(), 0));
    std::vector<std::vector<double>> values(2, std::vector<double>(mesh.nCells(), 0.0));
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (!mesh.isBoundary(i))
            continue;
        mask[0][i] = 1;
        mask[1][i] = 1;
    }

    std::array<fvm::Field<2>, 2> diag{fvm::Field<2>(mesh, 1.0), fvm::Field<2>(mesh, 1.0)};
    fvm::Field<2> divBuffer(mesh, 0.0);

    const fvm::ResidualResult res = fvm::computeResiduals(
        mesh, velocity, velocityOld, diag, divBuffer, mask, values);

    // max|delta u| = 0.01, the scale is the largest |u| over the interior
    // cells, which is the perturbed cell.
    const double expected = 0.01 / 1.01;
    MESSAGE("momentum residual = ", res.momU, " (expected ", expected, ")");
    CHECK_MESSAGE(std::abs(res.momU - expected) < 1e-12,
                  "the momentum residual must be the relative velocity change");
}

} // TEST_SUITE("cfd")
