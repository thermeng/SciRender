#include "LightingModel.h"

#include <cmath>

namespace {
    // kit-wide warm tint shared by light colors + viewport markers.
    // 0 = cold blue, 0.5 = neutral white, 1 = warm red.
    glm::vec3 warmColorTint(float w) {
        if (w < 0.5f) return glm::mix(glm::vec3(0.6f, 0.7f, 1.0f), glm::vec3(1.0f, 1.0f, 1.0f), w / 0.5f);
        return glm::mix(glm::vec3(1.0f, 1.0f, 1.0f), glm::vec3(1.0f, 0.85f, 0.7f), (w - 0.5f) / 0.5f);
    }
}

glm::vec3 LightingModel::kitDirection(float az, float el) {
    float a = az * 3.14159265f / 180.0f;
    float e = el * 3.14159265f / 180.0f;
    return glm::vec3(std::sin(a) * std::cos(e), std::sin(e), std::cos(a) * std::cos(e));
}

void LightingModel::computeDirections(const glm::dvec3& camPos,
                                      const glm::dvec3& camFocal,
                                      const glm::dvec3& camUp,
                                      glm::vec3& key, glm::vec3& fill,
                                      glm::vec3& back1, glm::vec3& back2,
                                      glm::vec3& head) const {
    glm::dvec3 fwd   = glm::normalize(camPos - camFocal); // toward camera
    glm::dvec3 right = glm::normalize(glm::cross(fwd, camUp));
    glm::dvec3 up    = glm::cross(right, fwd);
    glm::mat3 M(right, up, fwd); // kit X->right, Y->up, Z->toward camera
    auto kitDir = [&](float az, float el) -> glm::vec3 {
        return glm::vec3(glm::normalize(M * kitDirection(az, el)));
    };
    key  = kitDir(lightKeyAzimuth,  lightKeyElevation);
    fill = kitDir(lightFillAzimuth, lightFillElevation);
    back1 = kitDir(lightBackAzimuth,  lightBackElevation);
    back2 = kitDir(lightBackAzimuth + 180.0f, -lightBackElevation);
    head = kitDir(lightHeadAzimuth,  lightHeadElevation);
}

void LightingModel::applyPreset(int preset) {
    switch (preset) {
    case PRESET_STUDIO: // 3-point feel: strong key, soft fill, rim back, subtle head
        lightKeyAzimuth = 35.0f;   lightKeyElevation = 45.0f;   lightKF = 4.0f;
        lightFillAzimuth = -45.0f; lightFillElevation = 20.0f;   lightKB = 1.5f;
        lightBackAzimuth = 140.0f; lightBackElevation = 30.0f;
        lightHeadAzimuth = 0.0f;   lightHeadElevation = 0.0f;   lightKH = 0.5f;
        lightKeyIntensity = 1.0f;  lightWarm = 0.5f;
        matAmbient = 0.10f; matDiffuse = 0.78f; matSpecular = 0.25f; matShininess = 0.6f;
        break;

    case PRESET_CADFLAT: // even, shadowless look for inspecting geometry
        lightKeyAzimuth = 0.0f;   lightKeyElevation = 45.0f;  lightKF = 1.2f;
        lightFillAzimuth = 180.0f; lightFillElevation = 45.0f; lightKB = 1.2f;
        lightBackAzimuth = 90.0f;  lightBackElevation = 45.0f;
        lightHeadAzimuth = 0.0f;   lightHeadElevation = 0.0f;  lightKH = 1.0f;
        lightKeyIntensity = 1.0f;  lightWarm = 0.5f;
        matAmbient = 0.45f; matDiffuse = 0.8f; matSpecular = 0.0f; matShininess = 0.0f;
        break;

    case PRESET_SOFT: // gentle, low-contrast, warm
    default:
        lightKeyAzimuth = 20.0f;   lightKeyElevation = 40.0f;  lightKF = 2.5f;
        lightFillAzimuth = -30.0f; lightFillElevation = 15.0f; lightKB = 1.8f;
        lightBackAzimuth = 120.0f; lightBackElevation = 25.0f;
        lightHeadAzimuth = 0.0f;   lightHeadElevation = 0.0f;  lightKH = 0.8f;
        lightKeyIntensity = 1.0f;  lightWarm = 0.6f;
        matAmbient = 0.20f; matDiffuse = 0.7f; matSpecular = 0.08f; matShininess = 0.3f;
        break;
    }
}

void LightingModel::reset() {
    // Default Light Kit configuration.
    lightKitEnabled = true;
    lightKeyIntensity = 0.5f;  lightWarm = 0.5f;
    lightKF = 3.0f; lightKB = 3.5f; lightKH = 3.0f;
    lightKeyAzimuth = 10.0f;  lightKeyElevation = 50.0f;
    lightFillAzimuth = -10.0f; lightFillElevation = -75.0f;
    lightBackAzimuth = 110.0f; lightBackElevation = 0.0f;
    lightHeadAzimuth = 0.0f;   lightHeadElevation = 0.0f;
    matAmbient = 0.08f; matDiffuse = 0.75f; matSpecular = 0.15f; matShininess = 0.5f;
}

// Exposed for the gizmo light-marker overlay: kit-local dirs tinted warm.
inline glm::vec3 warmTintForMarkers(float warm) { return warmColorTint(warm); }
