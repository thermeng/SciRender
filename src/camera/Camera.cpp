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

    glm::dmat4 transform = glm::translate(glm::dmat4(1.0), focalPoint)
        * glm::rotate(glm::dmat4(1.0), glm::radians(angle), -right)
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
    right = -glm::normalize(right);

    glm::dvec3 up = glm::normalize(glm::cross(right, dir));

    glm::dvec3 motion = right * dx * panSpeed + up * dy * panSpeed;
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

glm::quat Camera::computeGizmoQuat() const {
    glm::dvec3 forward = directionOfProjection();
    glm::dvec3 right = glm::cross(forward, viewUp);
    if (glm::length(right) < 0.001) right = glm::dvec3(1.0, 0.0, 0.0);
    right = glm::normalize(right);
    glm::dvec3 realUp = glm::cross(right, forward);

    glm::dmat4 camOrient(1.0);
    camOrient[0] = glm::dvec4(right, 0.0);
    camOrient[1] = glm::dvec4(realUp, 0.0);
    camOrient[2] = glm::dvec4(-forward, 0.0);

    return glm::quat(glm::inverse(glm::quat_cast(camOrient)));
}

void Camera::snapToOrthoView(int axis) {
    switch (axis) {
    case 0: // Right
        position = { focalPoint.x + distance, focalPoint.y, focalPoint.z };
        viewUp = { 0.0, 1.0, 0.0 };
        break;
    case 1: // Top
        position = { focalPoint.x, focalPoint.y + distance, focalPoint.z };
        viewUp = { 0.0, 0.0, -1.0 };
        break;
    case 2: // Front
        position = { focalPoint.x, focalPoint.y, focalPoint.z + distance };
        viewUp = { 0.0, 1.0, 0.0 };
        break;
    }
}

glm::mat4 Camera::getViewMatrix() const {
    return glm::lookAt(glm::vec3(position), glm::vec3(focalPoint), glm::vec3(viewUp));
}

glm::mat4 Camera::getProjectionMatrix(double aspectRatio, double nearPlane, double farPlane) const {
    // 45.0f is a standard field-of-view value. Adjust this if you track FOV state variables.
    return glm::perspective(glm::radians(45.0f), static_cast<float>(aspectRatio), static_cast<float>(nearPlane), static_cast<float>(farPlane));
}