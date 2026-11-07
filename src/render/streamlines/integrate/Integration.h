#pragma once
// Integration parameters, seeding and integrator (plan §4.4, §5).
#include "render/streamlines/data/StreamlineData.h"
#include "render/streamlines/domain/FieldDomain.h"
#include "render/streamlines/domain/Sampling.h"
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

namespace streamline {

enum class Direction { Forward, Backward, Both };
enum class SeedMode   { Volume, Surface, PlaneXY, PlaneXZ, PlaneYZ };

struct Budget {
    int    maxSeeds        = 50'000;
    int    maxStepsPerPath = 5'000;
    size_t maxTotalPoints  = 4'000'000;
};

struct IntegrationParameters {
    float    stepSize       = 0.01f;
    int      maxSteps       = 200;
    float    maxPathLength  = 0.0f;   // 0 = unset; always honoured WITH maxSteps
    Direction direction     = Direction::Both;
    SeedMode  seedMode      = SeedMode::Volume;
    int       seedCount     = 100;
    double    planePos      = 0.5;
    double    jitter        = 0.0;
    int       planeCountU   = 10, planeCountV = 10;
    uint64_t  rngSeed       = 0x5eedULL;
    Budget    budget{};
    // Returns effective step budget honouring BOTH caps (retires finding #1).
    int effectiveMaxSteps(float extent) const;
};

enum class ParamError { InvalidStep, InvalidSteps, InvalidSeedCount, InvalidPlane, InvalidJitter };
std::optional<IntegrationParameters>
validateParameters(IntegrationParameters p, const FieldDomain& domain);

// Deterministic seeding (retires steady_clock RNG + volume-slab truncation).
std::vector<glm::vec3> generateSeeds(const FieldDomain& domain,
                                     const IntegrationParameters& params);

// RK4 fixed-step integrator behind a strategy interface (plan §5.4).
class IIntegrator {
public:
    virtual ~IIntegrator() = default;
    // Integrates one direction from seed. Fills outPoints/outSpeeds (seed-first).
    virtual StopReason integrate(const IFieldSampler& sampler,
                                 const glm::vec3& seed, int dirSign,
                                 const IntegrationParameters& params,
                                 const std::atomic<bool>& cancel,
                                 std::vector<glm::vec3>& outPoints,
                                 std::vector<float>& outSpeeds,
                                 std::vector<glm::vec3>& outFieldVecs) const = 0;
};

class Rk4Integrator final : public IIntegrator {
public:
    StopReason integrate(const IFieldSampler& sampler,
                         const glm::vec3& seed, int dirSign,
                         const IntegrationParameters& params,
                         const std::atomic<bool>& cancel,
                         std::vector<glm::vec3>& outPoints,
                         std::vector<float>& outSpeeds,
                         std::vector<glm::vec3>& outFieldVecs) const override;
};

// Pure service: (domain, params) -> StreamlineData. No Qt/GL/thread state.
StreamlineData integrateAll(const FieldDomain& domain,
                            const IntegrationParameters& params,
                            const std::atomic<bool>& cancel);

} // namespace streamline
