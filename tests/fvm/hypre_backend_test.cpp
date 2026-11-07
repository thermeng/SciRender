// Hypre BoomerAMG backend integration test.
//
// Covers the optional Hypre backend for fvm::AMGHierarchy:
//   - Config::useHypre defaults to false (amgcl stays the default backend).
//   - With useHypre=true, the hierarchy builds and solves a Poisson system.
//   - When Hypre is not linked in (FVM_HAVE_HYPRE undefined), the same call
//     must still succeed through the amgcl fallback (never a hard failure).
//
// No Qt/GL: compiles the real AMG.cpp.

#include "fvm/AMG.h"
#include "doctest/doctest.h"

#include <Eigen/Sparse>

#include <cmath>
#include <vector>

namespace {

// Standard 5-point Laplacian on an nx*ny grid (SPD).
fvm::SparseMatrix poisson2D(int nx, int ny) {
    const int n = nx * ny;
    std::vector<Eigen::Triplet<double>> trips;
    trips.reserve(static_cast<std::size_t>(n) * 5);
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const int k = j * nx + i;
            trips.emplace_back(k, k, 4.0);
            if (i > 0) trips.emplace_back(k, k - 1, -1.0);
            if (i < nx - 1) trips.emplace_back(k, k + 1, -1.0);
            if (j > 0) trips.emplace_back(k, k - nx, -1.0);
            if (j < ny - 1) trips.emplace_back(k, k + nx, -1.0);
        }
    }
    fvm::SparseMatrix A(n, n);
    A.setFromTriplets(trips.begin(), trips.end());
    return A;
}

double relResidual(const fvm::SparseMatrix& A, const Eigen::VectorXd& b,
                   const Eigen::VectorXd& x) {
    return (b - A * x).norm() / b.norm();
}

} // namespace

TEST_SUITE("fvm") {

TEST_CASE("fvm.amg.hypre.config_defaults_amgcl") {
    // Default must stay amgcl so existing behaviour is untouched until the
    // user opts in.
    fvm::AMGHierarchy::Config cfg;
    cfg.tolerance = 1e-8;
    CHECK(cfg.useHypre == false);

    // The public deployment must not silently require Hypre: with the default
    // config the standard solve still works.
    const fvm::SparseMatrix A = poisson2D(16, 16);
    const int n = static_cast<int>(A.rows());
    Eigen::VectorXd b = Eigen::VectorXd::Ones(n);
    Eigen::VectorXd x = Eigen::VectorXd::Zero(n);
    fvm::AMGHierarchy amg(cfg);
    amg.build(A);
    CHECK(amg.isBuilt());
    CHECK(amg.solve(b, x));
    CHECK(relResidual(A, b, x) < 1e-5);
}

TEST_CASE("fvm.amg.hypre.poisson_solve") {
    const fvm::SparseMatrix A = poisson2D(32, 32);
    const int n = static_cast<int>(A.rows());
    Eigen::VectorXd b = Eigen::VectorXd::Ones(n);
    Eigen::VectorXd x = Eigen::VectorXd::Zero(n);

    fvm::AMGHierarchy::Config cfg;
    cfg.useHypre = true;
    cfg.tolerance = 1e-8;

#ifdef FVM_HAVE_HYPRE
    fvm::AMGHierarchy amg(cfg);
    amg.build(A);
    CHECK_MESSAGE(amg.isBuilt(), "Hypre BoomerAMG hierarchy must build");
    const bool ok = amg.solve(b, x);
    CHECK_MESSAGE(ok, "Hypre solve must succeed");
    CHECK(x.allFinite());
    CHECK_MESSAGE(relResidual(A, b, x) < 1e-5,
                  "Hypre Poisson rel residual " << relResidual(A, b, x));
#else
    // Hypre not present in this build: useHypre must fall back to amgcl
    // rather than failing the build or the solve.
    MESSAGE("Hypre not found at configure time; exercising amgcl fallback for useHypre=true");
    fvm::AMGHierarchy amg(cfg);
    amg.build(A);
    CHECK(amg.isBuilt());
    const bool ok = amg.solve(b, x);
    CHECK(ok);
    CHECK(x.allFinite());
    CHECK(relResidual(A, b, x) < 1e-5);
#endif
}

TEST_CASE("fvm.amg.hypre.flexible_cycle") {
    const fvm::SparseMatrix A = poisson2D(20, 20);
    const int n = static_cast<int>(A.rows());
    Eigen::VectorXd b = Eigen::VectorXd::Ones(n);
    Eigen::VectorXd x = Eigen::VectorXd::Zero(n);

    fvm::AMGHierarchy::Config cfg;
    cfg.useHypre = true;
    cfg.tolerance = 1e-8;
    cfg.cycleType = fvm::AMGHierarchy::Config::CycleType::Flexible;
    cfg.flexAlpha = 0.5; // prolongation tolerance
    cfg.flexBeta = 0.9;  // restriction tolerance
    cfg.flexMaxSweeps = 1;

    fvm::AMGHierarchy amg(cfg);
    amg.build(A);
    CHECK(amg.isBuilt());
    const bool ok = amg.solve(b, x);
    CHECK(ok);
    CHECK(x.allFinite());
    CHECK(relResidual(A, b, x) < 1e-5);
}

} // TEST_SUITE("fvm")
