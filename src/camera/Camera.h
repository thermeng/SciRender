#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

class Camera {
public:
    Camera();
    ~Camera() = default;

    // Camera state vectors (Using double precision matching VTK spec)
    glm::dvec3 position;
    glm::dvec3 focalPoint;
    glm::dvec3 viewUp;
    double distance;

    // Core Transformation Methods
    void azimuth(double angle);
    void elevation(double angle);
    void roll(double angle);
    void pan(double dx, double dy);
    void dolly(double factor);

    // Utilities & Space Mappings
    void orthogonalizeViewUp();
    void snapToOrthoView(int axis);
    void updateDistance();

    glm::dvec3 directionOfProjection() const;
    glm::mat4 getViewMatrix() const;
};