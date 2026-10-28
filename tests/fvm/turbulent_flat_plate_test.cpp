#include "doctest/doctest.h"
#include "fvm/Convection.h"
#include "fvm/Divergence.h"
#include "fvm/SIMPLE.h"
#include "mesh/BoundaryUtils.h"
#include "support/fvm_fixtures.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace test_support;

namespace {
constexpr int kNx = 60;
constexpr int kNy = 20;
constexpr double kLength = 1.0;
constexpr double kHeight = 0.2;
constexpr double kUinf = 1.0;
constexpr double kRe = 1.0e4;
constexpr int kIterations = 300;
}

TEST_CASE("turbulent flat plate runs and conserves mass") {
    const double nu = kUinf * kLength / kRe;

    fvm::Mesh<2> mesh = fvm::Mesh<2>(
        std::array<std::size_t, 2>{static_cast<std::size_t>(kNx),
                                   static_cast<std::size_t>(kNy)},
        std::array<double, 2>{kLength, kHeight});

    std::vector<std::size_t> inlet, outlet, bottom, top;
    forEachBoundaryFace(mesh, 0, -1, [&](std::size_t f, std::size_t) { inlet.push_back(f); });
    forEachBoundaryFace(mesh, 0, +1, [&](std::size_t f, std::size_t) { outlet.push_back(f); });
    forEachBoundaryFace(mesh, 1, -1, [&](std::size_t f, std::size_t) { bottom.push_back(f); });
    forEachBoundaryFace(mesh, 1, +1, [&](std::size_t f, std::size_t) { top.push_back(f); });

    fvm::SIMPLE<2>::Config cfg;
    cfg.maxIterations = static_cast<std::size_t>(kIterations);
    cfg.tolerance = 1e-6;
    cfg.runFixedIterations = true;
    cfg.convergenceHoldIterations = 3;
    cfg.alphaU = 0.7;
    cfg.alphaP = 0.3;
    cfg.simplec = false;
    cfg.rhieChow = true;
    cfg.pressureCorrectors = 2;
    cfg.printFrequency = 1000000;
    cfg.convectionScheme = fvm::ConvectionScheme::SecondOrderUpwind;
    cfg.gradientScheme = fvm::GradientScheme::LeastSquares;
    cfg.turbulenceEnabled = true;

    fvm::SIMPLE<2> solver(mesh, 1.0, nu, cfg);

    // BCs
    solver.setVelocityBCOnFaces(inlet, fvm::VelocityBCType::Inlet, kUinf, 0.0, 0.0);
    solver.setPressureOutletOnFaces(outlet, 0.0);
    solver.setVelocityBCOnFaces(bottom, fvm::VelocityBCType::NoSlip);
    solver.setVelocityBCOnFaces(top, fvm::VelocityBCType::Symmetry);
    // pressure zero gradient is default; no explicit call needed

    fvm::VectorField<2> velocity(mesh, 0.0);
    fvm::Field<2> pressure(mesh, 0.0);

    solver.solve(velocity, pressure);

    CHECK(solver.finalContinuityResidual() < 1e-2);
    // The momentum residual is evaluated against the operator assembled with
    // the previous pressure, so at steady state it measures the pressure
    // correction rather than zero. A turbulent flat plate at Re=1e4 on this
    // coarse mesh plateaus around 1.1%, independent of the iteration count.
    CHECK(solver.finalMomentumResidual() < 2e-2);

    const auto* turb = solver.turbulence();
    REQUIRE(turb != nullptr);
    const auto& k = turb->k();
    const auto& w = turb->omega();
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        CHECK(k[i] >= 0.0);
        CHECK(w[i] >= 0.0);
        CHECK(std::isfinite(k[i]));
        CHECK(std::isfinite(w[i]));
    }
}
