#pragma once

#include "cfd/cfd_types.h"
#include "fvm/AMG.h"

namespace cfd {

// Translate the UI's plain AMG mirror into the solver's multigrid config.
//
// cfd::AmgSettings exists so the UI never has to include Eigen; this header is
// the single place the two representations meet. Every field is copied
// explicitly (with enum switches rather than casts) so a knob added to
// fvm::AMGHierarchy::Config shows up here as a compile error rather than
// silently defaulting.
inline fvm::AMGHierarchy::Config toFvmAmg(const AmgSettings& s) {
    using F = fvm::AMGHierarchy::Config;
    F c;

    c.strengthThreshold = s.strengthThreshold;
    c.truncThreshold = s.truncThreshold;
    c.maxLevels = s.maxLevels;
    c.minCoarseSize = s.minCoarseSize;
    c.preSweeps = s.preSweeps;
    c.postSweeps = s.postSweeps;
    c.maxIterations = s.maxIterations;
    c.tolerance = s.tolerance;
    c.epsilon = s.epsilon;
    c.stagnationFactor = s.stagnationFactor;
    c.stagnationMinIter = s.stagnationMinIter;
    c.stagnationLooseFactor = s.stagnationLooseFactor;
    c.nonsymmetric = s.nonsymmetric;

    c.coarseEnough = s.coarseEnough;
    c.directCoarse = s.directCoarse;

    c.tolerance = s.tolerance;

    switch (s.coarsening) {
        case AmgSettings::Coarsening::RugeStuben: c.coarsening = F::Coarsening::RugeStuben; break;
        case AmgSettings::Coarsening::SmoothedAggregation:
        default:                                  c.coarsening = F::Coarsening::SmoothedAggregation; break;
    }
    // Our pressure/momentum solves are scalar, so 's "Automatic"
    // group size (2 in 2D, 4 in 3D) resolves to 1: block coarsening only makes
    // sense for a genuinely blocked system.
    c.groupSizeControl = (s.groupSizeControl == AmgSettings::GroupSizeControl::Manual)
                             ? F::GroupSizeControl::Manual
                             : F::GroupSizeControl::Automatic;
    c.blockSize = (s.groupSizeControl == AmgSettings::GroupSizeControl::Manual)
                      ? s.blockSize
                      : 1;

    switch (s.verbosity) {
        case AmgSettings::Verbosity::Low:       c.verbosity = F::Verbosity::Low; break;
        case AmgSettings::Verbosity::High:      c.verbosity = F::Verbosity::High; break;
        case AmgSettings::Verbosity::Diagnosis: c.verbosity = F::Verbosity::Diagnosis; break;
        case AmgSettings::Verbosity::None:
        default:                                c.verbosity = F::Verbosity::None; break;
    }

    switch (s.cycleType) {
        case AmgSettings::CycleType::WCycle:   c.cycleType = F::CycleType::WCycle; break;
        case AmgSettings::CycleType::FCycle:   c.cycleType = F::CycleType::FCycle; break;
        case AmgSettings::CycleType::VCycle:
        default:                               c.cycleType = F::CycleType::VCycle; break;
    }

    switch (s.acceleration) {
        case AmgSettings::Acceleration::None:              c.acceleration = F::Acceleration::None; break;
        case AmgSettings::Acceleration::BiCGStab:          c.acceleration = F::Acceleration::BiCGStab; break;
        case AmgSettings::Acceleration::GMRES:             c.acceleration = F::Acceleration::GMRES; break;
        case AmgSettings::Acceleration::ConjugateGradient:
        default:                                           c.acceleration = F::Acceleration::ConjugateGradient; break;
    }

    switch (s.smoother) {
        case AmgSettings::Smoother::Jacobi:      c.smoother = F::Smoother::Jacobi; break;
        case AmgSettings::Smoother::ILU:         c.smoother = F::Smoother::ILU0; break;
        case AmgSettings::Smoother::GaussSeidel:
        default:                                 c.smoother = F::Smoother::GaussSeidel; break;
    }

    switch (s.scaling) {
        case AmgSettings::ProportionalScaling::Automatic: c.scaling = F::ProportionalScaling::Automatic; break;
        case AmgSettings::ProportionalScaling::Disabled:
        default:                                          c.scaling = F::ProportionalScaling::Disabled; break;
    }

    return c;
}

} // namespace cfd
