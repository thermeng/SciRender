#pragma once

#include "cfd/cfd_types.h"
#include "cfd/convergence_policy.h"
#include "fvm/SIMPLE.h"
#include "fvm/SIMPLETypes.h"

namespace cfd {

// Single place where CaseSettings becomes the solver's SIMPLE config, so the
// UI controller never has to spell out the 40-field mapping and a knob added
// to fvm::SIMPLE::Config surfaces here as a compile error.
//
// Stopping criteria now live in the nested CaseSettings::stopping object and
// are resolved into an immutable ResolvedStoppingPolicy that the solver reads
// directly. The raw CaseSettings::stopping is still copied (via sc = settings)
// so the config carries the user-facing values; the resolved policy is what
// the iteration loop actually consults.
template <std::size_t Dim>
inline typename fvm::SIMPLE<Dim>::Config toFvmConfig(const CaseSettings& settings) {
    typename fvm::SIMPLE<Dim>::Config sc;
    sc = settings; // Copy all CaseSettings fields (Config inherits from CaseSettings)
    // Config declares its own TransientConfig, shadowing CaseSettings::transient.
    // The solver reads the derived member, so it must be set explicitly or UI
    // transient knobs are silently ignored.
    sc.transient = settings.transient;
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

    // Resolve stopping criteria against the solve context (dimension,
    // turbulence, transient mode). Inapplicable equations (Momentum W in 2D,
    // k/omega without turbulence) are excluded from the gate here so the
    // solver never has to reason about dimension/turbulence inside the loop.
    //
    // A programmatic caller can hand us min > max (the UI validates, the
    // controller setter and case loader do not). Clamp rather than reject the
    // whole policy: falling back to defaults silently changes tEnd and the
    // corrector cap, which is far worse than running one fewer corrector.
    auto stopping = settings.stopping;
    if (stopping.transient.minOuterCorrectors >
        stopping.transient.maxOuterCorrectors) {
        stopping.transient.minOuterCorrectors =
            stopping.transient.maxOuterCorrectors;
    }
    auto resolved = resolveStoppingPolicy(
        stopping, Dim == 3, settings.turbulence, settings.transient.enabled);
    // Callers validate before reaching here; fall back to an empty policy
    // (iteration limit 0) so an invalid config cannot run forever.
    sc.stoppingPolicy = resolved.value_or(cfd::ResolvedStoppingPolicy{});

    return sc;
}

} // namespace cfd
