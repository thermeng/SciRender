#pragma once
// Immutable integration artifact (plan §4.3). Produced once by
// IntegrationService, shared as shared_ptr<const StreamlineData>,
// read by geometry builders, particles and the GPU upload path.
// Never mutated after publication.
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace streamline {

struct StreamlinePath {
    std::vector<glm::vec3> points;      // >= 2 for a valid path
    std::vector<glm::vec3> fieldVecs;   // size == points.size() (colour + arrows)
    std::vector<float>     speed;       // size == points.size()
    std::vector<float>     arcLength;   // size == points.size(), monotonic, [0]==0
    float totalLength = 0.0f;           // == arcLength.back(); never a placeholder
    float pathMaxMag  = 0.0f;           // cached once (particles must not rescan)
    bool  valid() const {
        return points.size() >= 2 && speed.size() == points.size()
            && fieldVecs.size() == points.size()
            && arcLength.size() == points.size();
    }
};

struct ArrowHead {
    glm::vec3 position{};
    glm::vec3 direction{0,0,1};
    float     magnitude = 0.0f;
    glm::vec3 fieldVec{};
};

struct MagnitudeRanges {
    float magMin = 0.0f, magMax = 1.0f;
    float compMin[3] = {0,0,0}, compMax[3] = {0,0,0};
};

enum class StopReason { NotStopped, LeftDomain, ZeroField, MaxSteps, MaxLength, OutsideMesh };

struct IntegrationStats {
    int seedsRequested = 0, seedsProduced = 0, seedsFiltered = 0;
    int pathsProduced = 0;
    int truncatedMaxSteps = 0, truncatedMaxLength = 0, truncatedDomain = 0;
};

struct Diagnostics {
    enum class Reason { Ok, InvalidDomain, ZeroField, AllSeedsOutside,
                        BudgetExceeded, Cancelled, NoPaths } reason = Reason::Ok;
    std::string detail;
};

struct StreamlineData {
    std::vector<StreamlinePath> paths;
    std::vector<glm::vec3>      seedPoints;
    std::vector<ArrowHead>      arrows;
    MagnitudeRanges             ranges;
    IntegrationStats            stats;
    Diagnostics                 diagnostics;
    uint64_t                    generation = 0;
    bool empty() const { return paths.empty(); }
};

} // namespace streamline
