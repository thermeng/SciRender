// Pressure correction: the pressure equation and its coupling to the velocity.
//
// Contract pinned here:
//   - A p' = b with A = -div(d grad(p')) and b = -div(u*) * V: a uniform flow
//     has no divergence and must therefore produce no pressure correction;
//   - the pinned reference cell carries p' = 0;
//   - the correction must remove the divergence it was built for;
//   - a cell without any interior face must not be silently turned into a
//     Dirichlet cell.
#include "doctest/doctest.h"
#include "fvm/Divergence.h"
#include "fvm/PressureCorrection.h"
#include "fvm/VelocityCorrection.h"
#include "support/fvm_fixtures.h"

#include <array>
#include <cmath>
#include <vector>

using namespace test_support;

namespace {

std::array<std::vector<double>, 2> uniformDCoeff(const fvm::Mesh<2>& mesh, double value) {
    return {std::vector<double>(mesh.nCells(), value),
            std::vector<double>(mesh.nCells(), value)};
}

} // namespace

TEST_SUITE("cfd") {

// A uniform flow has no divergence, so the pressure correction must be zero
// everywhere. A non-zero correction here would mean a spurious pressure
// gradient driving the flow.
TEST_CASE("cfd.pressure.rhie_chow_uniform_flow_gives_zero_pressure") {
    const fvm::Mesh<2> mesh = uniform2D(8);
    fvm::VectorField<2> uStar(mesh, 0.0);
    uStar[0].assign(1.0);
    fvm::Field<2> pressure(mesh, 0.0);
    fvm::Field<2> pPrime(mesh, 0.0);

    std::vector<char> isPressureNeumann(mesh.nCells(), 1);

    fvm::PressureCorrection<2> pc(mesh);
    pc.solve(uStar, uniformDCoeff(mesh, 1.0), pressure, pPrime, {}, isPressureNeumann);

    double worst = 0.0;
    for (std::size_t i = 0; i < mesh.nCells(); ++i)
        worst = std::max(worst, std::abs(pPrime[i]));
    MESSAGE("max |p'| = ", worst);
    CHECK_MESSAGE(worst < 1e-10, "a uniform flow must not produce a pressure correction");
}

// The pinned reference cell is the anchor of the otherwise singular pure
// Neumann problem: it must carry p' = 0.
TEST_CASE("cfd.pressure.reference_cell_pinned_to_zero") {
    const fvm::Mesh<2> mesh = uniform2D(8);
    fvm::VectorField<2> uStar(mesh, 0.0);
    uStar[0].assign(1.0);
    fvm::Field<2> pressure(mesh, 0.0);
    fvm::Field<2> pPrime(mesh, 0.0);

    std::vector<char> isPressureNeumann(mesh.nCells(), 1);

    fvm::PressureCorrection<2> pc(mesh);
    pc.solve(uStar, uniformDCoeff(mesh, 1.0), pressure, pPrime, {}, isPressureNeumann);

    const auto& pMask = pc.pressureMask();
    int pinned = 0;
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (pMask[i]) {
            ++pinned;
            CHECK_MESSAGE(std::abs(pPrime[i]) < 1e-12, "the pinned cell must carry p' = 0");
        }
    }
    CHECK_MESSAGE(pinned >= 1, "the pure Neumann problem needs a pinned reference cell");
}

// The pressure correction exists to remove the divergence of the predictor.
// After the correction the interior divergence must be much smaller than
// before.
TEST_CASE("cfd.pressure.correction_removes_divergence") {
    const fvm::Mesh<2> mesh = uniform2D(8);
    // A divergent predictor: u* = (x, 0) has div = 1.
    fvm::VectorField<2> uStar(mesh, 0.0);
    fillVector(uStar, [](double x, double, double) { return std::array<double, 2>{x, 0.0}; });

    fvm::Field<2> pressure(mesh, 0.0);
    fvm::Field<2> pPrime(mesh, 0.0);
    std::vector<char> isPressureNeumann(mesh.nCells(), 1);

    fvm::PressureCorrection<2> pc(mesh);
    pc.solve(uStar, uniformDCoeff(mesh, 1.0), pressure, pPrime, {}, isPressureNeumann);

    // Apply the correction the way the solver does.
    std::array<std::vector<double>, 2> dCoeff = uniformDCoeff(mesh, 1.0);
    std::vector<std::vector<char>> velMask(2, std::vector<char>(mesh.nCells(), 0));
    std::vector<std::vector<double>> velVals(2, std::vector<double>(mesh.nCells(), 0.0));
    std::array<fvm::Field<2>, 2> dpdx{fvm::Field<2>(mesh, 0.0), fvm::Field<2>(mesh, 0.0)};
    fvm::VectorField<2> corrected(mesh, 0.0);
    fvm::correctVelocity(mesh, uStar, pPrime, dCoeff, velMask, velVals, dpdx, corrected);

    fvm::Field<2> divBefore(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, uStar, divBefore);
    fvm::Field<2> divAfter(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, corrected, divAfter);

    double before = 0.0;
    double after = 0.0;
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (mesh.isBoundary(i))
            continue;
        before = std::max(before, std::abs(divBefore[i]));
        after = std::max(after, std::abs(divAfter[i]));
    }
    MESSAGE("interior max |div|: before = ", before, ", after = ", after);
    CHECK_MESSAGE(before > 0.5, "the predictor must actually be divergent");
    CHECK_MESSAGE(after < 0.1 * before, "the pressure correction must remove the divergence");
}

// A cell without a single interior face has no pressure equation. It must not
// be silently converted into a Dirichlet cell with p' = 0: that pins a
// pressure the physics never asked for.
TEST_CASE("cfd.pressure.isolated_cell_not_silently_pinned") {
    // A normal triangle plus a detached one: the detached cell has three
    // boundary faces and no interior face at all.
    std::vector<std::array<double, 2>> vertices = {
        {0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0},
        {5.0, 5.0}, {6.0, 5.0}, {5.0, 6.0},
    };
    std::vector<std::vector<std::size_t>> cells = {
        {0, 1, 2},
        {3, 4, 5},
    };
    const fvm::Mesh<2> mesh(std::move(vertices), std::move(cells));

    fvm::VectorField<2> uStar(mesh, 0.0);
    uStar[0].assign(1.0);
    fvm::Field<2> pressure(mesh, 0.0);
    fvm::Field<2> pPrime(mesh, 0.0);
    std::vector<char> isPressureNeumann(mesh.nCells(), 1);

    fvm::PressureCorrection<2> pc(mesh);
    pc.solve(uStar, uniformDCoeff(mesh, 1.0), pressure, pPrime, {}, isPressureNeumann);

    const auto& pMask = pc.pressureMask();
    // The detached cell (index 1) has no interior face.
    CHECK_MESSAGE(pMask.size() == mesh.nCells(), "the pressure mask must cover every cell");
    CHECK_MESSAGE(pMask[1] == 0, "a cell without an interior face must not be pinned");
}

} // TEST_SUITE("cfd")
