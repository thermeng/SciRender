#pragma once
#include <cstdint>
#include <memory>

#include "cfd/solution_state.h"

// One published frame from the solve. The snapshot is complete (velocity,
// pressure and turbulence), so the render-prep thread never has to reach back
// into the solver while it is advancing.
struct TransientStep {
    int step = 0;
    double time = 0.0;
    std::shared_ptr<const cfd::SolutionSnapshot> state;
    uint64_t gen = 0;
};
