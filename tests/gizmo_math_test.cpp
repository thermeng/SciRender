// Headless checks for animated camera transitions:
//   - core/camera_math.h  : pose interpolation (quat slerp) + easing
//   - Gizmo::footprintFor : footprint preset clamping
// Uses the real Camera class (plain GLM, no Qt/GL) to build snap poses so the
// tests exercise the exact poses production code produces. No Qt/GL.
#include "core/camera_math.h"
#include "core/Camera.h"
#include "render/overlays/gizmo.h"

#include <cmath>
#include <cstdio>

static int g_failures = 0;

#define CHECK_NEAR(actual, expected) do { \
    if (std::fabs((actual) - (expected)) > 1e-6) { \
        std::printf("FAIL %s:%d: %s = %.9f, expected %.9f\n", __FILE__, __LINE__, \
                    #actual, static_cast<double>(actual), static_cast<double>(expected)); \
        ++g_failures; \
    } \
} while (0)

#define CHECK_TRUE(cond) do { \
    if (!(cond)) { \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

static double dist(const glm::dvec3& a, const glm::dvec3& b) { return glm::length(a - b); }

// Build the pose a user's snap would produce: apply the real preset math.
static CameraPose poseForPreset(int preset, const Camera& from) {
    Camera c = from;
    c.snapToOrthoView(preset);
    return { c.position, c.focalPoint, c.viewUp };
}

static void testEasing() {
    CHECK_NEAR(easeInOutCubic(0.0), 0.0);
    CHECK_NEAR(easeInOutCubic(1.0), 1.0);
    CHECK_NEAR(easeInOutCubic(0.5), 0.5);
    // Symmetric ease: mirrored samples sum to one; monotone overall.
    CHECK_NEAR(easeInOutCubic(0.25) + easeInOutCubic(0.75), 1.0);
    double prev = -1e-9;
    for (int i = 0; i <= 20; ++i) {
        const double e = easeInOutCubic(i / 20.0);
        CHECK_TRUE(e >= prev);
        prev = e;
    }
    // Out-of-range inputs clamp instead of extrapolating.
    CHECK_NEAR(easeInOutCubic(-0.7), 0.0);
    CHECK_NEAR(easeInOutCubic(1.7), 1.0);
}

// Interpolation invariants that must hold for ANY start/end pair we animate.
static void checkTransitionInvariants(const char* label, const CameraPose& a,
                                      const CameraPose& b, int presetB) {
    const double distA = glm::length(a.focal - a.pos);
    const double distB = glm::length(b.focal - b.pos);
    (void)presetB;

    // Endpoints exact.
    {
        const CameraPose s = interpolatePose(a, b, 0.0);
        CHECK_NEAR(dist(s.pos, a.pos), 0.0);
        CHECK_NEAR(dist(s.focal, a.focal), 0.0);
        const CameraPose e = interpolatePose(a, b, 1.0);
        CHECK_NEAR(dist(e.pos, b.pos), 0.0);
        CHECK_NEAR(dist(e.focal, b.focal), 0.0);
        CHECK_NEAR(dist(e.up, b.up), 1e-6);   // up is re-derived; orthonormalized inputs match
    }

    // Along the path: orbit radius stays put, up never collapses into view dir.
    for (int i = 0; i <= 10; ++i) {
        const double t = i / 10.0;
        const CameraPose p = interpolatePose(a, b, t);
        const double orbit = glm::length(p.focal - p.pos);
        CHECK_NEAR(orbit - distA, 0.0);       // snap paths preserve distance
        CHECK_NEAR(orbit - distB, 0.0);
        const glm::dvec3 fwd = glm::normalize(p.focal - p.pos);
        CHECK_TRUE(glm::length(p.up) > 0.999);
        CHECK_TRUE(std::fabs(glm::dot(glm::normalize(p.up), fwd)) < 1e-6);
        CHECK_TRUE(std::isfinite(p.pos.x) && std::isfinite(p.up.y));
    }
    (void)label;
}

static void testSnapTransitions() {
    Camera base;
    base.focalPoint = glm::dvec3(0.0, 0.0, 0.0);
    base.distance = 10.0;
    base.position = glm::dvec3(5.0, 6.5, 7.25);   // generic diagonal vantage
    base.viewUp = glm::dvec3(0.0, 1.0, 0.0);
    base.orthogonalizeViewUp();

    struct Pair { int from; int to; };
    // Includes the degenerate-prone cases: vertical flip (+Z -> -Z with the
    // same world-up hint) and 180-degree azimuth swings (-Y -> +Y).
    static const Pair pairs[] = {
        { 4, 0 },  // +Z face -> +X face
        { 4, 5 },  // +Z -> -Z: naive viewUp lerp would collapse here
        { 3, 2 },  // -Y -> +Y: half-turn about Z
        { 0, 1 },  // +X -> -X
        { 2, 5 },
    };
    for (const Pair& pr : pairs) {
        const CameraPose a = poseForPreset(pr.from, base);
        const CameraPose b = poseForPreset(pr.to, base);
        checkTransitionInvariants("snap", a, b, pr.to);

        // Mid-flight retarget: interpolating from a mid-sample of A->B toward C
        // must itself satisfy all invariants (the GUI does exactly this when a
        // second arrowhead is clicked during a transition).
        const CameraPose mid = interpolatePose(a, b, 0.37);
        const CameraPose c = poseForPreset((pr.to + 2) % 6, base);
        checkTransitionInvariants("retarget", mid, c, (pr.to + 2) % 6);
    }
}

// Degenerate input guard: up parallel to view direction must not produce NaNs.
static void testDegenerateUp() {
    CameraPose a;
    a.pos = glm::dvec3(0.0, 0.0, 10.0);
    a.focal = glm::dvec3(0.0, 0.0, 0.0);
    a.up = glm::dvec3(0.0, 0.0, 1.0);            // parallel to view dir
    CameraPose b;
    b.pos = glm::dvec3(10.0, 0.0, 0.0);
    b.focal = glm::dvec3(0.0, 0.0, 0.0);
    b.up = glm::dvec3(0.0, 1.0, 0.0);
    const CameraPose p = interpolatePose(a, b, 0.5);
    CHECK_TRUE(std::isfinite(p.pos.x) && std::isfinite(p.pos.y) && std::isfinite(p.pos.z));
    CHECK_TRUE(std::isfinite(p.up.x) && std::isfinite(p.up.y) && std::isfinite(p.up.z));
    CHECK_NEAR(glm::length(p.up), 1.0);
}

static void testFootprintClamp() {
    CHECK_NEAR(Gizmo::footprintFor(0), 96.0);
    CHECK_NEAR(Gizmo::footprintFor(1), 128.0);
    CHECK_NEAR(Gizmo::footprintFor(2), 160.0);
    CHECK_NEAR(Gizmo::footprintFor(-3), 128.0);  // out-of-range falls back to M
    CHECK_NEAR(Gizmo::footprintFor(99), 128.0);
}

int main() {
    testEasing();
    testSnapTransitions();
    testDegenerateUp();
    testFootprintClamp();
    if (g_failures == 0) {
        std::printf("gizmo_math_test: ALL PASS\n");
        return 0;
    }
    std::printf("gizmo_math_test: %d CHECK(S) FAILED\n", g_failures);
    return 1;
}
