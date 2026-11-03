// Robustness check for the momentum AMG path: runs a short lid-driven cavity
// solve with useAMGMomentum enabled and asserts that the AMG hierarchy is
// built, the solve stays finite, and the fallback path (if taken) is clean.
#include "doctest/doctest.h"

#include "fvm/SIMPLE.h"
#include "io/ANSYSMeshReader.h"
#include "support/test_support.h"

#include <cmath>
#include <string>

using namespace test_support;

namespace {

struct Setup {
    fvm::Mesh<2> mesh;
    std::string lidGroup;
};

Setup loadCavity() {
    requireSample("cfd/CavityQuad_40x40.msh");
    const std::string path = samplePath("cfd/CavityQuad_40x40.msh").string();
    auto read = fvm::ANSYSMeshReader<2>::read(path);
    Setup s;
    s.mesh = std::move(read.mesh);
    double bestY = -1e300;
    for (const auto& [name, faces] : s.mesh.boundaryGroups()) {
        if (faces.empty()) continue;
        double meanY = 0.0;
        for (std::size_t f : faces) meanY += s.mesh.faceCenterPtr(f)[1];
        meanY /= static_cast<double>(faces.size());
        if (meanY > bestY) { bestY = meanY; s.lidGroup = name; }
    }
    return s;
}

} // namespace

TEST_CASE("fvm.momentum_amg.robustness") {
    const Setup setup = loadCavity();

    double xmin = 1e300, xmax = -1e300, ymin = 1e300, ymax = -1e300;
    for (std::size_t f = 0; f < setup.mesh.nFaces(); ++f) {
        if (setup.mesh.neighbor(f) != std::size_t(-1)) continue;
        const double* c = setup.mesh.faceCenterPtr(f);
        xmin = std::min(xmin, c[0]); xmax = std::max(xmax, c[0]);
        ymin = std::min(ymin, c[1]); ymax = std::max(ymax, c[1]);
    }
    const double L = ymax - ymin;
    const double nu = 1.0 * L / 400.0;

    fvm::SIMPLE<2>::Config cfg;
    cfg.maxIterations = 20;
    cfg.runFixedIterations = true;
    cfg.tolerance = 1e-6;
    cfg.convergenceHoldIterations = 1;
    cfg.alphaU = 0.7;
    cfg.alphaP = 0.3;
    cfg.simplec = false;
    cfg.rhieChow = true;
    cfg.pressureCorrectors = 2;
    cfg.printFrequency = 1000000;
    cfg.convectionScheme = fvm::ConvectionScheme::SecondOrderUpwind;
    cfg.gradientScheme = fvm::GradientScheme::LeastSquares;
    cfg.useAMGMomentum = true;
    cfg.momentumAmgThreshold = 0;
    cfg.momentumAmgMaxResidual = 1e-5;
    cfg.momentumAmgCycles = 5;
    cfg.momentumAmgConfig.preSweeps = 4;
    cfg.momentumAmgConfig.postSweeps = 4;

    fvm::SIMPLE<2> solver(setup.mesh, 1.0, nu, cfg);

    for (const auto& [name, faces] : setup.mesh.boundaryGroups()) {
        if (faces.empty()) continue;
        if (name == setup.lidGroup)
            solver.setVelocityBCOnFacesWithSlip(faces, "moving_wall", 1.0, 0.0, 0.0, false, 0.0);
        else
            solver.setVelocityBCOnFacesWithSlip(faces, "wall", 0.0, 0.0, 0.0, false, 0.0);
    }

    fvm::VectorField<2> velocity(setup.mesh, 0.0);
    fvm::Field<2> pressure(setup.mesh, 0.0);
    solver.solve(velocity, pressure);

    CHECK(std::isfinite(solver.finalContinuityResidual()));
    CHECK(std::isfinite(solver.finalMomentumResidual()));
    // AMG must have been attempted (built at least once) without crashing.
    CHECK(solver.momentumAmgBuilds() >= 1);

    std::cout << "Momentum AMG: solves=" << solver.momentumAmgSolves()
              << " rejected=" << solver.momentumAmgRejected()
              << " builds=" << solver.momentumAmgBuilds() << "\n";
}

// The default config must run AMG as a BiCGSTAB preconditioner, not as the
// outer solver. `momentumAmgConfig.acceleration == None` only says the
// hierarchy has no inner Krylov acceleration; it must not silently switch on
// the standalone mode. At Re=1000 on the 129x129 cavity the standalone cycles
// are not a contraction for the changing nonsymmetric operator: the momentum
// residual grows within one AMG-rebuild window and the velocity diverges to
// 1e150. This test pins the contract so that regression cannot come back.
TEST_CASE("fvm.momentum_amg.default_config_uses_preconditioner") {
    const Setup setup = loadCavity();

    auto runOnce = [&setup](bool asSolver) {
        fvm::SIMPLE<2>::Config cfg;
        cfg.maxIterations = 1;
        cfg.runFixedIterations = true;
        cfg.printFrequency = 1000000;
        cfg.useAMGMomentum = true;
        cfg.momentumAmgThreshold = 0;
        cfg.momentumAmgAsSolver = asSolver;
        // Keep the shipped acceleration: None means "no inner acceleration",
        // not "standalone solve".
        REQUIRE(cfg.momentumAmgConfig.acceleration ==
                fvm::AMGHierarchy::Config::Acceleration::None);

        fvm::SIMPLE<2> solver(setup.mesh, 1.0, 1.0 / 400.0, cfg);
        for (const auto& [name, faces] : setup.mesh.boundaryGroups()) {
            if (faces.empty()) continue;
            if (name == setup.lidGroup)
                solver.setVelocityBCOnFacesWithSlip(faces, "moving_wall", 1.0, 0.0, 0.0,
                                                    false, 0.0);
            else
                solver.setVelocityBCOnFacesWithSlip(faces, "wall", 0.0, 0.0, 0.0, false, 0.0);
        }

        fvm::VectorField<2> velocity(setup.mesh, 0.0);
        fvm::Field<2> pressure(setup.mesh, 0.0);
        solver.solve(velocity, pressure);

        CHECK(solver.momentumAmgBuilds() >= 1);
        return solver.momentumAmgStandaloneSolves();
    };

    CHECK_MESSAGE(runOnce(false) == 0,
                  "the default config must not use the standalone AMG solver");
    CHECK_MESSAGE(runOnce(true) > 0,
                  "momentumAmgAsSolver=true must still run the hierarchy as the solver");
}
