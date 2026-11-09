// Restart-state continuity for SIMPLE: a queued RestartState continues the
// transient clock and the BDF2 history from the saved step.
//
// This case used to live in cancellable_test.cpp, but it never touches
// fvm::Cancellable -- it builds a RestartState payload exactly as the
// controller does (cfd_session::queueRestartState) and checks that a second
// solver continues from the saved fields and step instead of restarting from
// zero. It lives in its own file so cancellable_test.cpp can test the pause /
// step / cancel protocol directly, without solver configuration or AMG output.
#include "doctest/doctest.h"

#include "fvm/SIMPLE.h"
#include "mesh/BoundaryUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace {

// A small open channel: velocity inlet on the left, pressure outlet on the
// right, no-slip walls above and below. Cheap enough that the run only needs
// a handful of solver steps.
struct Channel {
    fvm::Mesh<2> mesh{std::array<std::size_t, 2>{8, 4},
                      std::array<double, 2>{2.0, 1.0}};
    std::vector<std::size_t> inlet, outlet, walls;

    Channel() {
        fvm::forEachBoundaryFace(mesh, 0, -1,
                                 [&](std::size_t f, std::size_t) { inlet.push_back(f); });
        fvm::forEachBoundaryFace(mesh, 0, +1,
                                 [&](std::size_t f, std::size_t) { outlet.push_back(f); });
        fvm::forEachBoundaryFace(mesh, 1, -1,
                                 [&](std::size_t f, std::size_t) { walls.push_back(f); });
        fvm::forEachBoundaryFace(mesh, 1, +1,
                                 [&](std::size_t f, std::size_t) { walls.push_back(f); });
    }

    void apply(fvm::SIMPLE<2>& solver) const {
        solver.setVelocityBCOnFacesWithSlip(inlet, "inlet", 1.0, 0.0, 0.0, false, 0.0);
        solver.setVelocityBCOnFacesWithSlip(outlet, "pressure_outlet", 0.0, 0.0, 0.0,
                                            false, 0.0);
        solver.setVelocityBCOnFacesWithSlip(walls, "wall", 0.0, 0.0, 0.0, false, 0.0);
    }
};

fvm::SIMPLE<2>::Config transientConfig() {
    fvm::SIMPLE<2>::Config cfg;
    cfg.pressureCorrectors = 2;
    cfg.printFrequency = 1000000;
    cfg.convectionScheme = fvm::ConvectionScheme::Upwind;
    cfg.gradientScheme = fvm::GradientScheme::LeastSquares;
    cfg.maxIterations = 100;
    cfg.tolerance = 0.0; // never "converges": the run is bounded by tEnd
    cfg.solverType = fvm::SolverType::SIMPLE;
    cfg.transient.enabled = true;
    cfg.transient.dt = 0.005;
    cfg.transient.tEnd = 50.0;
    cfg.transient.timeScheme = fvm::TimeScheme::Euler;
    cfg.transient.adaptiveCfl = false;
    cfg.transient.maxOuterCorrectors = 1;
    return cfg;
}

} // namespace

TEST_SUITE("fvm") {

TEST_CASE("fvm.transient.restart_state_continues_at_the_saved_step") {
    Channel ch;

    // --- First leg: t = 0 .. 0.03 (3 steps of 0.01) -----------------------
    auto cfg1 = transientConfig();
    cfg1.transient.dt = 0.01;
    cfg1.transient.tEnd = 0.03;

    fvm::SIMPLE<2> first(ch.mesh, 1.0, 0.01, cfg1);
    ch.apply(first);
    fvm::VectorField<2> v1(ch.mesh, 0.0);
    fvm::Field<2> p1(ch.mesh, 0.0);
    first.solveTransient(v1, p1);

    REQUIRE(first.currentStep() == 3);
    REQUIRE(first.hasTransientState());
    REQUIRE(first.currentTime() == doctest::Approx(0.03).epsilon(1e-9));

    double savedMaxU = 0.0;
    for (std::size_t c = 0; c < ch.mesh.nCells(); ++c)
        savedMaxU = std::max(savedMaxU, std::abs(v1[0][c]));
    REQUIRE(savedMaxU > 0.0);

    // --- Build the restart payload exactly as the controller does ---------
    fvm::SIMPLE<2>::RestartState rs;
    rs.hasVelocity = true;
    for (std::size_t d = 0; d < 2; ++d)
        for (std::size_t c = 0; c < ch.mesh.nCells(); ++c)
            rs.velocity[d].push_back(v1[d][c]);
    for (std::size_t c = 0; c < ch.mesh.nCells(); ++c)
        rs.pressure.push_back(p1[c]);

    rs.hasTransient = true;
    rs.time = first.currentTime();
    rs.dt = first.currentDt();
    rs.dtPrev = first.currentDtPrev();
    rs.step = first.currentStep();

    rs.hasVelocityHistory = true;
    const auto& history = first.velocityNM1();
    for (std::size_t d = 0; d < 2; ++d) {
        REQUIRE(history[d].size() == ch.mesh.nCells());
        for (std::size_t c = 0; c < ch.mesh.nCells(); ++c)
            rs.velocityNM1[d].push_back(history[d][c]);
    }

    // --- Second leg: continue to t = 0.04 ---------------------------------
    auto cfg2 = transientConfig();
    cfg2.transient.dt = 0.01;
    cfg2.transient.tEnd = 0.04;

    fvm::SIMPLE<2> second(ch.mesh, 1.0, 0.01, cfg2);
    ch.apply(second);
    second.setRestartState(std::move(rs));
    CHECK(second.hasRestartState());

    fvm::VectorField<2> v2(ch.mesh, 0.0);
    fvm::Field<2> p2(ch.mesh, 0.0);
    second.solveTransient(v2, p2);

    CHECK_FALSE(second.hasRestartState()); // consumed, never re-applied
    CHECK(second.currentStep() == 4);
    CHECK(second.currentTime() == doctest::Approx(0.04).epsilon(1e-9));

    // The restored field must not have been replaced by the zero initial state
    // (that was the bug: a restart queued through the old restore* calls was
    // applied before the turbulence model existed and was then overwritten).
    double maxU = 0.0;
    for (std::size_t c = 0; c < ch.mesh.nCells(); ++c)
        maxU = std::max(maxU, std::abs(v2[0][c]));
    CHECK_MESSAGE(maxU > 0.5 * savedMaxU,
                  "continuation must start from the saved velocity field");
}

} // TEST_SUITE
