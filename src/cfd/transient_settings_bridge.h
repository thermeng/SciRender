#pragma once

#include "cfd/cfd_types.h"
#include "fvm/SIMPLE.h"
#include "fvm/SIMPLETypes.h"

namespace cfd {

// Which solver drives the run. PISO is chosen for transient runs because it
// treats the pressure-velocity coupling implicitly (one momentum predictor plus
// a fixed number of correctors on frozen coefficients) instead of iterating the
// whole SIMPLE loop inside every time step. Note the consequences, which the UI
// reflects by disabling the affected controls: PISO forces alphaU = alphaP = 1
// and turns Rhie-Chow on, so the relaxation and Rhie-Chow settings are inert.
inline fvm::SolverType toFvmSolverType(const TransientSettings& s) {
    return s.enabled ? fvm::SolverType::PISO : fvm::SolverType::SIMPLE;
}

// Translate the UI's plain transient mirror into the solver's transient config.
//
// Like cfd::toFvmAmg, this is the single place the two representations meet, so
// a field added to fvm::SIMPLE::TransientConfig surfaces here as a visible edit
// rather than silently taking its default. `piso` is deliberately left alone:
// it is the deprecated legacy switch, superseded by Config::solverType.
template <std::size_t Dim>
inline typename fvm::SIMPLE<Dim>::TransientConfig toFvmTransient(const TransientSettings& s) {
    typename fvm::SIMPLE<Dim>::TransientConfig c;

    c.enabled = s.enabled;
    c.dt = s.dt;
    c.tEnd = s.tEnd;

    switch (s.timeScheme) {
        case TransientSettings::TimeScheme::BackwardEuler:
            c.scheme = fvm::TimeScheme::Euler; break;
        case TransientSettings::TimeScheme::BDF2:
            c.scheme = fvm::TimeScheme::BDF2; break;
    }

    c.adaptive = s.adaptiveCfl;
    c.cfl = s.cfl;
    c.minDt = s.minDt;
    c.maxDt = s.maxDt;
    c.writeInterval = s.writeInterval;
    c.maxOuterCorrectors = s.maxOuterCorrectors;
    c.nonOrthogonalCorrectors = s.nonOrthogonalCorrectors;
    c.pvdFile = s.pvdFile;
    return c;
}

} // namespace cfd
