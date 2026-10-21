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
    // The combo boxes in the UI store the cfd enum ordinal, and the switch in
    // toFvmAmg assumes the ordinals line up with the fvm enums. Pin that down.
    CHECK_MESSAGE(int(cfd::AmgSettings::CoarseningType::RS) ==
                      int(fvm::AMGHierarchy::CoarseningType::RS),
                  "CoarseningType::RS ordinal");
    CHECK_MESSAGE(int(cfd::AmgSettings::CoarseningType::PMIS) ==
                      int(fvm::AMGHierarchy::CoarseningType::PMIS),
                  "CoarseningType::PMIS ordinal");
    CHECK_MESSAGE(int(cfd::AmgSettings::InterpolationType::Direct) ==
                      int(fvm::AMGHierarchy::InterpolationType::Direct),
                  "InterpolationType::Direct ordinal");
    CHECK_MESSAGE(int(cfd::AmgSettings::InterpolationType::Extended) ==
                      int(fvm::AMGHierarchy::InterpolationType::Extended),
                  "InterpolationType::Extended ordinal");
    CHECK_MESSAGE(int(cfd::AmgSettings::CycleType::V) == int(fvm::AMGHierarchy::CycleType::V),
                  "CycleType::V ordinal");
    CHECK_MESSAGE(int(cfd::AmgSettings::CycleType::F) == int(fvm::AMGHierarchy::CycleType::F),
                  "CycleType::F ordinal");
    CHECK_MESSAGE(int(cfd::AmgSettings::SmootherType::SymmetricGaussSeidel) ==
                      int(fvm::AMGHierarchy::SmootherType::SymmetricGaussSeidel),
                  "SmootherType::SymmetricGaussSeidel ordinal");
    CHECK_MESSAGE(int(cfd::AmgSettings::SmootherType::ForwardGaussSeidel) ==
                      int(fvm::AMGHierarchy::SmootherType::ForwardGaussSeidel),
                  "SmootherType::ForwardGaussSeidel ordinal");
    CHECK_MESSAGE(int(cfd::AmgSettings::SmootherType::ForwardSOR) ==
                      int(fvm::AMGHierarchy::SmootherType::ForwardSOR),
                  "SmootherType::ForwardSOR ordinal");

    // Deliberately non-default on every field, with values that cannot be
    // confused with one another, so a dropped copy is always caught.
    cfd::AmgSettings s;
    s.coarseningType = cfd::AmgSettings::CoarseningType::PMIS;
    s.interpolationType = cfd::AmgSettings::InterpolationType::Extended;
    s.cycleType = cfd::AmgSettings::CycleType::F;
    s.smootherType = cfd::AmgSettings::SmootherType::ForwardSOR;
    s.smootherOmega = 0.875;
    s.strengthThreshold = 0.375;
    s.truncThreshold = 0.125;
    s.maxLevels = 7;
    s.minCoarseSize = 23;
    s.preSweeps = 3;
    s.postSweeps = 4;
    s.maxIterations = 77;
    s.tolerance = 3e-11;
    s.rngSeed = 1234;
    s.stagnationFactor = 1.25;
    s.stagnationMinIter = 17;
    s.stagnationLooseFactor = 6.5;
    s.galerkinDropTol = 5e-14;

    const fvm::AMGHierarchy::Config c = cfd::toFvmAmg(s);

    CHECK_MESSAGE(c.coarseningType == fvm::AMGHierarchy::CoarseningType::PMIS, "coarseningType");
    CHECK_MESSAGE(c.interpolationType == fvm::AMGHierarchy::InterpolationType::Extended,
                  "interpolationType");
    CHECK_MESSAGE(c.cycleType == fvm::AMGHierarchy::CycleType::F, "cycleType");
    CHECK_MESSAGE(c.smootherType == fvm::AMGHierarchy::SmootherType::ForwardSOR, "smootherType");
    CHECK_NEAR(c.smootherOmega, 0.875);
    CHECK_NEAR(c.strengthThreshold, 0.375);
    CHECK_NEAR(c.truncThreshold, 0.125);
    CHECK_MESSAGE(c.maxLevels == 7, "maxLevels");
    CHECK_MESSAGE(c.minCoarseSize == 23, "minCoarseSize");
    CHECK_MESSAGE(c.preSweeps == 3, "preSweeps");
    CHECK_MESSAGE(c.postSweeps == 4, "postSweeps");
    CHECK_MESSAGE(c.maxIterations == 77, "maxIterations");
    CHECK_NEAR(c.tolerance, 3e-11);
    CHECK_MESSAGE(c.rngSeed == 1234, "rngSeed");
    CHECK_NEAR(c.stagnationFactor, 1.25);
    CHECK_MESSAGE(c.stagnationMinIter == 17, "stagnationMinIter");
    CHECK_NEAR(c.stagnationLooseFactor, 6.5);
    CHECK_NEAR(c.galerkinDropTol, 5e-14);

    // The presets must land on the values the solver shipped with, otherwise
    // Reset in the dialog would silently change solver behaviour.
    const fvm::AMGHierarchy::Config p = cfd::toFvmAmg(cfd::AmgSettings::pressureDefaults());
    CHECK_MESSAGE(p.interpolationType == fvm::AMGHierarchy::InterpolationType::Direct,
                  "pressure preset interpolation");
    CHECK_MESSAGE(p.cycleType == fvm::AMGHierarchy::CycleType::V, "pressure preset cycle");
    CHECK_MESSAGE(p.smootherType == fvm::AMGHierarchy::SmootherType::SymmetricGaussSeidel,
                  "pressure preset smoother");
    const bool pressureSweeps = (p.preSweeps == 2) && (p.postSweeps == 2);
    CHECK_MESSAGE(pressureSweeps, "pressure preset sweeps");

    const fvm::AMGHierarchy::Config m = cfd::toFvmAmg(cfd::AmgSettings::momentumDefaults());
    CHECK_MESSAGE(m.interpolationType == fvm::AMGHierarchy::InterpolationType::Direct,
                  "momentum preset interpolation");
    CHECK_MESSAGE(m.cycleType == fvm::AMGHierarchy::CycleType::V, "momentum preset cycle");
    CHECK_MESSAGE(m.smootherType == fvm::AMGHierarchy::SmootherType::ForwardGaussSeidel,
                  "momentum preset smoother");
    const bool momentumSweeps = (m.preSweeps == 1) && (m.postSweeps == 1);
    CHECK_MESSAGE(momentumSweeps, "momentum preset sweeps");
}

