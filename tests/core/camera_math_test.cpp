// Headless checks for animated camera transitions:
//   - core/camera_math.h  : pose interpolation (quat slerp) + easing
//   - Gizmo::footprintFor : footprint preset clamping
// Uses the real Camera class (plain GLM, no Qt/GL) to build snap poses so the
// tests exercise the exact poses production code produces. No Qt/GL.
#include "core/camera_math.h"
#include "core/Camera.h"
#include "doctest/doctest.h"
#include "render/overlays/gizmo.h"

#include <cmath>

namespace {

#define CHECK_NEAR(actual, expected, tol)                                                          \
    CHECK_MESSAGE(std::fabs(static_cast<double>(actual) - static_cast<double>(expected)) <= (tol), \
                  #actual " ~= " #expected " (actual=" << (actual) << ", expected=" << (expected) \
                               << ")")

double dist(const glm::dvec3& a, const glm::dvec3& b) { return glm::length(a - b); }

// Build the pose a user's snap would produce: apply the real preset math.
CameraPose poseForPreset(int preset, const Camera& from) {
    Camera c = from;
    c.snapToOrthoView(preset);
    return {c.position, c.focalPoint, c.viewUp};
}

// Interpolation invariants that must hold for ANY start/end pair we animate.
void checkTransitionInvariants(const char* label, const CameraPose& a, const CameraPose& b) {
    CAPTURE(label);
    const double distA = glm::length(a.focal - a.pos);
    const double distB = glm::length(b.focal - b.pos);

    // Endpoints exact.
    {
        const CameraPose s = interpolatePose(a, b, 0.0);
        CHECK_NEAR(dist(s.pos, a.pos), 0.0, 1e-6);
        CHECK_NEAR(dist(s.focal, a.focal), 0.0, 1e-6);
        const CameraPose e = interpolatePose(a, b, 1.0);
        CHECK_NEAR(dist(e.pos, b.pos), 0.0, 1e-6);
        CHECK_NEAR(dist(e.focal, b.focal), 0.0, 1e-6);
        CHECK_NEAR(dist(e.up, b.up), 1e-6, 1e-6); // up is re-derived; orthonormalized inputs match
    }

    // Along the path: orbit radius stays put, up never collapses into view dir.
    for (int i = 0; i <= 10; ++i) {
        const double t = i / 10.0;
        CAPTURE(t);
        const CameraPose p = interpolatePose(a, b, t);
        const double orbit = glm::length(p.focal - p.pos);
        CHECK_NEAR(orbit - distA, 0.0, 1e-6); // snap paths preserve distance
        CHECK_NEAR(orbit - distB, 0.0, 1e-6);
        const glm::dvec3 fwd = glm::normalize(p.focal - p.pos);
        const bool upUnit = glm::length(p.up) > 0.999;
        const bool upOrthogonal = std::fabs(glm::dot(glm::normalize(p.up), fwd)) < 1e-6;
        const bool finite = std::isfinite(p.pos.x) && std::isfinite(p.up.y);
        CHECK_MESSAGE(upUnit, "up must stay unit length");
        CHECK_MESSAGE(upOrthogonal, "up must not collapse into the view direction");
        CHECK_MESSAGE(finite, "pose must stay finite");
    }
}

} // namespace

TEST_SUITE("core") {

TEST_CASE("core.camera.easing") {
    CHECK_NEAR(easeInOutCubic(0.0), 0.0, 1e-6);
    CHECK_NEAR(easeInOutCubic(1.0), 1.0, 1e-6);
    CHECK_NEAR(easeInOutCubic(0.5), 0.5, 1e-6);
    // Symmetric ease: mirrored samples sum to one; monotone overall.
    CHECK_NEAR(easeInOutCubic(0.25) + easeInOutCubic(0.75), 1.0, 1e-6);
    double prev = -1e-9;
    for (int i = 0; i <= 20; ++i) {
        const double e = easeInOutCubic(i / 20.0);
        CAPTURE(i);
        CHECK_MESSAGE(e >= prev, "easing must be monotone");
        prev = e;
    }
    // Out-of-range inputs clamp instead of extrapolating.
    CHECK_NEAR(easeInOutCubic(-0.7), 0.0, 1e-6);
    CHECK_NEAR(easeInOutCubic(1.7), 1.0, 1e-6);
}

TEST_CASE("core.camera.snap_transitions") {
    Camera base;
    base.focalPoint = glm::dvec3(0.0, 0.0, 0.0);
    base.distance = 10.0;
    base.position = glm::dvec3(5.0, 6.5, 7.25); // generic diagonal vantage
    base.viewUp = glm::dvec3(0.0, 1.0, 0.0);
    base.orthogonalizeViewUp();

    struct Pair {
        int from;
        int to;
    };
    // Includes the degenerate-prone cases: vertical flip (+Z -> -Z with the
    // same world-up hint) and 180-degree azimuth swings (-Y -> +Y).
    static const Pair pairs[] = {
        {4, 0}, // +Z face -> +X face
        {4, 5}, // +Z -> -Z: naive viewUp lerp would collapse here
        {3, 2}, // -Y -> +Y: half-turn about Z
        {0, 1}, // +X -> -X
        {2, 5},
    };
    for (const Pair& pr : pairs) {
        CAPTURE(pr.from);
        CAPTURE(pr.to);
        const CameraPose a = poseForPreset(pr.from, base);
        const CameraPose b = poseForPreset(pr.to, base);
        checkTransitionInvariants("snap", a, b);

        // Mid-flight retarget: interpolating from a mid-sample of A->B toward C
        // must itself satisfy all invariants (the GUI does exactly this when a
        // second arrowhead is clicked during a transition).
        const CameraPose mid = interpolatePose(a, b, 0.37);
        const CameraPose c = poseForPreset((pr.to + 2) % 6, base);
        checkTransitionInvariants("retarget", mid, c);
    }
}

// Degenerate input guard: up parallel to view direction must not produce NaNs.
TEST_CASE("core.camera.degenerate_up") {
    CameraPose a;
    a.pos = glm::dvec3(0.0, 0.0, 10.0);
    a.focal = glm::dvec3(0.0, 0.0, 0.0);
    a.up = glm::dvec3(0.0, 0.0, 1.0); // parallel to view dir
    CameraPose b;
    b.pos = glm::dvec3(10.0, 0.0, 0.0);
    b.focal = glm::dvec3(0.0, 0.0, 0.0);
    b.up = glm::dvec3(0.0, 1.0, 0.0);
    const CameraPose p = interpolatePose(a, b, 0.5);
    const bool posFinite =
        std::isfinite(p.pos.x) && std::isfinite(p.pos.y) && std::isfinite(p.pos.z);
    const bool upFinite = std::isfinite(p.up.x) && std::isfinite(p.up.y) && std::isfinite(p.up.z);
    CHECK_MESSAGE(posFinite, "position must stay finite");
    CHECK_MESSAGE(upFinite, "up must stay finite");
    CHECK_NEAR(glm::length(p.up), 1.0, 1e-6);
}

TEST_CASE("core.camera.footprint_clamp") {
    CHECK_NEAR(Gizmo::footprintFor(0), 96.0, 1e-6);
    CHECK_NEAR(Gizmo::footprintFor(1), 128.0, 1e-6);
    CHECK_NEAR(Gizmo::footprintFor(2), 160.0, 1e-6);
    CHECK_NEAR(Gizmo::footprintFor(-3), 128.0, 1e-6); // out-of-range falls back to M
    CHECK_NEAR(Gizmo::footprintFor(99), 128.0, 1e-6);
}

} // TEST_SUITE("core")
