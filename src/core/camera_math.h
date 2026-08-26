#pragma once
// Pure camera-pose interpolation for animated view transitions (fly-to-face).
// GLM-only: no Qt, no OpenGL — safe to compile into unit tests.
//
// Poses are interpolated as (orientation, focal point) pairs: orientation via
// quaternion slerp of look-at frames, focal linearly. Camera position is then
// reconstructed along the slerped forward axis at the blended orbit distance,
// which keeps |pos - focal| pulse-free and never lets the up vector collapse
// into the view direction mid-flight (the classic failure of lerping viewUp).

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

struct CameraPose {
    glm::dvec3 pos;
    glm::dvec3 focal;
    glm::dvec3 up;
};

// Smoothstep-style ease with zero first derivatives at both ends (C1).
double easeInOutCubic(double t);

// Interpolate between two poses at parameter t in [0,1]. Endpoints are exact
// when both inputs carry an orthonormalized up vector perpendicular to their
// view direction (Camera maintains this invariant via orthogonalizeViewUp()).
CameraPose interpolatePose(const CameraPose& a, const CameraPose& b, double t);
