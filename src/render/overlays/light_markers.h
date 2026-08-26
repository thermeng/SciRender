#pragma once
// Solid-disc markers showing light-kit directions, composited over the same
// corner square as the axis triad. Extracted from Gizmo: lighting visualization
// is a distinct feature with its own settings page, not part of wayfinding.
// Pure GL; no Qt beyond context lifetime.

#include <glm/glm.hpp>
#include "render/foundation/gl_raii.h"

class LightMarkerOverlay {
public:
    LightMarkerOverlay() = default;
    ~LightMarkerOverlay();

    bool init();
    void shutdown();
    bool isInitialized() const { return markProgram.has(); }

    // dirs/cols: five kit directions (xy components place each disc inside the
    // square) and their tint colors. Placement reuses Gizmo::rectOrigin so the
    // markers always track the triad square's corner/size.
    void draw(const glm::vec3 dirs[5], const glm::vec3 cols[5], float dpr,
              int deviceW, int deviceH, int corner = 0, int foot = 128);

private:
    bool buildProgram();

    GlProgram markProgram;
    GLint markMvpLoc = -1;
    GlVao markVAO;
    GlBuffer markVBO;
};
