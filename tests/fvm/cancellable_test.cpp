// Pause / Step / Cancel contract of fvm::Cancellable, tested directly.
//
// This file once exercised the contract through a real SIMPLE solve, with the
// full solver configuration (pressure/velocity under-relaxation, schemes, ...)
// and hundreds of lines of AMG / solver-statistics output per run. None of
// that is needed to pin the contract: Cancellable is a small atomic-flag
// protocol whose only consumer-visible operation is the protected
// waitIfPaused() boundary handler, which loops call at the top of each solver
// iteration. Testing it through SIMPLE meant the suite paid the solver's cost
// and flake for what is really flag logic.
//
// Instead, a minimal Loop harness below exposes waitIfPaused() and runs a fake
// iteration counter in the same shape as SIMPLE::solve() / solveTransient():
// the counter only advances after waitIfPaused() returns true. Steps become
// exact counts, the run is silent, and no solver config is required.
//
// The restart-state continuity case that used to live here never touched
// Cancellable; it now lives in restart_state_test.cpp.
//
// The old implementation granted a Step by clearing the pause flag, so one
// Step click restarted the whole run, and a Cancel issued while the solver was
// parked slept forever. These tests pin the boundary-settled semantics:
//
//   * a pause takes effect at an iteration boundary,
//   * Step runs exactly ONE iteration and parks again,
//   * Resume releases the run,
//   * Cancel wakes a parked loop and stops on a boundary.
#include "doctest/doctest.h"

#include "fvm/Cancellable.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

using namespace std::chrono_literals;

// waitIfPaused() is protected so solvers can call it at their own boundaries.
// The test harness exposes it without changing production visibility.
struct TestCancellable : public fvm::Cancellable {
    using fvm::Cancellable::waitIfPaused;
};

template <typename Pred>
bool waitFor(Pred pred, std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (pred()) return true;
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return pred();
}

// How long to wait before declaring a parked loop frozen. A fake iteration
// costs ~1ms, so any extra work allowed by a bug would show up well inside
// this window.
constexpr auto kSettle = 50ms;

// Runs the same boundary shape as SIMPLE: do work only after waitIfPaused()
// returns true, and exit when it returns false (cancel).
struct Loop {
    TestCancellable cancellable;
    std::atomic<int> iterations{0};
    std::atomic<bool> finished{false};
    std::thread thread;

    Loop() = default;
    Loop(const Loop&) = delete;
    Loop& operator=(const Loop&) = delete;

    ~Loop() { stop(); }

    void start() {
        thread = std::thread([this] {
            while (cancellable.waitIfPaused()) {
                iterations.fetch_add(1, std::memory_order_relaxed);
                // Stand in for per-iteration solver work and keep the counter
                // incrementing at a readable rate instead of spinning.
                std::this_thread::sleep_for(1ms);
            }
            finished.store(true, std::memory_order_release);
        });
    }

    void stop() {
        if (thread.joinable()) {
            // Idempotent with an explicit requestCancel(): wakes a parked loop.
            cancellable.requestCancel();
            thread.join();
        }
    }
};

} // namespace

