#pragma once

#include "cfd/cfd_types.h"
#include "fvm/SIMPLE.h"
#include "fvm/SIMPLETypes.h"

namespace cfd {

// Single place where CaseSettings becomes the solver's SIMPLE config, so the
// UI controller never has to spell out the 40-field mapping and a knob added
// to fvm::SIMPLE::Config surfaces here as a compile error.
template <std::size_t Dim>
inline typename fvm::SIMPLE<Dim>::Config toFvmConfig(const CaseSettings& settings) {
    typename fvm::SIMPLE<Dim>::Config sc;
    sc = settings; // Copy all CaseSettings fields (Config inherits from CaseSettings)
    sc.solverType = settings.transient.enabled ? fvm::SolverType::PISO : fvm::SolverType::SIMPLE;
    sc.convectionScheme = settings.scheme == 1 ? fvm::ConvectionScheme::Central
                          : (settings.scheme == 2 ? fvm::ConvectionScheme::SecondOrderUpwind
                             : fvm::ConvectionScheme::Upwind);
    sc.gradientScheme = settings.gradientScheme == 1 ? fvm::GradientScheme::GreenGaussCellBased
                   : (settings.gradientScheme == 2 ? fvm::GradientScheme::GreenGaussNodeBased : fvm::GradientScheme::LeastSquares);
    sc.pressureUseAMG = true;
    sc.pressureUseAMGPreconditioner = true;
    sc.pressureAmgThreshold = 0;
    sc.pressureRefCell = settings.pressureRefCell == 0 ? SIZE_MAX : settings.pressureRefCell;
    // A fixed iteration cap overrides maxIterations and disables tolerance
    // gating (equivalent to runFixedIterations).
    if (settings.fixedIterations) {
        sc.maxIterations = *settings.fixedIterations;
        sc.runFixedIterations = true;
    }

    return sc;
}

} // namespace cfd