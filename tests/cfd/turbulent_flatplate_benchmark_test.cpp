#include "cfd/cfd_mesh_adapter.h"
#include "fvm/SIMPLE.h"
#include "mesh/BoundaryUtils.h"
#include "mesh/Mesh.h"
#include "support/test_support.h"
#include <doctest/doctest.h>
#include <cmath>
#include <iostream>
#include <vector>

TEST_CASE("turbulent flat plate benchmark") {
    test_support::requireSample("cfd/FlatPlate.msh");
    auto res = cfd::loadMesh(test_support::samplePath("cfd/FlatPlate.msh").string());
    REQUIRE(res.error.empty());
    REQUIRE(res.mesh2);
    const auto& mesh = *res.mesh2;

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

    const double rho = 1.225;
    const double mu = 1.81e-5;
    const double nu = mu / rho;
    const double Uinf = 30.0;

    fvm::SIMPLE<2>::Config cfg;
    cfg.tolerance = 1e-4;
    cfg.runFixedIterations = true;
    cfg.convergenceHoldIterations = 3;
    cfg.alphaU = 0.3;
    cfg.alphaP = 0.1;
    cfg.simplec = false;
    cfg.rhieChow = true;
    cfg.pressureCorrectors = 5;
    cfg.printFrequency = 1000000;
    cfg.convectionScheme = fvm::ConvectionScheme::Upwind;
    cfg.gradientScheme = fvm::GradientScheme::LeastSquares;
    cfg.turbulenceEnabled = true;
    cfg.wallFunction = true;
    cfg.kInf = 1e-3;
    cfg.omegaInf = 1.0;
    cfg.alphaK = 0.3;
    cfg.alphaOmega = 0.3;
    cfg.maxIterations = 200;
    cfg.pressureAmgThreshold = 1000;   // default 10000
    cfg.momentumAmgThreshold = 1000;   // default 20000
    cfg.useAMGMomentum = true;   

    fvm::SIMPLE<2> solver(mesh, rho, nu, cfg);

    solver.setVelocityBCOnFaces(inlet, fvm::VelocityBCType::Inlet, Uinf, 0.0, 0.0);
    solver.setVelocityBCOnFaces(outlet, fvm::VelocityBCType::Outlet);
    solver.setPressureOutletOnFacesWithBackflow(outlet, 0.0, true, Uinf, 0.0, 0.0, 1e-3, 1.0);
    solver.setVelocityBCOnFaces(wall, fvm::VelocityBCType::NoSlip);
    solver.setVelocityBCOnFaces(top, fvm::VelocityBCType::Symmetry);

    // diagnostics
    // diagnostics before solve
    {
        double minV=1e18,maxV=0;
        for (std::size_t i=0;i<mesh.nCells();++i){ double v=mesh.cellVolume(i); minV=std::min(minV,v); maxV=std::max(maxV,v);}
        double minA=1e18,maxA=0;
        for (std::size_t f=0;f<mesh.nFaces();++f){ double a=mesh.faceArea(f); minA=std::min(minA,a); maxA=std::max(maxA,a);}
        std::cout << "FACES: " << mesh.nFaces() << " faceArea min/max: " << minA << " " << maxA << "\n";
    }

    fvm::VectorField<2> velocity(mesh, 0.0);
    fvm::Field<2> pressure(mesh, 0.0);
    velocity.assign(0, Uinf);

    solver.solve(velocity, pressure);

    std::cout << "maxU=" << *std::max_element(velocity[0].values().begin(), velocity[0].values().end()) << "\n";
    std::cout << "continuity=" << solver.finalContinuityResidual()
              << " momentum=" << solver.finalMomentumResidual() << "\n";
    if (solver.turbulence()) {
        const auto& nut = solver.turbulence()->nut();
        double mn=1e18,mx=0;
        for (std::size_t i=0;i<mesh.nCells();++i){ mn=std::min(mn,nut[i]); mx=std::max(mx,nut[i]);}
        std::cout << "nut min/max: " << mn << " " << mx << "\n";
        std::cout << "maxYPlus=" << solver.turbulence()->maxYPlus() << "\n";
    }

    // Wall shear at x~0.8 (cell closest to wall)
    double cfSolver = 0, cfRef = 0, rex = 0;
    {
        double bestY = 1e18; std::size_t bestC = 0; bool found=false;
        for (std::size_t c = 0; c < mesh.nCells(); ++c) {
            auto center = mesh.cellCenter(c);
            if (std::abs(center[0] - 0.8) > 0.05) continue;
            if (center[1] < bestY) { bestY = center[1]; bestC = c; found=true; }
        }
        if (found) {
            double u0 = velocity[0][bestC];
            double y0 = mesh.cellCenter(bestC)[1];
            double tau = mu * u0 / y0;
            cfSolver = tau / (0.5 * rho * Uinf * Uinf);
            rex = Uinf * 0.8 / nu;
            cfRef = std::pow(2.0 * std::log10(rex) - 0.65, -2.3);
            std::cout << "Cf solver=" << cfSolver << " ref=" << cfRef << " y0=" << y0 << " u0=" << u0 << "\n";
        }
    }

    // Boundary layer thickness at x~0.8
    double deltaSolver = 0;
    {
        std::vector<std::pair<double,double>> pts;
        for (std::size_t c = 0; c < mesh.nCells(); ++c) {
            auto center = mesh.cellCenter(c);
            if (std::abs(center[0] - 0.8) > 0.05) continue;
            pts.push_back({center[1], velocity[0][c] / Uinf});
        }
        std::sort(pts.begin(), pts.end());
        for (auto& pr : pts) {
            if (pr.second >= 0.99) { deltaSolver = pr.first; break; }
        }
        double rex2 = Uinf * 0.8 / nu;
        double deltaRef = 0.8 * 0.382 * std::pow(rex2, -0.2);
        std::cout << "delta solver=" << deltaSolver << " ref=" << deltaRef << "\n";
    }

    CHECK(std::isfinite(solver.finalContinuityResidual()));
    CHECK(solver.finalContinuityResidual() < 1e-2);
}
