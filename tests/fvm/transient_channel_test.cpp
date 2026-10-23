// Transient open channel: a velocity inlet on the left, a pressure outlet on the
// right, no-slip walls. Exercises the transient (PISO-style) path of SIMPLE:
// the solver must march to tEnd without producing non-finite values and keep
// the mass balance bounded.
#include "doctest/doctest.h"
#include "fvm/Divergence.h"
#include "fvm/SIMPLE.h"
#include "mesh/BoundaryUtils.h"
#include "support/fvm_fixtures.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace test_support;

namespace {

constexpr int kNx = 20;
constexpr int kNy = 6;
constexpr double kLength = 4.0;
constexpr double kHeight = 1.0;
constexpr double kInletSpeed = 1.0;
constexpr double kReynolds = 100.0;

} // namespace

TEST_SUITE("fvm") {

TEST_CASE("fvm.transient.open_channel_runs") {
    const double nu = kInletSpeed * kHeight / kReynolds;

    fvm::Mesh<2> mesh = fvm::Mesh<2>(std::array<std::size_t, 2>{static_cast<std::size_t>(kNx),
                                                                  static_cast<std::size_t>(kNy)},
                                      std::array<double, 2>{kLength, kHeight});

    std::vector<std::size_t> inlet, outlet, walls;
    forEachBoundaryFace(mesh, 0, -1, [&](std::size_t f, std::size_t) { inlet.push_back(f); });
    forEachBoundaryFace(mesh, 0, +1, [&](std::size_t f, std::size_t) { outlet.push_back(f); });
    forEachBoundaryFace(mesh, 1, -1, [&](std::size_t f, std::size_t) { walls.push_back(f); });
    forEachBoundaryFace(mesh, 1, +1, [&](std::size_t f, std::size_t) { walls.push_back(f); });

    fvm::SIMPLE<2>::Config cfg;
    cfg.maxIterations = 5;
    cfg.tolerance = 1e-3;
    cfg.runFixedIterations = false;
    cfg.convergenceHoldIterations = 1;
    cfg.alphaU = 0.7;
    cfg.alphaP = 0.3;
    cfg.simplec = false;
    cfg.rhieChow = true;
    cfg.pressureCorrectors = 2;
    cfg.printFrequency = 1000000;
    cfg.convectionScheme = fvm::ConvectionScheme::Upwind;
    cfg.gradientScheme = fvm::GradientScheme::LeastSquares;
    cfg.transient.enabled = true;
    cfg.transient.dt = 0.01;
    cfg.transient.tEnd = 0.05;
    cfg.transient.scheme = fvm::TimeScheme::Euler;
    cfg.transient.adaptive = false;
    cfg.transient.maxOuterCorrectors = 2;
    cfg.solverType = fvm::SolverType::PISO;

    fvm::SIMPLE<2> solver(mesh, 1.0, nu, cfg);

    solver.setVelocityBCOnFacesWithSlip(inlet, "inlet", kInletSpeed, 0.0, 0.0, false, 0.0);
    solver.setVelocityBCOnFacesWithSlip(outlet, "pressure_outlet", 0.0, 0.0, 0.0, false, 0.0);
    solver.setVelocityBCOnFacesWithSlip(walls, "wall", 0.0, 0.0, 0.0, false, 0.0);

    fvm::VectorField<2> velocity(mesh, 0.0);
    fvm::Field<2> pressure(mesh, 0.0);
    solver.solveTransient(velocity, pressure);

    double maxU = 0.0;
    for (std::size_t c = 0; c < mesh.nCells(); ++c)
        maxU = std::max(maxU, std::abs(velocity[0][c]));
    CHECK_MESSAGE(std::isfinite(maxU), "velocity must stay finite");

    fvm::Field<2> div(mesh, 0.0);
    fvm::computeDivergenceInto(mesh, velocity, div, {}, {});
    double maxDiv = 0.0;
    for (std::size_t c = 0; c < mesh.nCells(); ++c)
        if (!mesh.isBoundary(c)) maxDiv = std::max(maxDiv, std::abs(div[c]));
    CHECK_MESSAGE(maxDiv < 1.0, "divergence must stay bounded");
}

TEST_CASE("fvm.transient.open_channel_reaches_steady") {
    const double nu = kInletSpeed * kHeight / kReynolds;

    auto buildMesh = [&]() {
        return fvm::Mesh<2>(std::array<std::size_t, 2>{static_cast<std::size_t>(kNx),
                                                        static_cast<std::size_t>(kNy)},
                            std::array<double, 2>{kLength, kHeight});
    };

    auto setupBCs = [&](fvm::SIMPLE<2>& solver, fvm::Mesh<2>& mesh,
                        std::vector<std::size_t>& inlet, std::vector<std::size_t>& outlet,
                        std::vector<std::size_t>& walls) {
        forEachBoundaryFace(mesh, 0, -1, [&](std::size_t f, std::size_t) { inlet.push_back(f); });
        forEachBoundaryFace(mesh, 0, +1, [&](std::size_t f, std::size_t) { outlet.push_back(f); });
        forEachBoundaryFace(mesh, 1, -1, [&](std::size_t f, std::size_t) { walls.push_back(f); });
        forEachBoundaryFace(mesh, 1, +1, [&](std::size_t f, std::size_t) { walls.push_back(f); });
        solver.setVelocityBCOnFacesWithSlip(inlet, "inlet", kInletSpeed, 0.0, 0.0, false, 0.0);
        solver.setVelocityBCOnFacesWithSlip(outlet, "pressure_outlet", 0.0, 0.0, 0.0, false, 0.0);
        solver.setVelocityBCOnFacesWithSlip(walls, "wall", 0.0, 0.0, 0.0, false, 0.0);
    };

    // Steady reference.
    fvm::Mesh<2> meshS = buildMesh();
    std::vector<std::size_t> inS, outS, wallS;
    fvm::SIMPLE<2>::Config cfgS;
    cfgS.maxIterations = 300;
    cfgS.tolerance = 1e-5;
    cfgS.runFixedIterations = false;
    cfgS.convergenceHoldIterations = 3;
    cfgS.alphaU = 0.7;
    cfgS.alphaP = 0.3;
    cfgS.rhieChow = true;
    cfgS.pressureCorrectors = 2;
    cfgS.printFrequency = 1000000;
    cfgS.convectionScheme = fvm::ConvectionScheme::Upwind;
    cfgS.gradientScheme = fvm::GradientScheme::LeastSquares;
    fvm::SIMPLE<2> solverS(meshS, 1.0, nu, cfgS);
    setupBCs(solverS, meshS, inS, outS, wallS);
    fvm::VectorField<2> velS(meshS, 0.0);
    fvm::Field<2> presS(meshS, 0.0);
    solverS.solve(velS, presS);

    // Transient run from rest.
    fvm::Mesh<2> meshT = buildMesh();
    std::vector<std::size_t> inT, outT, wallT;
    fvm::SIMPLE<2>::Config cfgT;
    cfgT.maxIterations = 5;
    cfgT.tolerance = 1e-3;
    cfgT.runFixedIterations = false;
    cfgT.convergenceHoldIterations = 1;
    cfgT.alphaU = 0.7;
    cfgT.alphaP = 0.3;
    cfgT.rhieChow = true;
    cfgT.pressureCorrectors = 2;
    cfgT.printFrequency = 1000000;
    cfgT.convectionScheme = fvm::ConvectionScheme::Upwind;
    cfgT.gradientScheme = fvm::GradientScheme::LeastSquares;
    cfgT.transient.enabled = true;
    cfgT.transient.dt = 0.01;
    cfgT.transient.tEnd = 2.0;
    cfgT.transient.scheme = fvm::TimeScheme::BDF2;
    cfgT.transient.adaptive = false;
    cfgT.transient.maxOuterCorrectors = 2;
    cfgT.solverType = fvm::SolverType::PISO;
    fvm::SIMPLE<2> solverT(meshT, 1.0, nu, cfgT);
    setupBCs(solverT, meshT, inT, outT, wallT);
    fvm::VectorField<2> velT(meshT, 0.0);
    fvm::Field<2> presT(meshT, 0.0);
    solverT.solveTransient(velT, presT);

    double maxDiff = 0.0;
    for (std::size_t c = 0; c < meshT.nCells(); ++c)
        maxDiff = std::max(maxDiff, std::abs(velT[0][c] - velS[0][c]));
    CHECK_MESSAGE(maxDiff < 0.1, "transient run must approach the steady solution");
}

} // TEST_SUITE("fvm")
