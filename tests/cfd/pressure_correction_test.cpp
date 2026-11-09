// Pressure correction: the pressure equation and its coupling to the velocity.
//
// Contract pinned here (observable outcomes, not assembly internals):
//   - a uniform flow has no divergence and must produce no pressure correction;
//   - the pinned reference cell carries p' = 0;
//   - the correction must remove the divergence it was built for;
//   - a cell without any interior face must be reported, not silently fixed.
#include "doctest/doctest.h"
#include "fvm/Divergence.h"
#include "fvm/PressureCorrection.h"
#include "fvm/VelocityCorrection.h"
#include "support/fvm_fixtures.h"

#include <algorithm>
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
// judged by the observable outcome: how much divergence was removed.
struct CouplingCase {
    double divergenceBefore = 0.0;
    double divergenceAfter = 0.0;
    double divergenceAwayFromReference = 0.0;
    double maxP = 0.0;
};

CouplingCase solveCouplingCase(const fvm::Mesh<2>& mesh, bool smoothSource = false,
                               bool compact = false) {
    CouplingCase result;

    fvm::VectorField<2> uStar(mesh, 0.0);
    if (smoothSource) {
        fillVector(uStar, [](double x, double, double) {
            return std::array<double, 2>{std::sin(2.0 * M_PI * x), 0.0};
        });
    } else {
        fillVector(uStar, [](double x, double, double) {
            const double u = (x < 0.5) ? x : (1.0 - x);
            return std::array<double, 2>{u, 0.0};
        });
    }

    fvm::Field<2> pressure(mesh, 0.0);
    fvm::Field<2> pPrime(mesh, 0.0);

    fvm::PressureCorrection<2>::Config cfg;
    cfg.compactRhieChow = compact;
    cfg.useAMG = false;
    fvm::PressureCorrection<2> pc(mesh, cfg);
    pc.solve(uStar, uniformDCoeff(mesh, 1.0), pressure, pPrime, fvm::BCDiscretization{}, nullptr);

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

    {
        std::vector<char> nearPinned(mesh.nCells(), 0);
        for (std::size_t i = 0; i < mesh.nCells(); ++i) {
            if (!pc.pressureMask()[i])
                continue;
            for (std::size_t nb : mesh.cellNeighbors(i))
                nearPinned[nb] = 1;
            nearPinned[i] = 1;
        }
        for (std::size_t i = 0; i < mesh.nCells(); ++i) {
            if (mesh.isBoundary(i) || nearPinned[i])
                continue;
            result.divergenceAwayFromReference =
                std::max(result.divergenceAwayFromReference, std::abs(divAfter[i]));
        }
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

    fvm::PressureCorrection<2> pc(mesh);
    pc.solve(uStar, uniformDCoeff(mesh, 1.0), pressure, pPrime, fvm::BCDiscretization{}, nullptr);

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

    fvm::PressureCorrection<2> pc(mesh);
    pc.solve(uStar, uniformDCoeff(mesh, 1.0), pressure, pPrime, fvm::BCDiscretization{}, nullptr);

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
// before. The predictor expands in the left half and contracts in the right
// half, so the pure Neumann problem is compatible (zero mean divergence).
TEST_CASE("cfd.pressure.correction_removes_divergence") {
    const CouplingCase c = solveCouplingCase(uniform2D(8));

    MESSAGE("interior max |div|: before = ", c.divergenceBefore, ", after = ", c.divergenceAfter,
            ", max |p'| = ", c.maxP);

    CHECK_MESSAGE(c.divergenceBefore > 0.5, "the predictor must actually be divergent");
    CHECK_MESSAGE(c.divergenceAfter < 0.1 * c.divergenceBefore,
                  "the pressure correction must remove the divergence");
}

// The correction must remove most of the predictor divergence at every
// resolution: the observable mass-balance outcome.
TEST_CASE("cfd.pressure.correction_removes_divergence_at_refinement") {
    for (int n : {8, 16, 32}) {
        const CouplingCase c = solveCouplingCase(uniform2D(n));
        MESSAGE("n=", n, ": before=", c.divergenceBefore, " after=", c.divergenceAfter);
        CHECK_MESSAGE(c.divergenceBefore > 0.5, "the predictor must actually be divergent");
        CHECK_MESSAGE(c.divergenceAfter < 0.1 * c.divergenceBefore,
                      "the correction must remove the divergence at n=" << n);
    }
}

// The coupling must keep the corrected divergence small away from the
// reference cell at every resolution, for both a kinked and a smooth source.
TEST_CASE("cfd.pressure.coupling_error_scaling") {
    for (int smooth = 0; smooth < 2; ++smooth) {
        const char* label = smooth ? "smooth source" : "kinked source";
        for (int n : {8, 16, 32}) {
            const CouplingCase c = solveCouplingCase(uniform2D(n), smooth != 0);
            MESSAGE(label, ": n=", n, " h=", 1.0 / n, ": residual |div u| = ", c.divergenceAfter,
                    ", away from the reference cell = ", c.divergenceAwayFromReference);
            CHECK_MESSAGE(c.divergenceAwayFromReference < 1e-9,
                          "the coupling error must remain small away from the reference cell");
        }
    }
}

// A cell without a single interior face has no pressure equation. It is
// excluded from the pressure unknowns and REPORTED: its mass balance can never
// be corrected, so a run containing such a cell never converges - and the
// solver has to say so instead of silently pretending the cell is fine.
TEST_CASE("cfd.pressure.isolated_cell_is_reported") {
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

    fvm::PressureCorrection<2> pc(mesh);
    pc.solve(uStar, uniformDCoeff(mesh, 1.0), pressure, pPrime, fvm::BCDiscretization{}, nullptr);

    CHECK_MESSAGE(pc.isolatedCellCount() == 1,
                  "a cell without a pressure equation must be reported");
    CHECK_MESSAGE(pc.pressureMask()[1] == 1,
                  "the isolated cell must be excluded from the pressure unknowns");
    CHECK_MESSAGE(std::abs(pPrime[1]) < 1e-12,
                  "the excluded cell carries no pressure correction");
}

// A checkerboard pressure is invisible to the central cell gradient but has a
// large two-point face gradient. The compact operator must turn it into a
// correction that cancels the checkerboard instead of letting it survive.
TEST_CASE("cfd.pressure.compact_rhie_chow_cancels_checkerboard") {
    const fvm::Mesh<2> mesh = uniform2D(8);
    fvm::VectorField<2> uStar(mesh, 0.0);
    fvm::Field<2> pressure(mesh, 0.0);
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        const auto& c = mesh.cellCenter(i);
        const int ix = static_cast<int>(std::floor(c[0] * 8.0));
        const int iy = static_cast<int>(std::floor(c[1] * 8.0));
        pressure[i] = ((ix + iy) % 2 == 0) ? 1.0 : -1.0;
    }
    fvm::Field<2> pPrime(mesh, 0.0);

    fvm::PressureCorrection<2>::Config cfg;
    cfg.compactRhieChow = true;
    cfg.useAMG = false;
    fvm::PressureCorrection<2> pc(mesh, cfg);
    pc.solve(uStar, uniformDCoeff(mesh, 1.0), pressure, pPrime, fvm::BCDiscretization{}, nullptr);

    double sum = 0.0;
    int count = 0;
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        const auto& c = mesh.cellCenter(i);
        const int ix = static_cast<int>(std::floor(c[0] * 8.0));
        const int iy = static_cast<int>(std::floor(c[1] * 8.0));
        if (ix < 3 || ix > 4 || iy < 3 || iy > 4)
            continue;
        sum += pressure[i] + pPrime[i];
        ++count;
    }
    const double mean = sum / count;
    double amplitude = 0.0;
    double maxP = 0.0;
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        const auto& c = mesh.cellCenter(i);
        const int ix = static_cast<int>(std::floor(c[0] * 8.0));
        const int iy = static_cast<int>(std::floor(c[1] * 8.0));
        if (ix < 3 || ix > 4 || iy < 3 || iy > 4)
            continue;
        amplitude = std::max(amplitude,
                             std::abs(pressure[i] + pPrime[i] - mean));
        maxP = std::max(maxP, std::abs(pPrime[i]));
    }
    MESSAGE("checkerboard amplitude after correction = ", amplitude,
            ", max |p'| = ", maxP);
    CHECK_MESSAGE(amplitude < 0.05,
                  "the compact Rhie-Chow correction must remove the "
                  "checkerboard mode");
    CHECK_MESSAGE(maxP > 0.5,
                  "the correction must actually act on the checkerboard");
}

