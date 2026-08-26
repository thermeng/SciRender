#include "core/camera_math.h"

#include <algorithm>

double easeInOutCubic(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t < 0.5 ? 4.0 * t * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
}

namespace {

// Rotation taking OpenGL camera space (looks down -Z, up +Y) into world space
// such that the camera's forward axis lands on `forward` with the nearest
// orientation to `upHint`. Matches the gluLookAt basis construction.
glm::dquat frameQuat(const glm::dvec3& forward, const glm::dvec3& upHint) {
    const glm::dvec3 f = glm::normalize(forward);
    glm::dvec3 s = glm::cross(f, upHint);
    double sLen = glm::length(s);
    if (sLen < 1e-9) {
        // upHint parallel to view direction: pick any fallback perpendicular.
        const glm::dvec3 alt = (std::abs(f.y) < 0.9) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
        s = glm::cross(f, alt);
        sLen = glm::length(s);
    }
    s /= sLen;
    const glm::dvec3 u = glm::normalize(glm::cross(s, f));
    return glm::dquat(glm::dmat3(s, u, -f));  // columns: right, up, backward
}

}  // namespace

CameraPose interpolatePose(const CameraPose& a, const CameraPose& b, double t) {
    t = std::clamp(t, 0.0, 1.0);

    const glm::dquat qa = frameQuat(a.focal - a.pos, a.up);
    const glm::dquat qb = frameQuat(b.focal - b.pos, b.up);
    const glm::dquat q = glm::slerp(qa, qb, t);  // shortest-arc

    CameraPose out;
    out.focal = glm::mix(a.focal, b.focal, t);
    // Blend orbit radii separately: identical for snap transitions (distance is
    // preserved), still sensible if a future caller animates a dolly.
    const double dist = glm::mix(glm::length(a.focal - a.pos), glm::length(b.focal - b.pos), t);
    out.pos = out.focal - (q * glm::dvec3(0, 0, -1)) * dist;
    out.up = q * glm::dvec3(0, 1, 0);
    return out;
}
