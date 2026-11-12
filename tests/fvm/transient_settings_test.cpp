// Regression checks for the transient configuration surface exposed to the UI.
//
// cfd::CaseSettings carries a cfd::TransientSettings mirror of the solver's
// transient config. A dropped or mismapped field would make a knob do
// nothing, or do something else, so this test pins the translation
// field-by-field (including the solver type decision) and the enum ordinals
// the combo relies on.
//
// No Qt/GL: only the header-only bridge and SIMPLETypes are needed.

#include "cfd/cfd_types.h"
#include "cfd/solver_config_bridge.h"
#include "fvm/SIMPLETypes.h"
#include "fvm/Types.h"
#include "doctest/doctest.h"

#include <cmath>

namespace {

#define CHECK_NEAR(got, want)                                                                      \
    CHECK_MESSAGE(std::fabs((got) - (want)) <= 1e-12 * (1.0 + std::fabs(want)),                   \
                  #got " ~= " #want " (got " << (got) << ", want " << (want) << ")")

} // namespace

TEST_SUITE("fvm") {

// Enum ordinals the combo (Backward Euler / BDF2) depends on.
TEST_CASE("fvm.transient.enum_ordinals") {
    CHECK_MESSAGE(static_cast<int>(fvm::TimeScheme::Euler) == 0,
                  "Euler ordinal 0");
    CHECK_MESSAGE(static_cast<int>(fvm::TimeScheme::BDF2) == 1, "BDF2 ordinal 1");
}

// Steady: solver type SIMPLE, enabled=false, defaults match TransientConfig.
TEST_CASE("fvm.transient.steady_defaults") {
    cfd::CaseSettings settings; // defaults = steady
    CHECK_MESSAGE(!settings.transient.enabled, "default disabled");
    auto cfg = cfd::toFvmConfig<2>(settings);
    auto c = cfg.transient;
    CHECK_MESSAGE(c.enabled == false, "enabled maps");
    CHECK_NEAR(c.dt, 0.001);
    CHECK_NEAR(cfg.stopping.transient.tEnd, 1.0);
    CHECK_MESSAGE(c.timeScheme == fvm::TimeScheme::Euler, "BackwardEuler -> Euler");
    CHECK_MESSAGE(c.adaptiveCfl == false, "adaptive maps");
    CHECK_NEAR(c.cfl, 0.5);
    CHECK_NEAR(c.minDt, 1e-6);
    CHECK_NEAR(c.maxDt, 0.01);
    CHECK_MESSAGE(c.writeInterval == 1, "writeInterval maps");
    CHECK_MESSAGE(cfg.stopping.transient.maxOuterCorrectors == 3, "maxOuterCorrectors maps");
    CHECK_MESSAGE(c.pvdFile.empty(), "pvdFile maps (empty)");
    CHECK_MESSAGE(cfg.solverType == fvm::SolverType::SIMPLE, "steady -> SIMPLE");
}

// Transient: every field non-default, BDF2, solver type PISO.
TEST_CASE("fvm.transient.transient_mapping") {
    cfd::CaseSettings settings;
    auto& s = settings.transient;
    s.enabled = true;
    s.dt = 2.5e-4;
    s.timeScheme = fvm::TimeScheme::BDF2;
    s.adaptiveCfl = true;
    s.cfl = 0.3;
    s.minDt = 1e-7;
    s.maxDt = 5e-3;
    s.writeInterval = 7;
    s.pvdFile = "case.pvd";
    settings.stopping.transient.tEnd = 4.0;
    settings.stopping.transient.maxOuterCorrectors = 5;

    auto cfg = cfd::toFvmConfig<2>(settings);
    auto c = cfg.transient;
    CHECK_MESSAGE(c.enabled == true, "enabled maps");
    CHECK_NEAR(c.dt, 2.5e-4);
    CHECK_NEAR(cfg.stopping.transient.tEnd, 4.0);
    CHECK_MESSAGE(c.timeScheme == fvm::TimeScheme::BDF2, "BDF2 maps");
    CHECK_MESSAGE(c.adaptiveCfl == true, "adaptive maps");
    CHECK_NEAR(c.cfl, 0.3);
    CHECK_NEAR(c.minDt, 1e-7);
    CHECK_NEAR(c.maxDt, 5e-3);
    CHECK_MESSAGE(c.writeInterval == 7, "writeInterval maps");
    CHECK_MESSAGE(cfg.stopping.transient.maxOuterCorrectors == 5, "maxOuterCorrectors maps");
    CHECK_MESSAGE(c.pvdFile == "case.pvd", "pvdFile maps");
    CHECK_MESSAGE(cfg.solverType == fvm::SolverType::PISO, "transient -> PISO");
}

} // TEST_SUITE("fvm")
