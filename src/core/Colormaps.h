#pragma once
#include <glm/glm.hpp>
#include <cmath>

enum class ColormapType {
    Turbo = 0,         // Google's Engineering Rainbow: High-contrast replacement for Jet/Rainbow
    Viridis = 1,       // CFD/FEA Default: Perceptually uniform scalar magnitude (Stress, Speed)
    Inferno = 2,       // Sequential Thermal: Excellent for localized energy dissipation/hot-spots
    CoolWarm = 3,      // Balanced Diverging: Smooth sign-variant fields (e.g., pressure differentials)
    MutedCoolWarm = 4, // 3D Mesh-Friendly: Prevents whiteout, preserves surface lighting/shading
    BWR = 5,           // Classical Diverging: Explicit Blue-White-Red mapping (Tension/Compression)
    Cividis = 6,       // Print & Colorblind Safe: Matches strict engineering publication standards
    Grayscale = 7,     // Shading Baseline: Verifies geometry visibility and light interaction

    Count
};

namespace Colormaps {

    inline const char* getName(ColormapType type) {
        switch (type) {
        case ColormapType::Viridis:   return "Viridis";
        case ColormapType::Inferno:   return "Inferno";
        case ColormapType::Turbo:     return "Turbo";
        case ColormapType::CoolWarm:  return "CoolWarm";
        case ColormapType::MutedCoolWarm: return "Muted CoolWarm";
        case ColormapType::BWR:       return "BWR";
        case ColormapType::Cividis:   return "Cividis";
        case ColormapType::Grayscale: return "Grayscale";
        default:                      return "Unknown";
        }
    }

    inline glm::vec3 viridis(float t) {
        float clampedT = glm::clamp(t, 0.0f, 1.0f);
        glm::vec3 c0 = glm::vec3(0.267f, 0.004f, 0.329f);
        glm::vec3 c1 = glm::vec3(0.224f, 0.463f, 0.557f);
        glm::vec3 c2 = glm::vec3(0.122f, 0.733f, 0.494f);
        glm::vec3 c3 = glm::vec3(0.992f, 0.906f, 0.144f);

        if (clampedT < 0.33f)      return glm::mix(c0, c1, clampedT / 0.33f);
        else if (clampedT < 0.66f) return glm::mix(c1, c2, (clampedT - 0.33f) / 0.33f);
        else                       return glm::mix(c2, c3, (clampedT - 0.66f) / 0.34f);
    }

    inline glm::vec3 inferno(float t) {
        float clampedT = glm::clamp(t, 0.0f, 1.0f);
        glm::vec3 c0 = glm::vec3(0.000f, 0.000f, 0.004f);
        glm::vec3 c1 = glm::vec3(0.475f, 0.102f, 0.353f);
        glm::vec3 c2 = glm::vec3(0.914f, 0.349f, 0.086f);
        glm::vec3 c3 = glm::vec3(0.988f, 0.906f, 0.655f);

        if (clampedT < 0.33f)      return glm::mix(c0, c1, clampedT / 0.33f);
        else if (clampedT < 0.66f) return glm::mix(c1, c2, (clampedT - 0.33f) / 0.33f);
        else                       return glm::mix(c2, c3, (clampedT - 0.66f) / 0.34f);
    }

    // Google Turbo: Smooth, continuous polynomial approximation avoiding Jet's false gradients
    inline glm::vec3 turbo(float t) {
        float clampedT = glm::clamp(t, 0.0f, 1.0f);

        // Accurate coefficients derived directly from Google's Open-Source specification
        glm::vec4 kRedVec4 = glm::vec4(0.13572138f, 4.61539260f, -42.66032258f, 132.13108234f);
        glm::vec4 kGreenVec4 = glm::vec4(0.09140261f, 2.19418839f, 4.84296658f, -14.18503333f);
        glm::vec4 kBlueVec4 = glm::vec4(0.10667330f, 12.64194608f, -60.58204836f, 110.36276771f);

        glm::vec2 kRedVec2 = glm::vec2(-152.94239396f, 59.28637943f);
        glm::vec2 kGreenVec2 = glm::vec2(4.27729857f, 2.82956604f);
        glm::vec2 kBlueVec2 = glm::vec2(-89.90310912f, 27.34824973f);

        // Vector powers of x: [1.0, x, x^2, x^3]
        glm::vec4 v4 = glm::vec4(1.0f, clampedT, clampedT * clampedT, clampedT * clampedT * clampedT);
        // Vector higher-order powers: [x^4, x^5]
        glm::vec2 v2 = glm::vec2(v4.z * v4.z, v4.z * v4.w);

        // Fast parallel dot-product projection for RGB channels
        float r = glm::dot(v4, kRedVec4) + glm::dot(v2, kRedVec2);
        float g = glm::dot(v4, kGreenVec4) + glm::dot(v2, kGreenVec2);
        float b = glm::dot(v4, kBlueVec4) + glm::dot(v2, kBlueVec2);

        return glm::clamp(glm::vec3(r, g, b), 0.0f, 1.0f);
    }

