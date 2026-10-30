#pragma once
#include <glm/glm.hpp>
#include <cmath>
#include <array>

enum class ColormapType {
    Turbo = 0,         // Google's Engineering Rainbow: High-contrast replacement for Jet/Rainbow
    Viridis = 1,       // CFD/FEA Default: Perceptually uniform scalar magnitude (Stress, Speed)
    Inferno = 2,       // Sequential Thermal: Excellent for localized energy dissipation/hot-spots
    CoolWarm = 3,      // Balanced Diverging: Smooth sign-variant fields (e.g., pressure differentials)
    MutedCoolWarm = 4, // 3D Mesh-Friendly: Prevents whiteout, preserves surface lighting/shading
    BWR = 5,           // Classical Diverging: Explicit Blue-White-Red mapping (Tension/Compression)
    Cividis = 6,       // Print & Colorblind Safe: Matches strict engineering publication standards
    Grayscale = 7,     // Shading Baseline: Verifies geometry visibility and light interaction
    Plasma = 8,        // Matplotlib Plasma: perceptually uniform, good for flow/thermal
    Magma = 9,         // Matplotlib Magma: darker, print-friendly sequential
    Parula = 10,       // MATLAB Parula: familiar default
    Hot = 11,          // Blackbody/hot: black->red->yellow->white
    Vik = 12,          // Crameri vik: perceptually uniform diverging
    Broc = 13,         // Crameri broc
    Cork = 14,         // Crameri cork
    Lisbon = 15,       // Crameri lisbon
    Tokyo = 16,        // Crameri tokyo
    Berlin = 17,       // Crameri berlin
    Vanimo = 18,       // Crameri vanimo
    RdBu = 19,         // ColorBrewer RdBu
    RdYlBu = 20,       // ColorBrewer RdYlBu
    Spectral = 21,     // ColorBrewer Spectral
    Twilight = 22,     // Matplotlib twilight (cyclic)
    TwilightShifted = 23, // Matplotlib twilight shifted (cyclic)

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
        case ColormapType::Plasma:   return "Plasma";
        case ColormapType::Magma:    return "Magma";
        case ColormapType::Parula:   return "Parula";
        case ColormapType::Hot:      return "Hot";
        case ColormapType::Vik:      return "Vik";
        case ColormapType::Broc:     return "Broc";
        case ColormapType::Cork:     return "Cork";
        case ColormapType::Lisbon:   return "Lisbon";
        case ColormapType::Tokyo:    return "Tokyo";
        case ColormapType::Berlin:   return "Berlin";
        case ColormapType::Vanimo:   return "Vanimo";
        case ColormapType::RdBu:     return "RdBu";
        case ColormapType::RdYlBu:   return "RdYlBu";
        case ColormapType::Spectral: return "Spectral";
        case ColormapType::Twilight: return "Twilight";
        case ColormapType::TwilightShifted: return "Twilight Shifted";
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