// With AMG enabled the pressure solve must still remove divergence and stay
// finite: the solver path is an implementation choice, the corrected
// divergence is the observable outcome. The predictor is divergent (expands
// left, contracts right) so the correction has something to remove.
TEST_CASE("cfd.pressure.amg_solve_removes_divergence") {
    const std::size_t N = 12;
    fvm::Mesh<2> mesh(std::array<std::size_t, 2>{N, N}, std::array<double, 2>{1.0, 1.0});

    fvm::VectorField<2> uStar(mesh, 0.0);
    fillVector(uStar, [](double x, double, double) {
        const double u = (x < 0.5) ? x : (1.0 - x);
        return std::array<double, 2>{u, 0.0};
    });

    fvm::Field<2> divBeforeField(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, uStar, divBeforeField, {}, {});
    double before = 0.0;
    for (std::size_t i = 0; i < mesh.nCells(); ++i)
        if (!mesh.isBoundary(i)) before = std::max(before, std::abs(divBeforeField[i]));

    fvm::PressureCorrection<2>::Config cfg;
    cfg.useAMG = true;
    cfg.useAMGPreconditioner = true;
    cfg.pressureAmgThreshold = 0;
    cfg.pressureAmgPreconditionerCycles = 2;
    fvm::PressureCorrection<2> pc(mesh, cfg);

    fvm::Field<2> pressure(mesh, 0.0);
    fvm::Field<2> pPrime(mesh, 0.0);

    pc.solve(uStar, uniformDCoeff(mesh, 1.0), pressure, pPrime, fvm::BCDiscretization{}, nullptr);

    bool finite = true;
    for (std::size_t i = 0; i < mesh.nCells(); ++i)
        if (!std::isfinite(pPrime[i])) finite = false;
    CHECK_MESSAGE(finite, "the AMG pressure correction must stay finite");

    const std::array<std::vector<double>, 2> dCoeff = uniformDCoeff(mesh, 1.0);
    std::vector<std::vector<char>> velMask(2, std::vector<char>(mesh.nCells(), 0));
    std::vector<std::vector<double>> velVals(2, std::vector<double>(mesh.nCells(), 0.0));
    std::array<fvm::Field<2>, 2> dpdx{fvm::Field<2>(mesh, 0.0), fvm::Field<2>(mesh, 0.0)};
    fvm::VectorField<2> corrected(mesh, 0.0);
    fvm::correctVelocity(mesh, uStar, pPrime, dCoeff, velMask, velVals, dpdx, corrected);
    fvm::Field<2> divAfterField(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, corrected, divAfterField, {}, {});
    double after = 0.0;
    for (std::size_t i = 0; i < mesh.nCells(); ++i)
        if (!mesh.isBoundary(i)) after = std::max(after, std::abs(divAfterField[i]));

    MESSAGE("AMG correction: divergence before = ", before, ", after = ", after);
    CHECK_MESSAGE(before > 0.5, "the predictor must actually be divergent");
    CHECK_MESSAGE(after < 0.1 * before, "the AMG correction must reduce the divergence");
}

} // TEST_SUITE("cfd")
