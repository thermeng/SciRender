#include "cfd/cfd_mesh_adapter.h"
#include "fvm/SIMPLE.h"
#include "mesh/BoundaryUtils.h"
#include "support/test_support.h"
#include <doctest/doctest.h>
#include <cmath>
#include <iostream>

TEST_CASE("flatplate mesh turbulent run") {
    test_support::requireSample("cfd/FlatPlate.msh");
    auto res = cfd::loadMesh(test_support::samplePath("cfd/FlatPlate.msh").string());
    REQUIRE(res.error.empty());
    REQUIRE(res.mesh2);
    const auto& mesh = *res.mesh2;

    // boundary faces by patch
    std::vector<std::size_t> inlet, outlet, wall, top;
    const auto& groups = mesh.boundaryGroups();
    auto get = [&](const char* name, std::vector<std::size_t>& v) {
        auto it = groups.find(name);
        if (it != groups.end()) v = it->second;
    };
    get("inlet", inlet);
    get("outlet", outlet);
    get("bottomedge", wall);
    get("freestream", top);

    std::cout << "inlet=" << inlet.size() << " outlet=" << outlet.size() << " wall=" << wall.size() << " top=" << top.size() << "\n";

    double L = 1.0, U = 30.0;
    double nu = 1.81e-5 / 1.225;
    double rho = 1.225;

    fvm::SIMPLE<2>::Config cfg;
    cfg.maxIterations = 100;
    cfg.tolerance = 1e-4;
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
    cfg.kInf = 1e-3;
    cfg.omegaInf = 1.0;
    cfg.alphaK = 0.7;
    cfg.alphaOmega = 0.7;

    fvm::SIMPLE<2> solver(mesh, rho, nu, cfg);

    solver.setVelocityBCOnFaces(inlet, fvm::VelocityBCType::Inlet, U, 0.0, 0.0);
    solver.setPressureOutletOnFaces(outlet, 0.0);
    solver.setVelocityBCOnFaces(wall, fvm::VelocityBCType::NoSlip);
    solver.setVelocityBCOnFaces(top, fvm::VelocityBCType::Symmetry);

    fvm::VectorField<2> velocity(mesh, 0.0);
    fvm::Field<2> pressure(mesh, 0.0);
    velocity.assign(0, U);

    solver.solve(velocity, pressure);

    std::cout << "continuity=" << solver.finalContinuityResidual() << " momentum=" << solver.finalMomentumResidual() << "\n";
    if (solver.turbulence()) {
        std::cout << "k resid=" << solver.turbulence()->finalKResid() << " omega resid=" << solver.turbulence()->finalOmegaResid() << "\n";
        std::cout << "maxYPlus=" << solver.turbulence()->maxYPlus() << "\n";
    }
    CHECK(std::isfinite(solver.finalContinuityResidual()));
}
