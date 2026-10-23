#pragma once

#include <atomic>

namespace fvm {

// Shared cancellation state for all solvers. Inheriting this removes the
// duplicated atomic + requestCancel boilerplate that previously drifted
// between AdvectionDiffusionSolver and SIMPLE.
struct Cancellable {
    void requestCancel() { cancelRequested_.store(true, std::memory_order_release); }
    void resetCancel()   { cancelRequested_.store(false, std::memory_order_release); }
    bool isCancelRequested() const { return cancelRequested_.load(std::memory_order_acquire); }

protected:
    std::atomic<bool> cancelRequested_{false};
};

} // namespace fvm