TEST_SUITE("fvm") {

TEST_CASE("fvm.cancellable.pause_step_resume_cancel") {
    Loop loop;
    loop.start();

    REQUIRE(waitFor([&] { return loop.iterations.load() >= 3; }));
    CHECK_FALSE(loop.cancellable.isPaused());
    CHECK_FALSE(loop.cancellable.isPauseSettled());

    // --- Pause settles at an iteration boundary --------------------------
    loop.cancellable.requestPause();
    REQUIRE(waitFor([&] { return loop.cancellable.isPauseSettled(); }));
    CHECK(loop.cancellable.isPaused());
    const int frozen = loop.iterations.load();
    CHECK(frozen >= 3);

    std::this_thread::sleep_for(kSettle);
    CHECK_MESSAGE(loop.iterations.load() == frozen,
                  "a paused loop must not perform extra iterations");

    // --- Step advances exactly one iteration and stays paused -------------
    loop.cancellable.requestStep();
    REQUIRE(waitFor([&] { return loop.iterations.load() == frozen + 1; }));
    REQUIRE(waitFor([&] { return loop.cancellable.isPauseSettled(); }));
    std::this_thread::sleep_for(kSettle);
    CHECK_MESSAGE(loop.iterations.load() == frozen + 1,
                  "one Step click must advance exactly one iteration");
    CHECK(loop.cancellable.isPaused());
    CHECK(loop.cancellable.isPauseSettled());
    CHECK_FALSE(loop.cancellable.hasStepBudget());

    // --- A second Step proves the loop re-parked rather than resuming -----
    loop.cancellable.requestStep();
    REQUIRE(waitFor([&] { return loop.iterations.load() == frozen + 2; }));
    REQUIRE(waitFor([&] { return loop.cancellable.isPauseSettled(); }));
    std::this_thread::sleep_for(kSettle);
    CHECK(loop.iterations.load() == frozen + 2);
    CHECK(loop.cancellable.isPaused());

    // --- Resume releases the run ------------------------------------------
    loop.cancellable.requestResume();
    REQUIRE(waitFor([&] { return loop.iterations.load() > frozen + 2; }));
    CHECK_FALSE(loop.cancellable.isPauseSettled());

    // --- Cancel stops the run ---------------------------------------------
    loop.cancellable.requestCancel();
    REQUIRE(waitFor([&] { return loop.finished.load(); }));
    loop.stop();
    CHECK(loop.cancellable.isCancelRequested());
}

TEST_CASE("fvm.cancellable.cancel_wakes_parked_loop") {
    Loop loop;
    loop.start();

    REQUIRE(waitFor([&] { return loop.iterations.load() >= 2; }));
    loop.cancellable.requestPause();
    REQUIRE(waitFor([&] { return loop.cancellable.isPauseSettled(); }));
    const int frozen = loop.iterations.load();

    // The regression: the old pause loop ignored cancel requests, so
    // cancelling a parked run hung until the user resumed it. requestCancel()
    // must clear the pause so the parked waitIfPaused() observes the cancel.
    loop.cancellable.requestCancel();
    REQUIRE_MESSAGE(waitFor([&] { return loop.finished.load(); }),
                    "Cancel while parked must wake the loop promptly");
    loop.stop();

    CHECK(loop.iterations.load() == frozen);
    CHECK(loop.cancellable.isCancelRequested());
    CHECK_FALSE(loop.cancellable.isPaused());
    CHECK_FALSE(loop.cancellable.isPauseSettled());
    CHECK_FALSE(loop.cancellable.hasStepBudget());
}

TEST_CASE("fvm.cancellable.cancel_while_running_stops_at_boundary") {
    Loop loop;
    loop.start();

    REQUIRE(waitFor([&] { return loop.iterations.load() >= 5; }));
    const int before = loop.iterations.load();
    loop.cancellable.requestCancel();
    REQUIRE(waitFor([&] { return loop.finished.load(); }));
    loop.stop();

    // At most one in-flight iteration completes; nothing runs after joining.
    const int finalCount = loop.iterations.load();
    CHECK(finalCount >= before);
    std::this_thread::sleep_for(kSettle);
    CHECK(loop.iterations.load() == finalCount);
    CHECK(loop.cancellable.isCancelRequested());
    CHECK_FALSE(loop.cancellable.isPaused());
}

TEST_CASE("fvm.cancellable.pause_requested_before_start_settles_immediately") {
    Loop loop;
    // Requested before any boundary was ever reached: isPaused() (the request
    // flag) is set, but nothing is settled until a loop actually parks.
    loop.cancellable.requestPause();
    CHECK(loop.cancellable.isPaused());
    CHECK_FALSE(loop.cancellable.isPauseSettled());

    loop.start();
    REQUIRE(waitFor([&] { return loop.cancellable.isPauseSettled(); }));
    std::this_thread::sleep_for(kSettle);
    CHECK(loop.iterations.load() == 0);

    loop.cancellable.requestStep();
    REQUIRE(waitFor([&] { return loop.iterations.load() == 1; }));
    REQUIRE(waitFor([&] { return loop.cancellable.isPauseSettled(); }));
    std::this_thread::sleep_for(kSettle);
    CHECK(loop.iterations.load() == 1);
    CHECK(loop.cancellable.isPaused());

    loop.cancellable.requestCancel();
    REQUIRE(waitFor([&] { return loop.finished.load(); }));
    loop.stop();
}

TEST_CASE("fvm.cancellable.request_flags_without_a_loop") {
    // No thread, no loop: these checks pin the flag effects of each request
    // exactly as the solver's boundary handler observes them.
    TestCancellable c;
    CHECK_FALSE(c.isPaused());
    CHECK_FALSE(c.isPauseSettled());
    CHECK_FALSE(c.hasStepBudget());
    CHECK_FALSE(c.isCancelRequested());

    c.requestPause();
    CHECK(c.isPaused());
    // Requested is not settled: no loop has parked yet.
    CHECK_FALSE(c.isPauseSettled());

    c.requestStep();
    CHECK(c.isPaused());
    CHECK(c.hasStepBudget());
    CHECK_FALSE(c.isPauseSettled());

    c.requestResume();
    CHECK_FALSE(c.isPaused());
    CHECK_FALSE(c.hasStepBudget());
    CHECK_FALSE(c.isPauseSettled());

    // Cancel clears both the pause and any outstanding step budget so a parked
    // or budgeted loop cannot continue.
    c.requestStep();
    c.requestCancel();
    CHECK(c.isCancelRequested());
    CHECK_FALSE(c.isPaused());
    CHECK_FALSE(c.hasStepBudget());
}

} // TEST_SUITE
