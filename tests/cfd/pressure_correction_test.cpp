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

// One pressure-correction solve on a predictor with a zero-mean divergence,
// plus everything the coupling can be judged by.
struct CouplingCase {
    double divergenceBefore = 0.0;
    double divergenceAfter = 0.0;
    double maxP = 0.0;
    // The ratio between the pressure matrix's row (divided by the cell
    // volume) and the divergence of the gradient the velocity correction
    // applies. It must be 1: the two operators have to be the same one.
    double operatorRatio = 0.0;
};

CouplingCase solveCouplingCase(const fvm::Mesh<2>& mesh) {
    CouplingCase result;

    // A predictor with a zero-mean divergence: it expands in the left half of
    // the domain and contracts in the right half, so the pure Neumann pressure
    // problem is compatible.
    fvm::VectorField<2> uStar(mesh, 0.0);
    fillVector(uStar, [](double x, double, double) {
        const double u = (x < 0.5) ? x : (1.0 - x);
        return std::array<double, 2>{u, 0.0};
    });

    fvm::Field<2> pressure(mesh, 0.0);
    fvm::Field<2> pPrime(mesh, 0.0);
    std::vector<char> isPressureNeumann(mesh.nCells(), 1);

    fvm::PressureCorrection<2> pc(mesh);
    pc.solve(uStar, uniformDCoeff(mesh, 1.0), pressure, pPrime, {}, isPressureNeumann);

    // Apply the correction the way the solver does.
    const std::array<std::vector<double>, 2> dCoeff = uniformDCoeff(mesh, 1.0);
    std::vector<std::vector<char>> velMask(2, std::vector<char>(mesh.nCells(), 0));
    std::vector<std::vector<double>> velVals(2, std::vector<double>(mesh.nCells(), 0.0));
    std::array<fvm::Field<2>, 2> dpdx{fvm::Field<2>(mesh, 0.0), fvm::Field<2>(mesh, 0.0)};
    fvm::VectorField<2> corrected(mesh, 0.0);
    fvm::correctVelocity(mesh, uStar, pPrime, dCoeff, velMask, velVals, dpdx, corrected);

    fvm::Field<2> divBefore(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, uStar, divBefore, {}, {});
    fvm::Field<2> divAfter(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, corrected, divAfter, {}, {});

    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        result.maxP = std::max(result.maxP, std::abs(pPrime[i]));
        if (mesh.isBoundary(i))
            continue;
        result.divergenceBefore = std::max(result.divergenceBefore, std::abs(divBefore[i]));
        result.divergenceAfter = std::max(result.divergenceAfter, std::abs(divAfter[i]));
    }

    // The operator consistency: the pressure matrix's row divided by the cell
    // volume must equal the divergence of the gradient the velocity correction
    // applies.
    const auto& A = pc.lastMatrix();
    Eigen::VectorXd pVec(mesh.nCells());
    for (std::size_t i = 0; i < mesh.nCells(); ++i)
        pVec[i] = pPrime[i];
    const Eigen::VectorXd Ap = A * pVec;

    fvm::VectorField<2> correction(mesh, 0.0);
    for (std::size_t d = 0; d < 2; ++d)
        for (std::size_t i = 0; i < mesh.nCells(); ++i)
            correction[d][i] = dCoeff[d][i] * dpdx[d][i];
    fvm::Field<2> divOfCorrection(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, correction, divOfCorrection, {}, {});

    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        if (mesh.isBoundary(i))
            continue;
        if (pc.pressureMask()[i])
            continue; // the pinned cells carry no flux equation
        const double fromMatrix = Ap[static_cast<int>(i)] / mesh.cellVolume(i);
        const double fromDivergence = divOfCorrection[i];
        const double ratio = std::abs(fromDivergence) / std::max(std::abs(fromMatrix), 1e-30);
        result.operatorRatio = std::max(result.operatorRatio, ratio);
    }
    return result;
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
//
// The source must be COMPATIBLE: the pressure equation of a closed domain is
// pure Neumann, so it only has a solution when the right-hand side sums to
// zero. A predictor with a uniform divergence (div = 1 everywhere) does not
// satisfy that - the walls would have to supply the missing mass - and the
// solver's reference cell then has to absorb an impossible balance, which
// says nothing about the coupling.
//
// The predictor below therefore expands in the left half of the domain and
// contracts in the right half: div = +1 and -1, a zero mean over the domain.
TEST_CASE("cfd.pressure.correction_removes_divergence") {
    const CouplingCase c = solveCouplingCase(uniform2D(8));

    MESSAGE("interior max |div|: before = ", c.divergenceBefore, ", after = ", c.divergenceAfter,
            ", max |p'| = ", c.maxP);

    CHECK_MESSAGE(c.divergenceBefore > 0.5, "the predictor must actually be divergent");
    CHECK_MESSAGE(c.divergenceAfter < 0.1 * c.divergenceBefore,
                  "the pressure correction must remove the divergence");
}

// The pressure matrix and the gradient the velocity correction applies must be
// the SAME operator: the matrix's row divided by the cell volume has to equal
// the divergence of that gradient. Only then can the correction cancel the
// divergence it was built for.
//
// Today they are not: the matrix assembles the distance-weighted (5-point)
// Laplacian, while the correction subtracts the divergence of the cell-centred
// least-squares gradient. The two agree to O(h^2) in smooth regions and
// diverge where the field has strong curvature - which is exactly where the
// solver leaves the largest mass-balance error.
TEST_CASE("cfd.pressure.operator_consistency") {
    const CouplingCase c = solveCouplingCase(uniform2D(8));

    MESSAGE("operator consistency: |div(d grad p')| / |(A p')/V| = ", c.operatorRatio);
    CHECK_MESSAGE(c.operatorRatio < 1.2,
                  "the pressure matrix and the correction's gradient must be the same operator "
                      "(ratio " << c.operatorRatio << ")");
}

// A cell without a single interior face has no pressure equation. It is
// excluded from the pressure unknowns and REPORTED: its mass balance can never
// be corrected, so a run containing such a cell never converges - and the
// solver has to say so instead of silently pretending the cell is fine.
TEST_CASE("cfd.pressure.isolated_cell_is_reported") {
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

    // The detached cell (index 1) has no interior face.
    CHECK_MESSAGE(pc.isolatedCellCount() == 1,
                  "a cell without a pressure equation must be reported");
    CHECK_MESSAGE(pc.pressureMask()[1] == 1,
                  "the isolated cell must be excluded from the pressure unknowns");
    CHECK_MESSAGE(std::abs(pPrime[1]) < 1e-12,
                  "the excluded cell carries no pressure correction");
}

} // TEST_SUITE("cfd")
