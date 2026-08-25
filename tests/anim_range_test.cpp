// Headless check for FieldResolver::AnimRangeState — the animation colormap
// range rules (whole-sequence union vs per-frame rescale). No Qt/GL.
#include "core/FieldResolver.h"

#include <cmath>
#include <cstdio>

static int g_failures = 0;

#define CHECK_NEAR(actual, expected) do { \
    if (std::fabs((actual) - (expected)) > 1e-5f) { \
        std::printf("FAIL %s:%d: %s = %f, expected %f\n", __FILE__, __LINE__, \
                    #actual, static_cast<double>(actual), static_cast<double>(expected)); \
        ++g_failures; \
    } \
} while (0)

using FieldResolver::AnimRangeState;

// Whole-sequence mode: expand-only union across frames of one field.
static void testGlobalUnion() {
    AnimRangeState s;
    s.global = true;
    float lo, hi;
    s.advance(true, "pressure", 1.f, 2.f, lo, hi);
    CHECK_NEAR(lo, 1.f); CHECK_NEAR(hi, 2.f);
    s.advance(false, "pressure", 0.f, 3.f, lo, hi);   // wider → expands both ways
    CHECK_NEAR(lo, 0.f); CHECK_NEAR(hi, 3.f);
    s.advance(false, "pressure", 1.f, 2.f, lo, hi);   // narrower → union holds
    CHECK_NEAR(lo, 0.f); CHECK_NEAR(hi, 3.f);
}

// Mid-sequence field switch must RESEED, not expand the old field's union
// (the bug this regression-guards: selected field normalized against another
// field's accumulated range).
static void testFieldSwitchReseeds() {
    AnimRangeState s;
    float lo, hi;
    s.advance(true, "pressure", 10.f, 20.f, lo, hi);
    s.advance(false, "pressure", 8.f, 25.f, lo, hi);
    CHECK_NEAR(lo, 8.f); CHECK_NEAR(hi, 25.f);
    s.advance(false, "velocity_mag", 0.f, 1.f, lo, hi);   // different field
    CHECK_NEAR(lo, 0.f); CHECK_NEAR(hi, 1.f);
    s.advance(false, "velocity_mag", -0.5f, 2.f, lo, hi); // then unions within new field
    CHECK_NEAR(lo, -0.5f); CHECK_NEAR(hi, 2.f);
}

// Per-frame mode: every frame uses exactly its own extent.
static void testPerFramePassthrough() {
    AnimRangeState s;
    s.global = false;
    float lo, hi;
    s.advance(true, "pressure", 1.f, 2.f, lo, hi);
    CHECK_NEAR(lo, 1.f); CHECK_NEAR(hi, 2.f);
    s.advance(false, "pressure", 100.f, 200.f, lo, hi);
    CHECK_NEAR(lo, 100.f); CHECK_NEAR(hi, 200.f);
}

// Mode switch invalidates the accumulator so the next global frame reseeds
// instead of expanding a stale union from the other mode.
static void testInvalidateForcesReseed() {
    AnimRangeState s;
    float lo, hi;
    s.advance(true, "pressure", 1.f, 2.f, lo, hi);
    s.advance(false, "pressure", 0.f, 50.f, lo, hi);
    s.global = false;
    s.advance(false, "pressure", 7.f, 9.f, lo, hi);
    CHECK_NEAR(lo, 7.f); CHECK_NEAR(hi, 9.f);
    s.global = true;
    s.invalidate();
    s.advance(false, "pressure", 7.f, 9.f, lo, hi);   // same field name!
    CHECK_NEAR(lo, 7.f); CHECK_NEAR(hi, 9.f);
}

// A restarted sequence (new load) reseeds even in global mode.
static void testRestartReseeds() {
    AnimRangeState s;
    float lo, hi;
    s.advance(true, "pressure", 0.f, 100.f, lo, hi);
    s.advance(false, "pressure", -10.f, 110.f, lo, hi);
    s.advance(true, "temperature", 300.f, 400.f, lo, hi);   // fresh sequence
    CHECK_NEAR(lo, 300.f); CHECK_NEAR(hi, 400.f);
}

int main() {
    testGlobalUnion();
    testFieldSwitchReseeds();
    testPerFramePassthrough();
    testInvalidateForcesReseed();
    testRestartReseeds();
    if (g_failures == 0) {
        std::printf("anim_range_test: all checks passed\n");
        return 0;
    }
    std::printf("anim_range_test: %d failure(s)\n", g_failures);
    return 1;
}
