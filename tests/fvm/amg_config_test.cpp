// Regression checks for the AMG configuration surface exposed to the UI.
//
// Two things are easy to break silently and are covered here:
//   1. cfd::AmgSettings -> fvm::AMGHierarchy::Config translation. The UI edits
//      the plain mirror; a dropped or mismapped field would make a knob do
//      nothing, or do something else.
//   2. The hierarchy actually honouring every enum combination, including the
//      smoother a nonsymmetric operator needs.
//
// No Qt/GL: compiles the real AMG.cpp plus the header-only bridge.

#include "cfd/amg_settings_bridge.h"
#include "fvm/AMG.h"
#include "doctest/doctest.h"

#include <Eigen/Sparse>

#include <cmath>
#include <string>
#include <vector>

namespace {

#define CHECK_NEAR(got, want)                                                                      \
    CHECK_MESSAGE(std::fabs((got) - (want)) <= 1e-12 * (1.0 + std::fabs(want)),                   \
                  #got " ~= " #want " (got " << (got) << ", want " << (want) << ")")

// Standard 5-point Laplacian on an nx*ny grid: symmetric positive definite,
// which is exactly the operator AML/AMG is built for.
fvm::SparseMatrix poisson2D(int nx, int ny) {
    const int n = nx * ny;
    std::vector<Eigen::Triplet<double>> trips;
    trips.reserve(static_cast<std::size_t>(n) * 5);
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const int k = j * nx + i;
            trips.emplace_back(k, k, 4.0);
            if (i > 0)
                trips.emplace_back(k, k - 1, -1.0);
            if (i < nx - 1)
                trips.emplace_back(k, k + 1, -1.0);
            if (j > 0)
                trips.emplace_back(k, k - nx, -1.0);
            if (j < ny - 1)
                trips.emplace_back(k, k + nx, -1.0);
        }
    }
    fvm::SparseMatrix A(n, n);
    A.setFromTriplets(trips.begin(), trips.end());
    return A;
}

double relResidual(const fvm::SparseMatrix& A, const Eigen::VectorXd& b, const Eigen::VectorXd& x) {
    return (b - A * x).norm() / b.norm();
}

} // namespace

TEST_SUITE("fvm") {

// ---------------------------------------------------------------------------
// 1. cfd::AmgSettings <-> fvm::AMGHierarchy::Config mapping
// ---------------------------------------------------------------------------
TEST_CASE("fvm.amg.settings_translation") {
    // Translation copies the effective fields.

    // Deliberately non-default on every field, with values that cannot be
    // confused with one another, so a dropped copy is always caught.
    cfd::AmgSettings s;
    s.strengthThreshold = 0.375;
    s.truncThreshold = 0.125;
    s.maxLevels = 7;
    s.minCoarseSize = 23;
    s.preSweeps = 3;
    s.postSweeps = 4;
    s.maxIterations = 77;
    s.tolerance = 3e-11;
    s.stagnationFactor = 1.25;
    s.stagnationMinIter = 17;
    s.stagnationLooseFactor = 6.5;

    const fvm::AMGHierarchy::Config c = cfd::toFvmAmg(s);

    CHECK_NEAR(c.strengthThreshold, 0.375);
    CHECK_NEAR(c.truncThreshold, 0.125);
    CHECK_MESSAGE(c.maxLevels == 7, "maxLevels");
    CHECK_MESSAGE(c.minCoarseSize == 23, "minCoarseSize");
    CHECK_MESSAGE(c.preSweeps == 3, "preSweeps");
    CHECK_MESSAGE(c.postSweeps == 4, "postSweeps");
    CHECK_MESSAGE(c.maxIterations == 77, "maxIterations");
    CHECK_NEAR(c.tolerance, 3e-11);
    CHECK_NEAR(c.stagnationFactor, 1.25);
    CHECK_MESSAGE(c.stagnationMinIter == 17, "stagnationMinIter");
    CHECK_NEAR(c.stagnationLooseFactor, 6.5);

    // The presets must land on the values the solver shipped with, otherwise
    // Reset in the dialog would silently change solver behaviour.
    const fvm::AMGHierarchy::Config p = cfd::toFvmAmg(cfd::AmgSettings::pressureDefaults());
    const bool pressureSweeps = (p.preSweeps == 1) && (p.postSweeps == 1);
    CHECK_MESSAGE(pressureSweeps, "pressure preset sweeps");

    const fvm::AMGHierarchy::Config m = cfd::toFvmAmg(cfd::AmgSettings::momentumDefaults());
    const bool momentumSweeps = (m.preSweeps == 1) && (m.postSweeps == 1);
    CHECK_MESSAGE(momentumSweeps, "momentum preset sweeps");
}

// ---------------------------------------------------------------------------
// 2. Every enum combination builds and converges
// ---------------------------------------------------------------------------
TEST_CASE("fvm.amg.standalone_solve") {
    const fvm::SparseMatrix A = poisson2D(32, 32);
    const int n = static_cast<int>(A.rows());
    Eigen::VectorXd b = Eigen::VectorXd::Ones(n);
    Eigen::VectorXd x = Eigen::VectorXd::Zero(n);

    fvm::AMGHierarchy::Config cfg;
    cfg.tolerance = 1e-8;
    fvm::AMGHierarchy amg(cfg);
    amg.build(A);
    CHECK(amg.isBuilt());
    const bool ok = amg.solve(b, x);
    CHECK(ok);
    CHECK(x.allFinite());
    CHECK(relResidual(A, b, x) < 1e-5);
}

// ---------------------------------------------------------------------------
// 3. Preconditioner cycle count
// ---------------------------------------------------------------------------
TEST_CASE("fvm.amg.preconditioner_cycles") {
    const fvm::SparseMatrix A = poisson2D(24, 24);
    const int n = static_cast<int>(A.rows());
    Eigen::VectorXd b = Eigen::VectorXd::Ones(n);

    fvm::AMGHierarchy::Config cfg;
    fvm::AMGHierarchy amg(cfg);
    amg.build(A);

    fvm::AMGPreconditioner one;
    one.setAMG(&amg);
    one.setCycles(1);
    const double r1 = relResidual(A, b, one.solve(b));

    fvm::AMGPreconditioner two;
    two.setAMG(&amg);
    two.setCycles(2);
    const double r2 = relResidual(A, b, two.solve(b));

    CHECK_MESSAGE(r2 < r1, "two cycles beat one (r2=" << r2 << " r1=" << r1 << ")");

    // A cycle count below one must clamp rather than skip the preconditioner,
    // which would silently turn the Krylov solve into an unpreconditioned one.
    fvm::AMGPreconditioner zero;
    zero.setAMG(&amg);
    zero.setCycles(0);
    const double r0 = relResidual(A, b, zero.solve(b));
    CHECK_NEAR(r0, r1);
}

} // TEST_SUITE("fvm")