// ---------------------------------------------------------------------------
// 2. Every enum combination builds and converges
// ---------------------------------------------------------------------------
TEST_CASE("fvm.amg.enum_combinations") {
    const fvm::SparseMatrix A = poisson2D(32, 32);
    const int n = static_cast<int>(A.rows());
    Eigen::VectorXd b = Eigen::VectorXd::Ones(n);
    Eigen::VectorXd x0 = Eigen::VectorXd::Zero(n);

    const char* const coarseNames[] = {"RS", "PMIS"};
    const char* const interpNames[] = {"Direct", "Extended"};
    const char* const cycleNames[] = {"V", "F"};
    const char* const smoothNames[] = {"SymmetricGS", "ForwardGS", "ForwardSOR"};

    int cases = 0;
    for (int co = 0; co < 2; ++co) {
        for (int in = 0; in < 2; ++in) {
            for (int cy = 0; cy < 2; ++cy) {
                for (int sm = 0; sm < 3; ++sm) {
                    const std::string label = std::string("coarse=") + coarseNames[co] +
                                              " interp=" + interpNames[in] + " cycle=" +
                                              cycleNames[cy] + " smoother=" + smoothNames[sm];
                    CAPTURE(label);

                    fvm::AMGHierarchy::Config cfg;
                    cfg.coarseningType = static_cast<fvm::AMGHierarchy::CoarseningType>(co);
                    cfg.interpolationType = static_cast<fvm::AMGHierarchy::InterpolationType>(in);
                    cfg.cycleType = static_cast<fvm::AMGHierarchy::CycleType>(cy);
                    cfg.smootherType = static_cast<fvm::AMGHierarchy::SmootherType>(sm);
                    // Below 1 for SOR: an over-relaxed sweep is not guaranteed
                    // to converge on a nonsymmetric operator.
                    cfg.smootherOmega = (sm == 2) ? 0.9 : 1.0;
                    cfg.tolerance = 1e-8;

                    fvm::AMGHierarchy amg(cfg);
                    amg.build(A);
                    Eigen::VectorXd x = x0;
                    const bool ok = amg.solve(b, x);

                    CHECK_MESSAGE(amg.isBuilt(), "hierarchy built");
                    CHECK_MESSAGE(x.allFinite(), "solution is finite");
                    const double r = x.allFinite() ? relResidual(A, b, x) : 1.0;
                    CAPTURE(r);
                    CHECK_MESSAGE(ok, "solver reported convergence");
                    CHECK_MESSAGE(r < 1e-5, "relative residual below 1e-5");
                    ++cases;
                }
            }
        }
    }
    CHECK_MESSAGE(cases == 24, "exercised all 24 configurations, got " << cases);
}

// ---------------------------------------------------------------------------
// 3. Preconditioner cycle count
// ---------------------------------------------------------------------------
TEST_CASE("fvm.amg.preconditioner_cycles") {
    const fvm::SparseMatrix A = poisson2D(24, 24);
    const int n = static_cast<int>(A.rows());
    Eigen::VectorXd b = Eigen::VectorXd::Ones(n);

    fvm::AMGHierarchy::Config cfg;
    cfg.coarseningType = fvm::AMGHierarchy::CoarseningType::RS;
    cfg.interpolationType = fvm::AMGHierarchy::InterpolationType::Direct;
    cfg.smootherType = fvm::AMGHierarchy::SmootherType::SymmetricGaussSeidel;
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
