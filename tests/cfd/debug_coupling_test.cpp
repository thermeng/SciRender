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

// TEMPORARY probe: localize the composed-matrix deviation on the x^2 field.
#include "fvm/Gradient.h"
TEST_CASE("cfd.temp.matrix_probe") {
    const fvm::Mesh<2> mesh = uniform2D(8);
    const std::size_t n = mesh.nCells();

    fvm::VectorField<2> uStarZero(mesh, 0.0);
    fvm::Field<2> pressure(mesh, 0.0);
    fvm::Field<2> pPrime(mesh, 0.0);
    std::vector<char> isPressureNeumann(n, 1);
    fvm::PressureCorrection<2> pc(mesh);
    std::vector<std::vector<double>> pVecPerDim;
    (void)pVecPerDim;
    pc.solve(uStarZero, {{std::vector<double>(n, 1.0), std::vector<double>(n, 1.0)}}, pressure, pPrime,
             {}, isPressureNeumann);

    // p = x^2 through the assembled matrix.
    std::vector<double> pVec(n, 0.0);
    for (std::size_t i = 0; i < n; ++i)
        pVec[i] = mesh.cellCenter(i)[0] * mesh.cellCenter(i)[0];
    const Eigen::VectorXd Ap = pc.lastMatrix() * Eigen::Map<const Eigen::VectorXd>(pVec.data(), static_cast<Eigen::Index>(n));

    // The pin and the "deep" set (2 rings from walls and masks).
    std::size_t pinned = n;
    for (std::size_t i = 0; i < n; ++i)
        if (pc.pressureMask()[i]) { pinned = i; break; }
    printf("pinned cell = %zu (i=%zu, j=%zu)\n", pinned, mesh.cellCoords(pinned)[0], mesh.cellCoords(pinned)[1]);

    // LS gradient divergence of the same p field, per cell.
    fvm::Gradient<2> gradient(mesh, fvm::GradientScheme::LeastSquares);
    std::array<fvm::Field<2>, 2> lsq{fvm::Field<2>(mesh, 0.0), fvm::Field<2>(mesh, 0.0)};
    fvm::Field<2> pField(mesh, 0.0);
    for (std::size_t i = 0; i < n; ++i) pField[i] = mesh.cellCenter(i)[0] * mesh.cellCenter(i)[0];
    gradient.compute(pField, lsq);
    fvm::VectorField<2> gradField(mesh, 0.0);
    for (std::size_t d = 0; d < 2; ++d)
        for (std::size_t i = 0; i < n; ++i) gradField[d][i] = lsq[d][i];
    fvm::Field<2> divOfGrad(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, gradField, divOfGrad, {}, {});

    printf("map of (Ap/V - 2)  [j=0..7 rows, i=0..7 cols]:\n");
    for (std::size_t j = 0; j < 8; ++j) {
        printf("  ");
        for (std::size_t i = 0; i < 8; ++i) {
            const std::size_t c = mesh.cellIndex(i, 7 - j);
            printf("%+8.4f", Ap(static_cast<Eigen::Index>(c)) / mesh.cellVolume(c) - 2.0);
        }
        printf("\n");
    }
    printf("map of div(LS grad p) - 2:\n");
    for (std::size_t j = 0; j < 8; ++j) {
        printf("  ");
        for (std::size_t i = 0; i < 8; ++i) {
            const std::size_t c = mesh.cellIndex(i, 7 - j);
            printf("%+8.4f", divOfGrad[c] - 2.0);
        }
        printf("\n");
    }

    // The matrix row of the worst deep cell, printed column by column.
    std::size_t worst = n;
    double worstVal = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (mesh.isBoundary(i)) continue;
        const double v = std::abs(Ap(static_cast<Eigen::Index>(i)) / mesh.cellVolume(i) - 2.0);
        if (v > worstVal) { worstVal = v; worst = i; }
    }
    printf("pinned/masked cells:");
    for (std::size_t i = 0; i < n; ++i)
        if (pc.pressureMask()[i])
            printf(" %zu(i%zu,j%zu)", i, mesh.cellCoords(i)[0], mesh.cellCoords(i)[1]);
    printf("\n");
    if (worst < n) {
        printf("worst cell %zu (i=%zu, j=%zu): Ap/V = %+.6f\n", worst, mesh.cellCoords(worst)[0],
               mesh.cellCoords(worst)[1], Ap(static_cast<Eigen::Index>(worst)) / mesh.cellVolume(worst));
        const auto& A = pc.lastMatrix();
        printf("  row entries (col: (i,j) value):\n");
        for (Eigen::Index c = 0; c < A.outerSize(); ++c) {
            for (Eigen::SparseMatrix<double>::InnerIterator it(A, c); it; ++it) {
                if (it.row() == static_cast<Eigen::Index>(worst)) {
                    const std::size_t cc = static_cast<std::size_t>(c);
                    printf("   col %zu (i=%zu, j=%zu): %+0.6f\n", cc, mesh.cellCoords(cc)[0],
                           mesh.cellCoords(cc)[1], it.value());
                }
            }
        }
        // And what div(LS grad) looks like at the worst cell:
        printf("  div(LS grad) there = %+.6f\n", divOfGrad[worst]);
    }

    // Row dumps for the last column, first column and the pin ring.
    auto dumpRow = [&](std::size_t r) {
        if (r >= n) return;
        printf("row %zu (i=%zu, j=%zu): Ap/V = %+.5f, div(LSgrad) = %+.5f, masked = %d\n", r,
               mesh.cellCoords(r)[0], mesh.cellCoords(r)[1],
               Ap(static_cast<Eigen::Index>(r)) / mesh.cellVolume(r), divOfGrad[r],
               (int)pc.pressureMask()[r]);
        const auto& A = pc.lastMatrix();
        for (Eigen::Index c = 0; c < A.outerSize(); ++c) {
            for (Eigen::SparseMatrix<double>::InnerIterator it(A, c); it; ++it) {
                if (it.row() == static_cast<Eigen::Index>(r)) {
                    const std::size_t cc = static_cast<std::size_t>(c);
                    printf("   col %zu (i=%zu, j=%zu): %+0.6f\n", cc, mesh.cellCoords(cc)[0],
                           mesh.cellCoords(cc)[1], it.value());
                }
            }
        }
    };
    dumpRow(mesh.cellIndex(7, 3));
    dumpRow(mesh.cellIndex(2, 3));
    dumpRow(mesh.cellIndex(3, 2));
}

} // TEST_SUITE("cfd")
