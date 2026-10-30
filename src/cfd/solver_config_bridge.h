#pragma once

#include "cfd/cfd_types.h"
#include "fvm/SIMPLE.h"
#include "fvm/SIMPLETypes.h"
#include "cfd/amg_settings_bridge.h"
#include "cfd/transient_settings_bridge.h"

namespace cfd {

// Single place where CaseSettings becomes the solver's SIMPLE config, so the
// UI controller never has to spell out the 40-field mapping and a knob added
// to fvm::SIMPLE::Config surfaces here as a compile error.
template <std::size_t Dim>
inline typename fvm::SIMPLE<Dim>::Config toFvmConfig(const CaseSettings& settings) {
    typename fvm::SIMPLE<Dim>::Config sc;

    sc.maxIterations = settings.maxIterations;
    sc.tolerance = settings.tolerance;
    sc.runFixedIterations = settings.runFixedIterations;
    sc.convergenceHoldIterations = settings.convergenceHoldIterations;
    sc.alphaU = settings.alphaU;
    sc.alphaP = settings.alphaP;
    sc.simplec = settings.simplec;
    sc.rhieChow = settings.rhieChow;
    sc.convectionScheme = settings.scheme == 1 ? fvm::ConvectionScheme::Central
                          : (settings.scheme == 2 ? fvm::ConvectionScheme::SecondOrderUpwind
                             : fvm::ConvectionScheme::Upwind);
    sc.gradientScheme = settings.gradientScheme == 1 ? fvm::GradientScheme::GreenGaussCellBased
                       : (settings.gradientScheme == 2 ? fvm::GradientScheme::GreenGaussNodeBased : fvm::GradientScheme::LeastSquares);
    sc.printFrequency = settings.printFrequency;
    sc.residualHistoryWindow = settings.residualHistoryWindow;
    sc.pressureCorrectors = settings.pressureCorrectors;
    sc.limitPressureCorrection = settings.limitPressureCorrection;
    sc.pressureCorrectionLimit = settings.pressureCorrectionLimit;
    sc.pressureRefCell = settings.pressureRefCell == 0 ? SIZE_MAX : settings.pressureRefCell;
    sc.pressureUseAMG = settings.pressureUseAMG;
    sc.pressureUseAMGPreconditioner = settings.pressureUseAMGPreconditioner;
    sc.pressureAmgThreshold = settings.pressureAmgThreshold;
    sc.pressureAmgConfig = toFvmAmg(settings.pressureAmg);
    sc.useAMGMomentum = settings.useAMGMomentum;
    sc.momentumAmgThreshold = settings.momentumAmgThreshold;
    sc.momentumAmgCycles = settings.momentumAmgCycles;
    sc.momentumAmgMaxResidual = settings.momentumAmgMaxResidual;
    sc.momentumSolverStats = settings.momentumSolverStats;
    sc.momentumAmgConfig = toFvmAmg(settings.momentumAmg);
    sc.solverType = toFvmSolverType(settings.transient);
    sc.transient = toFvmTransient<Dim>(settings.transient);

    return sc;
}

} // namespace cfd