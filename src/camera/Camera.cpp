#include "Camera.h"
#include <algorithm> // For std::clamp

Camera::Camera()
    : position(0.0, 0.0, 3.0),
    focalPoint(0.0, 0.0, 0.0),
    viewUp(0.0, 1.0, 0.0),
    distance(3.0) {
}

void Camera::azimuth(double angle) {
    glm::dmat4 transform = glm::translate(glm::dmat4(1.0), focalPoint)
        * glm::rotate(glm::dmat4(1.0), glm::radians(angle), viewUp)
        * glm::translate(glm::dmat4(1.0), -focalPoint);

    position = glm::dvec3(transform * glm::dvec4(position, 1.0));
}

void Camera::elevation(double angle) {
    glm::dvec3 dir = directionOfProjection();
    glm::dvec3 right = glm::cross(viewUp, dir);

    if (glm::length(right) < 0.001) {
        right = glm::dvec3(1.0, 0.0, 0.0);
    }
    else {
        right = glm::normalize(right);
    }

    // VTK rotates about +right (= cross(viewUp, directionOfProjection)); the
    // negation here previously inverted the elevation sign relative to VTK.
    glm::dmat4 transform = glm::translate(glm::dmat4(1.0), focalPoint)
        * glm::rotate(glm::dmat4(1.0), glm::radians(angle), right)
        * glm::translate(glm::dmat4(1.0), -focalPoint);

    position = glm::dvec3(transform * glm::dvec4(position, 1.0));

    viewUp = glm::normalize(glm::dvec3(transform * glm::dvec4(viewUp, 0.0)));
}

void Camera::roll(double angle) {
    glm::dvec3 dir = directionOfProjection();
    glm::dmat4 rot = glm::rotate(glm::dmat4(1.0), glm::radians(angle), dir);
    viewUp = glm::normalize(glm::dvec3(rot * glm::dvec4(viewUp, 0.0)));
}

void Camera::orthogonalizeViewUp() {
    glm::dvec3 dir = directionOfProjection();
    glm::dvec3 side = glm::cross(dir, viewUp);
    if (glm::length(side) < 0.001) return; // Prevent collapse at poles
    side = glm::normalize(side);
    viewUp = glm::normalize(glm::cross(side, dir));
}

void Camera::pan(double dx, double dy) {
    double panSpeed = distance * 0.002;
    glm::dvec3 dir = directionOfProjection();
    glm::dvec3 right = glm::cross(dir, viewUp);
    if (glm::length(right) < 0.001) right = glm::dvec3(1.0, 0.0, 0.0);
    right = glm::normalize(right);
    glm::dvec3 up = glm::normalize(glm::cross(right, dir)); // true up
    // Grab-style: moving the mouse right should drag the scene right (negate X).
    // Vertical must use the true up so it tracks the horizontal axis consistently.
    glm::dvec3 motion = (-right) * dx * panSpeed + up * dy * panSpeed;
    focalPoint += motion;
    position += motion;
}

void Camera::dolly(double factor) {
    if (factor <= 0.0) factor = 0.001; // Guard against division by zero/negative zoom
    double newDist = distance / factor;
    glm::dvec3 dir = directionOfProjection();
    position = focalPoint - dir * newDist;

    updateDistance();
}

void Camera::updateDistance() {
    distance = glm::length(focalPoint - position);
}

glm::dvec3 Camera::directionOfProjection() const {
    return glm::normalize(focalPoint - position);
}

void Camera::snapToOrthoView(int axis) {
    // ParaView/VTK axial presets: the SAME world axis stays "up" for each opposite pair,
    // so +X/-X, +Y/-Y, +Z/-Z only differ by azimuth (a horizontal mirror), never a
    // vertical flip. This keeps the reference axis always pointing up, like ParaView.
    //   X / Y faces -> world +Z is up;  Z face -> world +Y is up.
    glm::dvec3 target = focalPoint;
    glm::dvec3 up(0.0, 1.0, 0.0);
    switch (axis) {
    case 0: target.x += distance; up = { 0.0, 0.0,  1.0 }; break;  // +X
    case 1: target.x -= distance; up = { 0.0, 0.0,  1.0 }; break;  // -X
    case 2: target.y += distance; up = { 0.0, 0.0,  1.0 }; break;  // +Y
    case 3: target.y -= distance; up = { 0.0, 0.0,  1.0 }; break;  // -Y
    case 4: target.z += distance; up = { 0.0, 1.0,  0.0 }; break;  // +Z
    case 5: target.z -= distance; up = { 0.0, 1.0,  0.0 }; break;  // -Z
    default: return; // Unknown axis: leave camera untouched
    }
    position = target;
    viewUp = up;
    orthogonalizeViewUp();
}

glm::mat4 Camera::getViewMatrix() const {
    return glm::lookAt(glm::vec3(position), glm::vec3(focalPoint), glm::vec3(viewUp));
}