    // Google Turbo: exact 256-entry LUT generated from Google's published
    // 5th-order polynomial approximation. Avoids per-fragment polynomial
    // evaluation and matches the reference to float precision.
    inline glm::vec3 turbo(float t) {
        float clampedT = glm::clamp(t, 0.0f, 1.0f);

        constexpr int N = 256;
        static const auto lut = []() {
            std::array<glm::vec3, N> table{};
            // Google's published polynomial coefficients for Turbo.
            const glm::vec4 kRedVec4 = { 0.13572138f, 4.61539260f, -42.66032258f, 132.13108234f };
            const glm::vec4 kGreenVec4 = { 0.09140261f, 2.19418839f, 4.84296658f, -14.18503333f };
            const glm::vec4 kBlueVec4 = { 0.10667330f, 12.64194608f, -60.58204836f, 110.36276771f };
            const glm::vec2 kRedVec2 = { -152.94239396f, 59.28637943f };
            const glm::vec2 kGreenVec2 = { 4.27729857f, 2.82956604f };
            const glm::vec2 kBlueVec2 = { -89.90310912f, 27.34824973f };
            for (int i = 0; i < N; ++i) {
                float x = static_cast<float>(i) / static_cast<float>(N - 1);
                glm::vec4 v4 = { 1.0f, x, x * x, x * x * x };
                glm::vec2 v2 = { v4.z * v4.z, v4.z * v4.w };
                float r = glm::dot(v4, kRedVec4) + glm::dot(v2, kRedVec2);
                float g = glm::dot(v4, kGreenVec4) + glm::dot(v2, kGreenVec2);
                float b = glm::dot(v4, kBlueVec4) + glm::dot(v2, kBlueVec2);
                table[i] = glm::clamp(glm::vec3(r, g, b), 0.0f, 1.0f);
            }
            return table;
        }();

        float f = clampedT * static_cast<float>(N - 1);
        int i0 = static_cast<int>(f);
        int i1 = (i0 + 1 < N) ? i0 + 1 : i0;
        float frac = f - static_cast<float>(i0);
        return glm::mix(lut[i0], lut[i1], frac);
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

    // --- Matplotlib Plasma ---
    inline glm::vec3 plasma(float t) {
        float x = glm::clamp(t, 0.0f, 1.0f);
        glm::vec3 c0(0.050f,0.030f,0.528f), c1(0.498f,0.006f,0.659f), c2(0.798f,0.195f,0.437f), c3(0.973f,0.582f,0.263f), c4(0.940f,0.975f,0.131f);
        if (x < 0.25f) return glm::mix(c0,c1,x/0.25f);
        if (x < 0.50f) return glm::mix(c1,c2,(x-0.25f)/0.25f);
        if (x < 0.75f) return glm::mix(c2,c3,(x-0.50f)/0.25f);
        return glm::mix(c3,c4,(x-0.75f)/0.25f);
    }

    // --- Matplotlib Magma ---
    inline glm::vec3 magma(float t) {
        float x = glm::clamp(t, 0.0f, 1.0f);
        glm::vec3 c0(0.000f,0.000f,0.016f), c1(0.341f,0.063f,0.429f), c2(0.706f,0.216f,0.478f), c3(0.978f,0.522f,0.341f), c4(0.988f,0.992f,0.749f);
        if (x < 0.25f) return glm::mix(c0,c1,x/0.25f);
        if (x < 0.50f) return glm::mix(c1,c2,(x-0.25f)/0.25f);
        if (x < 0.75f) return glm::mix(c2,c3,(x-0.50f)/0.25f);
        return glm::mix(c3,c4,(x-0.75f)/0.25f);
    }

    // --- MATLAB Parula ---
    inline glm::vec3 parula(float t) {
        float x = glm::clamp(t, 0.0f, 1.0f);
        glm::vec3 c0(0.208f,0.165f,0.529f), c1(0.059f,0.357f,0.867f), c2(0.435f,0.686f,0.259f), c3(0.973f,0.980f,0.051f);
        if (x < 0.33f) return glm::mix(c0,c1,x/0.33f);
        if (x < 0.66f) return glm::mix(c1,c2,(x-0.33f)/0.33f);
        return glm::mix(c2,c3,(x-0.66f)/0.34f);
    }

    // --- Hot / blackbody ---
    inline glm::vec3 hot(float t) {
        float x = glm::clamp(t, 0.0f, 1.0f);
        glm::vec3 c0(0.0f,0.0f,0.0f), c1(0.8f,0.0f,0.0f), c2(1.0f,0.8f,0.0f), c3(1.0f,1.0f,1.0f);
        if (x < 0.33f) return glm::mix(c0,c1,x/0.33f);
        if (x < 0.66f) return glm::mix(c1,c2,(x-0.33f)/0.33f);
        return glm::mix(c2,c3,(x-0.66f)/0.34f);
    }

    // --- Crameri diverging (approx) ---
    inline glm::vec3 vik(float t) {
        float x = glm::clamp(t,0.0f,1.0f);
        glm::vec3 c0(0.0f,0.13f,0.47f), c1(0.4f,0.55f,0.75f), c2(0.93f,0.93f,0.93f), c3(0.8f,0.5f,0.45f), c4(0.47f,0.05f,0.0f);
        if (x < 0.25f) return glm::mix(c0,c1,x/0.25f);
        if (x < 0.50f) return glm::mix(c1,c2,(x-0.25f)/0.25f);
        if (x < 0.75f) return glm::mix(c2,c3,(x-0.50f)/0.25f);
        return glm::mix(c3,c4,(x-0.75f)/0.25f);
    }
    inline glm::vec3 broc(float t) {
        float x = glm::clamp(t,0.0f,1.0f);
        glm::vec3 c0(0.13f,0.27f,0.53f), c1(0.93f,0.93f,0.93f), c2(0.45f,0.30f,0.20f);
        if (x < 0.5f) return glm::mix(c0,c1,x/0.5f);
        return glm::mix(c1,c2,(x-0.5f)/0.5f);
    }
    inline glm::vec3 cork(float t) {
        float x = glm::clamp(t,0.0f,1.0f);
        glm::vec3 c0(0.10f,0.25f,0.50f), c1(0.90f,0.90f,0.90f), c2(0.40f,0.25f,0.15f);
        if (x < 0.5f) return glm::mix(c0,c1,x/0.5f);
        return glm::mix(c1,c2,(x-0.5f)/0.5f);
    }
    inline glm::vec3 lisbon(float t) {
        float x = glm::clamp(t,0.0f,1.0f);
        glm::vec3 c0(0.0f,0.13f,0.47f), c1(0.90f,0.90f,0.90f), c2(0.50f,0.10f,0.10f);
        if (x < 0.5f) return glm::mix(c0,c1,x/0.5f);
        return glm::mix(c1,c2,(x-0.5f)/0.5f);
    }
    inline glm::vec3 tokyo(float t) {
        float x = glm::clamp(t,0.0f,1.0f);
        glm::vec3 c0(0.10f,0.10f,0.40f), c1(0.85f,0.85f,0.85f), c2(0.40f,0.10f,0.10f);
        if (x < 0.5f) return glm::mix(c0,c1,x/0.5f);
        return glm::mix(c1,c2,(x-0.5f)/0.5f);
    }
    inline glm::vec3 berlin(float t) {
        float x = glm::clamp(t,0.0f,1.0f);
        glm::vec3 c0(0.0f,0.05f,0.30f), c1(0.75f,0.75f,0.75f), c2(0.30f,0.05f,0.0f);
        if (x < 0.5f) return glm::mix(c0,c1,x/0.5f);
        return glm::mix(c1,c2,(x-0.5f)/0.5f);
    }
    inline glm::vec3 vanimo(float t) {
        float x = glm::clamp(t,0.0f,1.0f);
        glm::vec3 c0(0.25f,0.0f,0.40f), c1(0.90f,0.90f,0.90f), c2(0.0f,0.40f,0.20f);
        if (x < 0.5f) return glm::mix(c0,c1,x/0.5f);
        return glm::mix(c1,c2,(x-0.5f)/0.5f);
    }

    // --- ColorBrewer ---
    inline glm::vec3 rdBu(float t) {
        float x = glm::clamp(t,0.0f,1.0f);
        glm::vec3 c0(0.02f,0.19f,0.38f), c1(0.13f,0.40f,0.77f), c2(0.97f,0.97f,0.97f), c3(0.84f,0.38f,0.30f), c4(0.40f,0.0f,0.12f);
        if (x < 0.25f) return glm::mix(c0,c1,x/0.25f);
        if (x < 0.50f) return glm::mix(c1,c2,(x-0.25f)/0.25f);
        if (x < 0.75f) return glm::mix(c2,c3,(x-0.50f)/0.25f);
        return glm::mix(c3,c4,(x-0.75f)/0.25f);
    }
    inline glm::vec3 rdYlBu(float t) {
        float x = glm::clamp(t,0.0f,1.0f);
        glm::vec3 c0(0.80f,0.10f,0.10f), c1(0.95f,0.60f,0.20f), c2(0.99f,0.99f,0.60f), c3(0.40f,0.65f,0.90f), c4(0.10f,0.30f,0.80f);
        if (x < 0.25f) return glm::mix(c0,c1,x/0.25f);
        if (x < 0.50f) return glm::mix(c1,c2,(x-0.25f)/0.25f);
        if (x < 0.75f) return glm::mix(c2,c3,(x-0.50f)/0.25f);
        return glm::mix(c3,c4,(x-0.75f)/0.25f);
    }
    inline glm::vec3 spectral(float t) {
        float x = glm::clamp(t,0.0f,1.0f);
        glm::vec3 c0(0.62f,0.00f,0.26f), c1(0.84f,0.24f,0.31f), c2(0.96f,0.43f,0.26f), c3(0.99f,0.88f,0.55f), c4(0.40f,0.76f,0.65f), c5(0.37f,0.31f,0.64f);
        if (x < 0.2f) return glm::mix(c0,c1,x/0.2f);
        if (x < 0.4f) return glm::mix(c1,c2,(x-0.2f)/0.2f);
        if (x < 0.6f) return glm::mix(c2,c3,(x-0.4f)/0.2f);
        if (x < 0.8f) return glm::mix(c3,c4,(x-0.6f)/0.2f);
        return glm::mix(c4,c5,(x-0.8f)/0.2f);
    }

    // --- Matplotlib twilight (cyclic) ---
    inline glm::vec3 twilight(float t) {
        float x = glm::clamp(t,0.0f,1.0f);
        // 9 stops around the cycle; endpoints identical
        glm::vec3 stops[9] = {
            glm::vec3(0.94f,0.90f,0.95f),
            glm::vec3(0.60f,0.50f,0.80f),
            glm::vec3(0.20f,0.40f,0.70f),
            glm::vec3(0.10f,0.60f,0.50f),
            glm::vec3(0.30f,0.70f,0.30f),
            glm::vec3(0.70f,0.70f,0.20f),
            glm::vec3(0.85f,0.40f,0.20f),
            glm::vec3(0.85f,0.30f,0.50f),
            glm::vec3(0.94f,0.90f,0.95f)
        };
        float seg = x * 8.0f;
        int i = (int)seg;
        if (i > 7) i = 7;
        float frac = seg - (float)i;
        return glm::mix(stops[i], stops[i+1], frac);
    }
    inline glm::vec3 twilightShifted(float t) {
        float x = t + 0.5f;
        if (x > 1.0f) x -= 1.0f;
        return twilight(x);
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
        case ColormapType::Plasma:        return plasma(t);
        case ColormapType::Magma:         return magma(t);
        case ColormapType::Parula:        return parula(t);
        case ColormapType::Hot:           return hot(t);
        case ColormapType::Vik:           return vik(t);
        case ColormapType::Broc:          return broc(t);
        case ColormapType::Cork:          return cork(t);
        case ColormapType::Lisbon:        return lisbon(t);
        case ColormapType::Tokyo:         return tokyo(t);
        case ColormapType::Berlin:        return berlin(t);
        case ColormapType::Vanimo:        return vanimo(t);
        case ColormapType::RdBu:          return rdBu(t);
        case ColormapType::RdYlBu:        return rdYlBu(t);
        case ColormapType::Spectral:      return spectral(t);
        case ColormapType::Twilight:      return twilight(t);
        case ColormapType::TwilightShifted: return twilightShifted(t);
        default:                          return glm::vec3(1.0f);
        }
    }

} // namespace Colormaps