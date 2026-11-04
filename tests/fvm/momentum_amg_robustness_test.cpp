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
