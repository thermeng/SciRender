#pragma once

#include <atomic>
#include <chrono>
#include <thread>

namespace fvm {

// Shared cancellation/pause state for all solvers.
//
// Pause semantics: a pause takes effect at a solver iteration boundary (one
// SIMPLE iteration, or one transient time step) so that the fields, the BDF2
// history and the time/step counters always describe a completed step and a
// Save made while paused is a valid restart point.
//
// Step is a one-shot budget: while paused, `requestStep()` lets the solver run
// exactly one iteration and then parks it again. It deliberately does NOT
// resume the run (the previous implementation cleared the pause flag, so one
// Step click restarted the whole solve).
struct Cancellable {
    void requestCancel() {
        cancelRequested_.store(true, std::memory_order_release);
        // A parked solver must notice the cancel instead of waiting forever.
        paused_.store(false, std::memory_order_release);
        stepBudget_.store(0, std::memory_order_release);
    }
    void resetCancel() { cancelRequested_.store(false, std::memory_order_release); }
    bool isCancelRequested() const { return cancelRequested_.load(std::memory_order_acquire); }

    void requestPause() { paused_.store(true, std::memory_order_release); }
    void requestResume() {
        stepBudget_.store(0, std::memory_order_release);
        pausedAck_.store(false, std::memory_order_release);
        paused_.store(false, std::memory_order_release);
    }
    // While paused: run exactly one iteration/time step, then pause again.
    void requestStep() {
        stepBudget_.store(1, std::memory_order_release);
        pausedAck_.store(false, std::memory_order_release);
        paused_.store(true, std::memory_order_release);
    }
    // The pause was *requested* (may still be in flight).
    bool isPaused() const { return paused_.load(std::memory_order_acquire); }
    // The solver is actually parked at a step boundary. Only then is the
    // in-memory state a consistent restart point for Save.
    bool isPauseSettled() const { return pausedAck_.load(std::memory_order_acquire); }
    bool hasStepBudget() const { return stepBudget_.load(std::memory_order_acquire) > 0; }

protected:
    // Boundary handler: call at the top of the solver loop, before doing any
    // work for the next iteration. Blocks while a pause is pending and no step
    // budget is outstanding. Returns false when a cancel was requested while
    // parked, in which case the caller must stop without performing work.
    bool waitIfPaused() {
        for (;;) {
            if (cancelRequested_.load(std::memory_order_acquire)) {
                pausedAck_.store(false, std::memory_order_release);
                return false;
            }
            if (!paused_.load(std::memory_order_acquire)) {
                pausedAck_.store(false, std::memory_order_release);
                return true;
            }
            const int budget = stepBudget_.load(std::memory_order_acquire);
            if (budget > 0) {
                stepBudget_.store(budget - 1, std::memory_order_release);
                pausedAck_.store(false, std::memory_order_release);
                return true;
            }
            pausedAck_.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::atomic<bool> cancelRequested_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> pausedAck_{false};
    std::atomic<int> stepBudget_{0};
};

} // namespace fvm
