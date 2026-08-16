#pragma once

// Centralized render configuration constants. Replaces hardcoded magic numbers
// scattered across the renderer, viewport, and mesh manager. Defaults are
// tuned for a balanced desktop experience; can be overridden at runtime via
// RenderSettings or loaded from a JSON profile for different hardware.

struct RenderConfig {
    // LOD (Level of Detail) parameters
    float lodDebounceSeconds = 0.14f;    // time after camera stops before full-res mesh is used
    int   lodMinVertices     = 4000;     // minimum vertex count for LOD to be considered
    float lodDecimateRatio   = 0.5f;     // target ratio for decimated mesh (0.5 = half the triangles)

    // Camera framing
    float cameraFitMultiplier = 1.3f;    // multiplier on fit distance when resetting camera

    // Viewport interaction
    float mouseSensitivity    = 0.5f;    // degrees per pixel of mouse movement
    int   postMotionRedrawMs  = 160;     // ms after motion stops to trigger one final full-res redraw

    // Static accessor for default config (used by Renderer, ViewportVisualizer, etc.)
    static const RenderConfig& defaults() {
        static const RenderConfig cfg;
        return cfg;
    }
};
