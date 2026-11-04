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
    sc.contTol = settings.contTol;
    sc.momUTol = settings.momUTol;
    sc.momVTol = settings.momVTol;
    sc.momWTol = settings.momWTol;
    sc.turbKTol = settings.turbKTol;
    sc.turbOmegaTol = settings.turbOmegaTol;
    sc.gateContinuity = settings.gateContinuity;
    sc.gateMomU = settings.gateMomU;
    sc.gateMomV = settings.gateMomV;
    sc.gateMomW = settings.gateMomW;
    sc.gateTurbK = settings.gateTurbK;
    sc.gateTurbOmega = settings.gateTurbOmega;
    sc.fixedIterations = settings.fixedIterations;
    sc.runFixedIterations = settings.runFixedIterations;
    // A fixed iteration cap overrides maxIterations and disables tolerance
    // gating (equivalent to runFixedIterations).
    if (settings.fixedIterations) {
        sc.maxIterations = *settings.fixedIterations;
        sc.runFixedIterations = true;
    }
    sc.convergenceHoldIterations = settings.convergenceHoldIterations;
    sc.normalizeResiduals = settings.normalizeResiduals;
    sc.residualNormalizationIterations = settings.residualNormalizationIterations;
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
    sc.velocityCorrectionMaxUnlimitedSpeed = settings.velocityCorrectionMaxUnlimitedSpeed;
    sc.velocityCorrectionMaxGrowthRate = settings.velocityCorrectionMaxGrowthRate;
    sc.pressureCorrectors = settings.pressureCorrectors;
    sc.limitPressureCorrection = settings.limitPressureCorrection;
    sc.pressureCorrectionLimit = settings.pressureCorrectionLimit;
    sc.pressureRefCell = settings.pressureRefCell == 0 ? SIZE_MAX : settings.pressureRefCell;
    sc.pressureUseAMG = settings.pressureUseAMG;
    sc.pressureUseAMGPreconditioner = settings.pressureUseAMGPreconditioner;
    sc.pressureAmgThreshold = settings.pressureAmgThreshold;
    sc.pressureAmgCycles = settings.pressureAmgCycles;
    sc.pressureSolveTolerance = settings.pressureSolveTolerance;
    sc.pressureAmgConfig = toFvmAmg(settings.pressureAmg);
    sc.solverType = toFvmSolverType(settings.transient);
    sc.transient = toFvmTransient<Dim>(settings.transient);

    return sc;
}

} // namespace cfd