    inline glm::vec3 coolwarm(float t) {
        float clampedT = glm::clamp(t, 0.0f, 1.0f);
        glm::vec3 blue = glm::vec3(0.230f, 0.299f, 0.754f);
        glm::vec3 white = glm::vec3(0.865f, 0.865f, 0.865f);
        glm::vec3 red = glm::vec3(0.706f, 0.016f, 0.150f);

        float s = clampedT < 0.5f ? clampedT * 2.0f : (clampedT - 0.5f) * 2.0f;
        float h = s * s * (3.0f - 2.0f * s);
        return clampedT < 0.5f ? glm::mix(blue, white, h) : glm::mix(white, red, h);
    }

    // Muted CoolWarm: Restricts midpoint brightness to prevent clipping out 3D light vectors
    inline glm::vec3 mutedCoolWarm(float t) {
        float clampedT = glm::clamp(t, 0.0f, 1.0f);
        glm::vec3 darkBlue = glm::vec3(0.230f, 0.299f, 0.754f);
        glm::vec3 neutralBg = glm::vec3(0.700f, 0.700f, 0.700f); // Muted core preserves STL edge shadow details
        glm::vec3 darkRed = glm::vec3(0.706f, 0.016f, 0.150f);

        float s = clampedT < 0.5f ? clampedT * 2.0f : (clampedT - 0.5f) * 2.0f;
        return clampedT < 0.5f ? glm::mix(darkBlue, neutralBg, s) : glm::mix(neutralBg, darkRed, s);
    }

    // BWR: Strict Linear Blue-White-Red Diverging map
    inline glm::vec3 bwr(float t) {
        float clampedT = glm::clamp(t, 0.0f, 1.0f);
        glm::vec3 blue = glm::vec3(0.000f, 0.000f, 1.000f);
        glm::vec3 white = glm::vec3(1.000f, 1.000f, 1.000f);
        glm::vec3 red = glm::vec3(1.000f, 0.000f, 0.000f);

        float s = clampedT < 0.5f ? clampedT * 2.0f : (clampedT - 0.5f) * 2.0f;
        return clampedT < 0.5f ? glm::mix(blue, white, s) : glm::mix(white, red, s);
    }

    inline glm::vec3 cividis(float t) {
        float clampedT = glm::clamp(t, 0.0f, 1.0f);
        glm::vec3 c0 = glm::vec3(0.000f, 0.134f, 0.311f);
        glm::vec3 c1 = glm::vec3(0.301f, 0.334f, 0.457f);
        glm::vec3 c2 = glm::vec3(0.603f, 0.550f, 0.518f);
        glm::vec3 c3 = glm::vec3(0.992f, 0.854f, 0.220f);

        if (clampedT < 0.33f)      return glm::mix(c0, c1, clampedT / 0.33f);
        else if (clampedT < 0.66f) return glm::mix(c1, c2, (clampedT - 0.33f) / 0.33f);
        else                       return glm::mix(c2, c3, (clampedT - 0.66f) / 0.34f);
    }

    inline glm::vec3 grayscale(float t) {
        return glm::vec3(glm::clamp(t, 0.0f, 1.0f));
    }

    inline glm::vec3 evaluate(float t, ColormapType type) {
        switch (type) {
        case ColormapType::Viridis:       return viridis(t);
        case ColormapType::Inferno:       return inferno(t);
        case ColormapType::Turbo:         return turbo(t);
        case ColormapType::CoolWarm:      return coolwarm(t);
        case ColormapType::MutedCoolWarm: return mutedCoolWarm(t);
        case ColormapType::BWR:           return bwr(t);
        case ColormapType::Cividis:       return cividis(t);
        case ColormapType::Grayscale:     return grayscale(t);
        default:                          return glm::vec3(1.0f);
        }
    }

} // namespace Colormaps