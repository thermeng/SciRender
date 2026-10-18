#pragma once

#include "cfd/cfd_types.h"
#include "fvm/AMG.h"

namespace cfd {

// Translate the UI's plain AMG mirror into the solver's multigrid config.
//
// cfd::AmgSettings exists so the UI never has to include Eigen; this header is
// the single place the two representations meet. Every field is copied
// explicitly, so a knob added to fvm::AMGHierarchy::Config shows up here as a
// compile error rather than silently defaulting. The cfd::AmgSettings enums are
// declared in the same order as their fvm counterparts, so the switch cases
// read top-to-bottom.
inline fvm::AMGHierarchy::Config toFvmAmg(const AmgSettings& s) {
    fvm::AMGHierarchy::Config c;

    switch (s.coarseningType) {
        case AmgSettings::CoarseningType::RS:
            c.coarseningType = fvm::AMGHierarchy::CoarseningType::RS; break;
        case AmgSettings::CoarseningType::PMIS:
            c.coarseningType = fvm::AMGHierarchy::CoarseningType::PMIS; break;
    }
    switch (s.interpolationType) {
        case AmgSettings::InterpolationType::Direct:
            c.interpolationType = fvm::AMGHierarchy::InterpolationType::Direct; break;
        case AmgSettings::InterpolationType::Extended:
            c.interpolationType = fvm::AMGHierarchy::InterpolationType::Extended; break;
    }
    switch (s.cycleType) {
        case AmgSettings::CycleType::V:
            c.cycleType = fvm::AMGHierarchy::CycleType::V; break;
        case AmgSettings::CycleType::F:
            c.cycleType = fvm::AMGHierarchy::CycleType::F; break;
    }
    switch (s.smootherType) {
        case AmgSettings::SmootherType::SymmetricGaussSeidel:
            c.smootherType = fvm::AMGHierarchy::SmootherType::SymmetricGaussSeidel; break;
        case AmgSettings::SmootherType::ForwardGaussSeidel:
            c.smootherType = fvm::AMGHierarchy::SmootherType::ForwardGaussSeidel; break;
        case AmgSettings::SmootherType::ForwardSOR:
            c.smootherType = fvm::AMGHierarchy::SmootherType::ForwardSOR; break;
    }

    c.smootherOmega = s.smootherOmega;
    c.strengthThreshold = s.strengthThreshold;
    c.truncThreshold = s.truncThreshold;
    c.maxLevels = s.maxLevels;
    c.minCoarseSize = s.minCoarseSize;
    c.preSweeps = s.preSweeps;
    c.postSweeps = s.postSweeps;
    c.maxIterations = s.maxIterations;
    c.tolerance = s.tolerance;
    c.rngSeed = s.rngSeed;
    c.stagnationFactor = s.stagnationFactor;
    c.stagnationMinIter = s.stagnationMinIter;
    c.stagnationLooseFactor = s.stagnationLooseFactor;
    c.galerkinDropTol = s.galerkinDropTol;
    return c;
}

} // namespace cfd
