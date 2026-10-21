// A diagnostic harness for the pressure-velocity coupling.
//
// It prints, for a small case, the residual divergence per cell, the pressure
// correction, and - for a few representative cells - the contribution of every
// face to the divergence of the predictor and of the corrected velocity. That
// is how the coupling defect was localised: the correction removes the
// divergence in total but not face by face, because the pressure matrix and the
// gradient the correction applies are different operators, and they disagree
// wherever the pressure correction has a local feature (the reference cell's
// dip, or a kink in the source).
#include "doctest/doctest.h"
#include "fvm/Divergence.h"
#include "fvm/Gradient.h"
#include "fvm/PressureCorrection.h"
#include "fvm/VelocityCorrection.h"
#include "support/fvm_fixtures.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace test_support;

TEST_SUITE("cfd") {

TEST_CASE("cfd.pressure.coupling_diagnosis") {
    const fvm::Mesh<2> mesh = uniform2D(8);
    const std::size_t n = mesh.nCells();

    fvm::VectorField<2> uStar(mesh, 0.0);
    fillVector(uStar, [](double x, double, double) {
        const double u = (x < 0.5) ? x : (1.0 - x);
        return std::array<double, 2>{u, 0.0};
    });

    fvm::Field<2> pressure(mesh, 0.0);
    fvm::Field<2> pPrime(mesh, 0.0);
    std::vector<char> isPressureNeumann(n, 1);
    fvm::PressureCorrection<2> pc(mesh);
    pc.solve(uStar, {{std::vector<double>(n, 1.0), std::vector<double>(n, 1.0)}}, pressure, pPrime,
             {}, isPressureNeumann);

    // The correction the solver applies (the same call the solver makes).
    std::array<std::vector<double>, 2> dCoeff{std::vector<double>(n, 1.0),
                                             std::vector<double>(n, 1.0)};
    std::vector<std::vector<char>> velMask(2, std::vector<char>(n, 0));
    std::vector<std::vector<double>> velVals(2, std::vector<double>(n, 0.0));
    std::array<fvm::Field<2>, 2> dpdx{fvm::Field<2>(mesh, 0.0), fvm::Field<2>(mesh, 0.0)};
    fvm::VectorField<2> corrected(mesh, 0.0);
    fvm::correctVelocity(mesh, uStar, pPrime, dCoeff, velMask, velVals, dpdx, corrected);

    // The residual divergence per cell (as the solver measures it).
    fvm::Field<2> divBefore(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, uStar, divBefore, {}, {});
    fvm::Field<2> divAfter(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, corrected, divAfter, {}, {});

    // An 8x8 map of the residual.
    printf("\nresidual divergence (max |.| = %g):\n", [&] {
        double m = 0;
        for (std::size_t i = 0; i < n; ++i) m = std::max(m, std::abs(divAfter[i]));
        return m;
    }());
    for (std::size_t j = 0; j < 8; ++j) {
        printf("  ");
        for (std::size_t i = 0; i < 8; ++i) {
            const std::size_t c = mesh.cellIndex(i, 7 - j);
            printf("%+7.3f", divAfter[c]);
        }
        printf("\n");
    }

    // The pressure correction and the reference cell.
    printf("\np' (row by row, top row first):\n");
    for (std::size_t j = 0; j < 8; ++j) {
        printf("  ");
        for (std::size_t i = 0; i < 8; ++i)
            printf("%+7.3f", pPrime[mesh.cellIndex(i, 7 - j)]);
        printf("\n");
    }
    printf("pinned cells:");
    for (std::size_t i = 0; i < n; ++i)
        if (pc.pressureMask()[i])
            printf(" %zu", i);
    printf("\n");

    // The face-by-face decomposition of the cancellation.
    auto decompose = [&](std::size_t P) {
        printf("\ncell %zu (i=%zu, j=%zu): div(u*) = %+.4f, div(corrected) = %+.4f", P,
               mesh.cellCoords(P)[0], mesh.cellCoords(P)[1], divBefore[P], divAfter[P]);
        // What the pressure equation asked for at this cell.
        const double wanted = -pc.lastRhs()[static_cast<int>(P)] / mesh.cellVolume(P);
        printf(", (A p')/V = %+.4f  (the correction should deliver -that)\n", wanted);
        for (std::size_t f : mesh.cellFaces(P)) {
            const std::size_t N = mesh.neighbor(f);
            const bool boundary = (N == std::size_t(-1));
            const double w = boundary ? 0.0 : mesh.faceWeight(f);
            const double area = mesh.faceArea(f);
            const double* nrm = mesh.faceNormalPtr(f);
            // The face value is interpolated between the face's OWNER and its
            // neighbour - the weight is measured from the owner.
            const std::size_t O = mesh.owner(f);
            auto faceValue = [&](const fvm::VectorField<2>& field) {
                const double u0 = field[0][O];
                const double u1 = boundary ? u0 : field[0][N];
                const double v0 = field[1][O];
                const double v1 = boundary ? v0 : field[1][N];
                return std::array<double, 2>{(1.0 - w) * u0 + w * u1, (1.0 - w) * v0 + w * v1};
            };
            const auto uf = faceValue(uStar);
            const auto cf = faceValue(corrected);
            // The divergence operator adds the flux at the face's owner and
            // SUBTRACTS it at the neighbour.
            const double sign = (O == P) ? 1.0 : -1.0;
            const double fluxU = sign * (uf[0] * nrm[0] + uf[1] * nrm[1]) * area / mesh.cellVolume(P);
            const double fluxC = sign * (cf[0] * nrm[0] + cf[1] * nrm[1]) * area / mesh.cellVolume(P);
            printf("   face %3zu %s N=%3zu: div(u*) %+9.5f  div(corrected) %+9.5f  residual %+9.5f\n", f,
                   boundary ? "BOUNDARY" : "        ", boundary ? 999 : N, fluxU, fluxC,
                   fluxU - fluxC);
        }
    };

    // A deep interior cell, a cell next to a wall and a cell next to the
    // reference cell.
    std::size_t pinned = n;
    for (std::size_t i = 0; i < n; ++i)
        if (pc.pressureMask()[i]) {
            pinned = i;
            break;
        }
    decompose(mesh.cellIndex(3, 3));
    decompose(mesh.cellIndex(0, 3));
    if (pinned < n) {
        for (std::size_t nb : mesh.cellNeighbors(pinned))
            decompose(nb);
    }
}

} // TEST_SUITE("cfd")
