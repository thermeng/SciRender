// Headless check for FieldResolver::AnimRangeState - the animation colormap
// range rules (whole-sequence union vs per-frame rescale). No Qt/GL.
#include "core/FieldResolver.h"
#include "doctest/doctest.h"

#include <cmath>

using FieldResolver::AnimRangeState;

namespace {

#define CHECK_NEAR(actual, expected, tol)                                                          \
    CHECK_MESSAGE(std::fabs(static_cast<double>(actual) - static_cast<double>(expected)) <= (tol), \
                  #actual " ~= " #expected " (actual=" << (actual) << ", expected=" << (expected) \
                               << ")")

} // namespace

TEST_SUITE("core") {

// Whole-sequence mode: expand-only union across frames of one field.
TEST_CASE("core.anim_range.global_union") {
    AnimRangeState s;
    s.global = true;
    float lo, hi;
    s.advance(true, "pressure", 1.f, 2.f, lo, hi);
    CHECK_NEAR(lo, 1.f, 1e-5f);
    CHECK_NEAR(hi, 2.f, 1e-5f);
    s.advance(false, "pressure", 0.f, 3.f, lo, hi); // wider -> expands both ways
    CHECK_NEAR(lo, 0.f, 1e-5f);
    CHECK_NEAR(hi, 3.f, 1e-5f);
    s.advance(false, "pressure", 1.f, 2.f, lo, hi); // narrower -> union holds
    CHECK_NEAR(lo, 0.f, 1e-5f);
    CHECK_NEAR(hi, 3.f, 1e-5f);
}

// Mid-sequence field switch must RESEED, not expand the old field's union
// (the bug this regression-guards: selected field normalized against another
// field's accumulated range).
TEST_CASE("core.anim_range.field_switch_reseeds") {
    AnimRangeState s;
    float lo, hi;
    s.advance(true, "pressure", 10.f, 20.f, lo, hi);
    s.advance(false, "pressure", 8.f, 25.f, lo, hi);
    CHECK_NEAR(lo, 8.f, 1e-5f);
    CHECK_NEAR(hi, 25.f, 1e-5f);
    s.advance(false, "velocity_mag", 0.f, 1.f, lo, hi); // different field
    CHECK_NEAR(lo, 0.f, 1e-5f);
    CHECK_NEAR(hi, 1.f, 1e-5f);
    s.advance(false, "velocity_mag", -0.5f, 2.f, lo, hi); // then unions within new field
    CHECK_NEAR(lo, -0.5f, 1e-5f);
    CHECK_NEAR(hi, 2.f, 1e-5f);
}

// Per-frame mode: every frame uses exactly its own extent.
TEST_CASE("core.anim_range.per_frame_passthrough") {
    AnimRangeState s;
    s.global = false;
    float lo, hi;
    s.advance(true, "pressure", 1.f, 2.f, lo, hi);
    CHECK_NEAR(lo, 1.f, 1e-5f);
    CHECK_NEAR(hi, 2.f, 1e-5f);
    s.advance(false, "pressure", 100.f, 200.f, lo, hi);
    CHECK_NEAR(lo, 100.f, 1e-5f);
    CHECK_NEAR(hi, 200.f, 1e-5f);
}

// Mode switch invalidates the accumulator so the next global frame reseeds
// instead of expanding a stale union from the other mode.
TEST_CASE("core.anim_range.invalidate_forces_reseed") {
    AnimRangeState s;
    float lo, hi;
    s.advance(true, "pressure", 1.f, 2.f, lo, hi);
    s.advance(false, "pressure", 0.f, 50.f, lo, hi);
    s.global = false;
    s.advance(false, "pressure", 7.f, 9.f, lo, hi);
    CHECK_NEAR(lo, 7.f, 1e-5f);
    CHECK_NEAR(hi, 9.f, 1e-5f);
    s.global = true;
    s.invalidate();
    s.advance(false, "pressure", 7.f, 9.f, lo, hi); // same field name!
    CHECK_NEAR(lo, 7.f, 1e-5f);
    CHECK_NEAR(hi, 9.f, 1e-5f);
}

// A restarted sequence (new load) reseeds even in global mode.
TEST_CASE("core.anim_range.restart_reseeds") {
    AnimRangeState s;
    float lo, hi;
    s.advance(true, "pressure", 0.f, 100.f, lo, hi);
    s.advance(false, "pressure", -10.f, 110.f, lo, hi);
    s.advance(true, "temperature", 300.f, 400.f, lo, hi); // fresh sequence
    CHECK_NEAR(lo, 300.f, 1e-5f);
    CHECK_NEAR(hi, 400.f, 1e-5f);
}

} // TEST_SUITE("core")
