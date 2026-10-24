#pragma once

#include <string>

// Pure value type for animation colormap range bookkeeping. Relocated from
// FieldResolver so it lives next to its only consumer (RenderSettings).
struct AnimRangeState {
    bool global = true;
    std::string field;
    float rangeMin = 0.0f;
    float rangeMax = 1.0f;

    void advance(bool firstFrame, const std::string& frameField,
                 float frameMin, float frameMax,
                 float& outMin, float& outMax) {
        if (!global) { outMin = frameMin; outMax = frameMax; return; }
        if (firstFrame || field != frameField) {
            rangeMin = frameMin;
            rangeMax = frameMax;
        } else {
            rangeMin = std::min(rangeMin, frameMin);
            rangeMax = std::max(rangeMax, frameMax);
        }
        field = frameField;
        outMin = rangeMin;
        outMax = rangeMax;
    }

    void invalidate() { field.clear(); }
};