// Pause / Step / Cancel contract of fvm::Cancellable, exercised through SIMPLE.
//
// The old implementation granted a Step by clearing the pause flag, so one
// Step click restarted the whole run, and a Cancel issued while the solver was
// parked slept forever. These tests pin the boundary-settled semantics:
//
//   * a pause takes effect at the end of an iteration / time step,
//   * Step runs exactly ONE iteration / time step and parks again,
//   * Cancel wakes a parked solver and stops on a step boundary,
//   * a queued RestartState continues the clock and the BDF2 history.
#include "doctest/doctest.h"

#include "fvm/SIMPLE.h"
#include "mesh/BoundaryUtils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

template <typename Pred>
bool waitFor(Pred pred, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (pred()) return true;
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return pred();
}

// A small open channel: velocity inlet on the left, pressure outlet on the
// right, no-slip walls above and below. Cheap enough that the tests only wait
// for a handful of solver iterations.
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

fvm::SIMPLE<2>::Config baseConfig() {
    fvm::SIMPLE<2>::Config cfg;
    cfg.alphaU = 0.7;
    cfg.alphaP = 0.3;
    cfg.rhieChow = true;
    cfg.pressureCorrectors = 2;
    cfg.printFrequency = 1000000;
    cfg.convectionScheme = fvm::ConvectionScheme::Upwind;
    cfg.gradientScheme = fvm::GradientScheme::LeastSquares;
    return cfg;
}

fvm::SIMPLE<2>::Config transientConfig() {
    auto cfg = baseConfig();
    cfg.maxIterations = 100;
    cfg.tolerance = 0.0; // never "converges": the run is bounded by tEnd
    cfg.solverType = fvm::SolverType::SIMPLE;
    cfg.transient.enabled = true;
    cfg.transient.dt = 0.005;
    cfg.transient.tEnd = 50.0;
    cfg.transient.scheme = fvm::TimeScheme::Euler;
    cfg.transient.adaptive = false;
    cfg.transient.maxOuterCorrectors = 1;
    return cfg;
}

// Owns the solver thread and cancels + joins on destruction, so a failing
// REQUIRE can never leave a joinable std::thread behind (terminate).
struct Runner {
    std::thread thread;
    fvm::SIMPLE<2>* solver = nullptr;

    ~Runner() { stop(); }

    void stop() {
        if (thread.joinable()) {
            if (solver) solver->requestCancel();
            thread.join();
        }
    }
};

} // namespace

TEST_SUITE("fvm") {

TEST_CASE("fvm.cancellable.pause_step_resume_cancel") {
    Channel ch;
    auto cfg = baseConfig();
    // Large budget + zero tolerance: the run only stops when we cancel it.
    cfg.maxIterations = 5000000;
    cfg.tolerance = 0.0;
    cfg.runFixedIterations = false;
    cfg.convergenceHoldIterations = 1;

    std::atomic<int> lastIteration{0};
    cfg.iterationCallback = [&](int iter, double, double) {
        lastIteration.store(iter);
    };

    fvm::SIMPLE<2> solver(ch.mesh, 1.0, 0.01, cfg);
    ch.apply(solver);
    fvm::VectorField<2> velocity(ch.mesh, 0.0);
    fvm::Field<2> pressure(ch.mesh, 0.0);

    Runner runner;
    runner.solver = &solver;
    runner.thread = std::thread([&] { solver.solve(velocity, pressure); });

    REQUIRE(waitFor([&] { return lastIteration.load() >= 3; }));
    CHECK_FALSE(solver.isPaused());
    CHECK_FALSE(solver.isPauseSettled());

    // --- Pause settles at an iteration boundary --------------------------
    solver.requestPause();
    REQUIRE(waitFor([&] { return solver.isPauseSettled(); }));
    CHECK(solver.isPaused());
    const int frozen = lastIteration.load();
    CHECK(frozen >= 3);

    std::this_thread::sleep_for(80ms);
    CHECK_MESSAGE(lastIteration.load() == frozen,
                  "a paused solver must not perform extra iterations");

    // --- Step advances exactly one iteration and stays paused -------------
    solver.requestStep();
    REQUIRE(waitFor([&] { return lastIteration.load() == frozen + 1; }));
    REQUIRE(waitFor([&] { return solver.isPauseSettled(); }));
    std::this_thread::sleep_for(80ms);
    CHECK_MESSAGE(lastIteration.load() == frozen + 1,
                  "one Step click must advance exactly one iteration");
    CHECK(solver.isPaused());

    // --- Resume releases the run ------------------------------------------
    solver.requestResume();
    REQUIRE(waitFor([&] { return lastIteration.load() > frozen + 1; }));

    // --- Cancel stops the run ---------------------------------------------
    solver.requestCancel();
    runner.stop();
    CHECK(solver.status() == fvm::SIMPLE<2>::Status::Cancelled);
}

TEST_CASE("fvm.transient.pause_step_cancel_on_a_step_boundary") {
    Channel ch;
    auto cfg = transientConfig();

    std::atomic<int> lastStep{0};
    cfg.iterationCallback = [&](int step, double, double) { lastStep.store(step); };

    fvm::SIMPLE<2> solver(ch.mesh, 1.0, 0.01, cfg);
    ch.apply(solver);
    fvm::VectorField<2> velocity(ch.mesh, 0.0);
    fvm::Field<2> pressure(ch.mesh, 0.0);

    Runner runner;
    runner.solver = &solver;
    runner.thread = std::thread([&] { solver.solveTransient(velocity, pressure); });

    REQUIRE(waitFor([&] { return lastStep.load() >= 2; }));
    solver.requestPause();
    REQUIRE(waitFor([&] { return solver.isPauseSettled(); }));

    const int frozen = lastStep.load();
    REQUIRE(frozen >= 2);
    // A pause is only reported as settled once the step clock and the fields
    // describe the same completed step.
    CHECK(solver.currentStep() == frozen);
    CHECK(solver.currentTime() == doctest::Approx(frozen * cfg.transient.dt).epsilon(1e-9));

    std::this_thread::sleep_for(80ms);
    CHECK_MESSAGE(lastStep.load() == frozen,
                  "a paused transient run must not advance time");

    // --- Step = exactly one time step -------------------------------------
    solver.requestStep();
    REQUIRE(waitFor([&] { return lastStep.load() == frozen + 1; }));
    REQUIRE(waitFor([&] { return solver.isPauseSettled(); }));
    std::this_thread::sleep_for(80ms);
    CHECK(lastStep.load() == frozen + 1);
    CHECK(solver.currentStep() == frozen + 1);
    CHECK(solver.currentTime() ==
          doctest::Approx((frozen + 1) * cfg.transient.dt).epsilon(1e-9));

    // --- Cancel while parked must wake the solver -------------------------
    // This is the regression: the old pause loop ignored cancel requests, so
    // cancelling a paused run hung until the user resumed it.
    solver.requestCancel();
    runner.stop();

    CHECK(solver.status() == fvm::SIMPLE<2>::Status::Cancelled);
    CHECK(solver.currentStep() == frozen + 1);
    // The cancel landed on a boundary, so the saved state is a whole step.
    CHECK(solver.currentTime() ==
          doctest::Approx(solver.currentStep() * cfg.transient.dt).epsilon(1e-9));

    // A cancelled step must still leave a finite, usable field.
    double maxU = 0.0;
    for (std::size_t c = 0; c < ch.mesh.nCells(); ++c)
        maxU = std::max(maxU, std::abs(velocity[0][c]));
    CHECK(std::isfinite(maxU));
}

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
