#pragma once

#include <glm/glm.hpp>

// Owns the 4-point Light Kit parameter set, material registers, and the
// camera-relative light-direction computation. Pure data + math; holds no GL
// resources and emits no Qt signals so it can be unit-tested in isolation.
class LightingModel {
public:
    static constexpr int PRESET_STUDIO = 0;
    static constexpr int PRESET_CADFLAT = 1;
    static constexpr int PRESET_SOFT = 2;

    // 4-point light kit directions (azimuth/elevation in degrees, about the
    // camera's look-at point so the lights track the view).
    float lightKF = 3.0f;
    float lightKB = 3.5f;
    float lightKH = 3.0f;
    float lightKeyAzimuth = 10.0f;
    float lightKeyElevation = 50.0f;
    float lightFillAzimuth = -10.0f;
    float lightFillElevation = -75.0f;
    float lightBackAzimuth = 110.0f;
    float lightBackElevation = 0.0f;
    float lightHeadAzimuth = 0.0f;
    float lightHeadElevation = 0.0f;

    // Light Kit state
    bool  lightKitEnabled = true;
    bool  showLightMarkers = false; // triad light-marker overlay (default off)
    float lightKeyIntensity = 1.0f;  // "Int" — key intensity (0..1)
    float lightWarm = 0.5f;          // kit-wide warm tint (0 cold .. 0.5 neutral .. 1 warm)

    float matAmbient = 0.08f;
    float matDiffuse = 0.75f;
    float matSpecular = 0.15f;
    float matShininess = 16.0f;

    // Computes the five kit light directions in WORLD space given the camera
    // basis. Lights live in the camera frame and are rotated into world space,
    // so they move with the view (headlight stays head-on, key stays overhead).
    void computeDirections(const glm::dvec3& camPos,
                           const glm::dvec3& camFocal,
                           const glm::dvec3& camUp,
                           glm::vec3& key, glm::vec3& fill,
                           glm::vec3& back1, glm::vec3& back2,
                           glm::vec3& head) const;

    // Kit-local direction for a single (azimuth, elevation) light, in the
    // camera frame. Exposed so the gizmo overlay can draw the markers.
    static glm::vec3 kitDirection(float az, float el);

    void applyPreset(int preset);
    void reset();
};
