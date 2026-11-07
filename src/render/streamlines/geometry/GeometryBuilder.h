#pragma once
// Geometry generation from StreamlineData (plan §4.3/§6.3).
// Interleaved vertex layout (unchanged ABI): pos(3) mag(1) comp(3) normal(3) = 10 floats.
#include "render/streamlines/data/StreamlineData.h"
#include <glm/glm.hpp>
#include <vector>

namespace streamline {

struct GeometryConfig {
    int   style = 1;            // 0 lines, 1 tube, 2 ribbon
    float baseWidth = 0.005f;   // world units (extent * ribbonWidth, computed by caller)
    float taperFactor = 0.0f;   // [0,1]
    bool  showArrows = false;
    float arrowSize = 0.02f;    // fraction of extent
    float arrowSpacingFrac = 0.15f;
    float extent = 1.0f;
    size_t maxVertices = 4'000'000;
};

struct GeometryOutput {
    std::vector<float> verts;       // streamline tube/ribbon/lines
    std::vector<float> arrowVerts;  // arrowheads
    std::vector<float> seedVerts;   // xyz per seed
    int lineCount = 0, arrowCount = 0, seedCount = 0;
    MagnitudeRanges ranges;
    bool budgetExceeded = false;
};

// Parallel-transport frame: propagates the previous frame instead of
// re-deriving from a fixed axis (retires the |t.y|=0.9 twist).
struct Frame { glm::vec3 normal, binormal, tangent; };
Frame initialFrame(const glm::vec3& tangent);
Frame transportFrame(const Frame& prev, const glm::vec3& tangent);

GeometryOutput buildGeometry(const StreamlineData& data, const GeometryConfig& cfg);

} // namespace streamline
