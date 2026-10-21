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
    CHECK_MESSAGE(static_cast<int>(cfd::TransientSettings::TimeScheme::BackwardEuler) == 0,
                  "BackwardEuler ordinal 0");
    CHECK_MESSAGE(static_cast<int>(cfd::TransientSettings::TimeScheme::BDF2) == 1, "BDF2 ordinal 1");
}

// Steady: solver type SIMPLE, enabled=false, defaults match TransientConfig.
TEST_CASE("fvm.transient.steady_defaults") {
    cfd::TransientSettings s; // defaults = steady
    CHECK_MESSAGE(!s.enabled, "default disabled");
    auto c = cfd::toFvmTransient<2>(s);
    CHECK_MESSAGE(c.enabled == false, "enabled maps");
    CHECK_NEAR(c.dt, 0.001);
    CHECK_NEAR(c.tEnd, 1.0);
    CHECK_MESSAGE(c.scheme == fvm::TimeScheme::Euler, "BackwardEuler -> Euler");
    CHECK_MESSAGE(c.adaptive == false, "adaptive maps");
    CHECK_NEAR(c.cfl, 0.5);
    CHECK_NEAR(c.minDt, 1e-6);
    CHECK_NEAR(c.maxDt, 0.01);
    CHECK_MESSAGE(c.writeInterval == 1, "writeInterval maps");
    CHECK_MESSAGE(c.maxOuterCorrectors == 3, "maxOuterCorrectors maps");
    CHECK_MESSAGE(c.pvdFile.empty(), "pvdFile maps (empty)");
    CHECK_MESSAGE(cfd::toFvmSolverType(s) == fvm::SolverType::SIMPLE, "steady -> SIMPLE");
}

// Transient: every field non-default, BDF2, solver type PISO.
TEST_CASE("fvm.transient.transient_mapping") {
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
    CHECK_MESSAGE(c.enabled == true, "enabled maps");
    CHECK_NEAR(c.dt, 2.5e-4);
    CHECK_NEAR(c.tEnd, 4.0);
    CHECK_MESSAGE(c.scheme == fvm::TimeScheme::BDF2, "BDF2 maps");
    CHECK_MESSAGE(c.adaptive == true, "adaptive maps");
    CHECK_NEAR(c.cfl, 0.3);
    CHECK_NEAR(c.minDt, 1e-7);
    CHECK_NEAR(c.maxDt, 5e-3);
    CHECK_MESSAGE(c.writeInterval == 7, "writeInterval maps");
    CHECK_MESSAGE(c.maxOuterCorrectors == 5, "maxOuterCorrectors maps");
    CHECK_MESSAGE(c.pvdFile == "case.pvd", "pvdFile maps");
    CHECK_MESSAGE(cfd::toFvmSolverType(s) == fvm::SolverType::PISO, "transient -> PISO");
}

} // TEST_SUITE("fvm")
