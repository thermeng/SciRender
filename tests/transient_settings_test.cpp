// Regression checks for the transient configuration surface exposed to the UI.
//
// cfd::TransientSettings is the UI's plain mirror of fvm::SIMPLE::TransientConfig.
// A dropped or mismapped field would make a knob do nothing, or do something
// else, so this test pins the translation field-by-field (including the solver
// type decision) and the enum ordinals the combo relies on.
//
// No Qt/GL: compiles the header-only bridge plus SIMPLETypes (no SIMPLE.cpp).

#include "cfd/transient_settings_bridge.h"
#include "fvm/SIMPLETypes.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL  %s\n", what.c_str());
        ++g_failures;
    }
}

void checkNear(double got, double want, const std::string& what) {
    if (!(std::fabs(got - want) <= 1e-12 * (1.0 + std::fabs(want)))) {
        std::printf("  FAIL  %s (got %.17g want %.17g)\n", what.c_str(), got, want);
        ++g_failures;
    }
}

} // namespace

int main() {
    // Enum ordinals the combo (Backward Euler / BDF2) depends on.
    check(static_cast<int>(cfd::TransientSettings::TimeScheme::BackwardEuler) == 0,
          "BackwardEuler ordinal 0");
    check(static_cast<int>(cfd::TransientSettings::TimeScheme::BDF2) == 1,
          "BDF2 ordinal 1");

    // Steady: solver type SIMPLE, enabled=false, defaults match TransientConfig.
    {
        cfd::TransientSettings s; // defaults = steady
        check(!s.enabled, "default disabled");
        auto c = cfd::toFvmTransient<2>(s);
        check(c.enabled == false, "enabled maps");
        checkNear(c.dt, 0.001, "dt maps");
        checkNear(c.tEnd, 1.0, "tEnd maps");
        check(c.scheme == fvm::TimeScheme::Euler, "BackwardEuler -> Euler");
        check(c.adaptive == false, "adaptive maps");
        checkNear(c.cfl, 0.5, "cfl maps");
        checkNear(c.minDt, 1e-6, "minDt maps");
        checkNear(c.maxDt, 0.01, "maxDt maps");
        check(c.writeInterval == 1, "writeInterval maps");
        check(c.maxOuterCorrectors == 3, "maxOuterCorrectors maps");
        check(c.pvdFile.empty(), "pvdFile maps (empty)");
        check(cfd::toFvmSolverType(s) == fvm::SolverType::SIMPLE, "steady -> SIMPLE");
    }

    // Transient: every field non-default, BDF2, solver type PISO.
    {
        cfd::TransientSettings s;
        s.enabled = true;
        s.dt = 2.5e-4;
        s.tEnd = 4.0;
        s.timeScheme = cfd::TransientSettings::TimeScheme::BDF2;
        s.adaptiveCfl = true;
        s.cfl = 0.3;
        s.minDt = 1e-7;
        s.maxDt = 5e-3;
        s.writeInterval = 7;
        s.maxOuterCorrectors = 5;
        s.pvdFile = "case.pvd";

        auto c = cfd::toFvmTransient<2>(s);
        check(c.enabled == true, "enabled maps");
        checkNear(c.dt, 2.5e-4, "dt maps");
        checkNear(c.tEnd, 4.0, "tEnd maps");
        check(c.scheme == fvm::TimeScheme::BDF2, "BDF2 maps");
        check(c.adaptive == true, "adaptive maps");
        checkNear(c.cfl, 0.3, "cfl maps");
        checkNear(c.minDt, 1e-7, "minDt maps");
        checkNear(c.maxDt, 5e-3, "maxDt maps");
        check(c.writeInterval == 7, "writeInterval maps");
        check(c.maxOuterCorrectors == 5, "maxOuterCorrectors maps");
        check(c.pvdFile == "case.pvd", "pvdFile maps");
        check(cfd::toFvmSolverType(s) == fvm::SolverType::PISO, "transient -> PISO");
    }

    if (g_failures == 0) {
        std::printf("all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
