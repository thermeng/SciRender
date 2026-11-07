#include "render/streamlines/integrate/Integration.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace streamline {

int IntegrationParameters::effectiveMaxSteps(float extent) const {
    int eff = maxSteps;
    if (maxPathLength > 0.0f && stepSize > 0.0f)
        eff = std::min(eff, (int)std::ceil(maxPathLength / stepSize));
    return std::max(1, eff);
}

std::optional<IntegrationParameters>
validateParameters(IntegrationParameters p, const FieldDomain& domain) {
    const float extent = domain.bounds().extent;
    if (!std::isfinite(p.stepSize) || p.stepSize <= 0.0f || p.stepSize > extent)
        return std::nullopt;
    if (p.maxSteps < 1 || p.maxSteps > p.budget.maxStepsPerPath)
        return std::nullopt;
    if (p.seedCount < 0 || p.seedCount > p.budget.maxSeeds)
        return std::nullopt;
    if (!std::isfinite(p.planePos) || p.planePos < 0.0 || p.planePos > 1.0)
        return std::nullopt;
    if (!std::isfinite(p.jitter) || p.jitter < 0.0 || p.jitter > 1.0)
        return std::nullopt;
    if (p.maxPathLength < 0.0f || !std::isfinite(p.maxPathLength))
        return std::nullopt;
    p.planeCountU = std::clamp(p.planeCountU, 1, 200);
    p.planeCountV = std::clamp(p.planeCountV, 1, 200);
    return p;
}

std::vector<glm::vec3> generateSeeds(const FieldDomain& domain,
                                     const IntegrationParameters& params) {
    std::vector<glm::vec3> seeds;
    const auto& b = domain.bounds();
    const float sx = b.max.x - b.min.x, sy = b.max.y - b.min.y, sz = b.max.z - b.min.z;
    std::mt19937_64 rng(params.rngSeed);
    std::uniform_real_distribution<float> jit(-1.0f, 1.0f);
    auto addJitter = [&](glm::vec3& p) {
        if (params.jitter <= 0.0) return;
        p.x = std::clamp(p.x + sx * 0.05f * jit(rng) * (float)params.jitter, b.min.x, b.max.x);
        p.y = std::clamp(p.y + sy * 0.05f * jit(rng) * (float)params.jitter, b.min.y, b.max.y);
        p.z = std::clamp(p.z + sz * 0.05f * jit(rng) * (float)params.jitter, b.min.z, b.max.z);
    };

    switch (params.seedMode) {
    case SeedMode::Surface: {
        const auto& pos = domain.positions();
        int want = std::min(params.seedCount, (int)pos.size());
        if (want <= 0) return seeds;
        seeds.reserve(want);
        for (int i = 0; i < want; ++i) {
            int idx = (int)((long long)i * pos.size() / want);
            glm::vec3 p = pos[idx]; addJitter(p); seeds.push_back(p);
        }
        return seeds;
    }
    case SeedMode::PlaneXY: case SeedMode::PlaneXZ: case SeedMode::PlaneYZ: {
        int nU = std::max(1, params.planeCountU), nV = std::max(1, params.planeCountV);
        long long total = (long long)nU * nV;
        if (total > params.budget.maxSeeds) total = params.budget.maxSeeds;
        seeds.reserve((size_t)total);
        float pos = (float)params.planePos;
        for (int iv = 0; iv < nV && (int)seeds.size() < total; ++iv)
            for (int iu = 0; iu < nU && (int)seeds.size() < total; ++iu) {
                float tu = (iu + 0.5f) / nU, tv = (iv + 0.5f) / nV;
                glm::vec3 p;
                if (params.seedMode == SeedMode::PlaneXY)
                    p = {b.min.x + tu*sx, b.min.y + tv*sy, b.min.z + pos*sz};
                else if (params.seedMode == SeedMode::PlaneXZ)
                    p = {b.min.x + tu*sx, b.min.y + pos*sy, b.min.z + tv*sz};
                else
                    p = {b.min.x + pos*sx, b.min.y + tu*sy, b.min.z + tv*sz};
                addJitter(p); seeds.push_back(p);
            }
        return seeds;
    }
    case SeedMode::Volume: {
        if (params.seedCount <= 0) return seeds;
        int n = std::max(1, (int)std::ceil(std::cbrt((double)params.seedCount)));
        // Fill the n^3 lattice in a balanced order: stride through all cells
        // so a non-cubic request still covers every axis (retires slab bias).
        long long totalCells = (long long)n * n * n;
        int want = std::min(params.seedCount, (int)std::min<long long>(totalCells, params.budget.maxSeeds));
        seeds.reserve(want);
        for (int s = 0; s < want; ++s) {
            long long cell = (long long)s * totalCells / want; // even stride over lattice
            int ix = (int)(cell % n), iy = (int)((cell / n) % n), iz = (int)(cell / ((long long)n * n));
            glm::vec3 p{b.min.x + (ix + 0.5f)/n * sx,
                        b.min.y + (iy + 0.5f)/n * sy,
                        b.min.z + (iz + 0.5f)/n * sz};
            addJitter(p); seeds.push_back(p);
        }
        return seeds;
    }
    }
    return seeds;
}

StopReason Rk4Integrator::integrate(const IFieldSampler& sampler,
                                    const glm::vec3& seed, int dirSign,
                                    const IntegrationParameters& params,
                                    const std::atomic<bool>& cancel,
                                    std::vector<glm::vec3>& outPoints,
                                    std::vector<float>& outSpeeds,
                                    std::vector<glm::vec3>& outFieldVecs) const {
    outPoints.push_back(seed);
    glm::vec3 v0 = sampler.sample(seed);
    outSpeeds.push_back(std::sqrt(glm::dot(v0, v0)));
    outFieldVecs.push_back(v0);
    if (!sampler.inside(seed)) return StopReason::OutsideMesh;

    auto dirAt = [&](const glm::vec3& p) -> glm::vec3 {
        glm::vec3 v = sampler.sample(p);
        float m = std::sqrt(glm::dot(v, v));
        return (m > 1e-12f) ? (float)dirSign * (v / m) : glm::vec3(0.0f);
    };
    const float h = params.stepSize;
    int maxSteps = params.effectiveMaxSteps(1.0f);
    glm::vec3 pos = seed;
    float length = 0.0f;
    for (int step = 0; step < maxSteps; ++step) {
        if (cancel.load(std::memory_order_relaxed)) return StopReason::NotStopped;
        glm::vec3 k1 = dirAt(pos);
        if (glm::dot(k1,k1) < 1e-24f) return StopReason::ZeroField;
        glm::vec3 k2 = dirAt(pos + h * 0.5f * k1);
        if (glm::dot(k2,k2) < 1e-24f) return StopReason::ZeroField;
        glm::vec3 k3 = dirAt(pos + h * 0.5f * k2);
        if (glm::dot(k3,k3) < 1e-24f) return StopReason::ZeroField;
        glm::vec3 k4 = dirAt(pos + h * k3);
        if (glm::dot(k4,k4) < 1e-24f) return StopReason::ZeroField;
        glm::vec3 next = pos + (h / 6.0f) * (k1 + 2.0f*k2 + 2.0f*k3 + k4);
        glm::vec3 nv = sampler.sample(next);
        float nm = std::sqrt(glm::dot(nv, nv));
        if (nm <= 1e-12f) return StopReason::ZeroField;
        if (!sampler.inside(next)) return StopReason::LeftDomain;
        length += glm::length(next - pos);
        if (params.maxPathLength > 0.0f && length > params.maxPathLength)
            return StopReason::MaxLength;
        outPoints.push_back(next);
        outSpeeds.push_back(nm);
        outFieldVecs.push_back(nv);
        pos = next;
    }
    return StopReason::MaxSteps;
}

StreamlineData integrateAll(const FieldDomain& domain,
                            const IntegrationParameters& params,
                            const std::atomic<bool>& cancel) {
    StreamlineData data;
    data.stats.seedsRequested = params.seedCount;
    auto sampler = makeSampler(domain);
    auto seeds = generateSeeds(domain, params);
    data.stats.seedsProduced = (int)seeds.size();

    // Filter seeds with (near-)zero field, as before.
    std::vector<glm::vec3> live;
    live.reserve(seeds.size());
    for (auto& s : seeds) {
        if (!sampler->inside(s)) continue;
        glm::vec3 v = sampler->sample(s);
        if (glm::dot(v, v) <= 0.0f) continue;
        live.push_back(s);
    }
    data.stats.seedsFiltered = data.stats.seedsProduced - (int)live.size();
    if (live.empty()) {
        data.diagnostics.reason = Diagnostics::Reason::AllSeedsOutside;
        data.diagnostics.detail = "no seed with a non-zero field inside the domain";
        return data;
    }
    data.seedPoints = live;

    Rk4Integrator integrator;
    const bool doF = params.direction != Direction::Backward;
    const bool doB = params.direction != Direction::Forward;
    size_t totalPoints = 0;

    for (auto& seed : live) {
        if (cancel.load(std::memory_order_relaxed)) {
            data.diagnostics.reason = Diagnostics::Reason::Cancelled;
            return data;
        }
        if ((int)seeds.size() > params.budget.maxSeeds) {
            data.diagnostics.reason = Diagnostics::Reason::BudgetExceeded; return data;
        }
        std::vector<glm::vec3> fPts, bPts, fFv, bFv; std::vector<float> fSp, bSp;
        StopReason fStop = StopReason::NotStopped, bStop = StopReason::NotStopped;
        if (doF) fStop = integrator.integrate(*sampler, seed, +1, params, cancel, fPts, fSp, fFv);
        if (doB) bStop = integrator.integrate(*sampler, seed, -1, params, cancel, bPts, bSp, bFv);
        if (fStop == StopReason::MaxSteps) data.stats.truncatedMaxSteps++;
        if (fStop == StopReason::MaxLength) data.stats.truncatedMaxLength++;
        if (bStop == StopReason::MaxSteps) data.stats.truncatedMaxSteps++;
        if (bStop == StopReason::MaxLength) data.stats.truncatedMaxLength++;
        if (fStop == StopReason::LeftDomain || bStop == StopReason::LeftDomain)
            data.stats.truncatedDomain++;

        // Splice: backward (reversed, seed de-duplicated) + forward.
        // bPts is seed-first, so in reverse order the seed is the LAST
        // element: [rbegin(), rend()-1) keeps the tip and drops the seed.
        // (The old code used rbegin()+1, which dropped the tip instead.)
        StreamlinePath path;
        if (doB && !bPts.empty() && doF && !fPts.empty()) {
            path.points.insert(path.points.end(), bPts.rbegin(), bPts.rend() - 1);
            path.points.insert(path.points.end(), fPts.begin(), fPts.end());
            path.speed.insert(path.speed.end(), bSp.rbegin(), bSp.rend() - 1);
            path.speed.insert(path.speed.end(), fSp.begin(), fSp.end());
            path.fieldVecs.insert(path.fieldVecs.end(), bFv.rbegin(), bFv.rend() - 1);
            path.fieldVecs.insert(path.fieldVecs.end(), fFv.begin(), fFv.end());
        } else if (!fPts.empty()) { path.points = fPts; path.speed = fSp; path.fieldVecs = fFv; }
        else if (!bPts.empty())  { path.points = bPts; path.speed = bSp; path.fieldVecs = bFv; }
        if (path.points.size() < 2) continue;
        totalPoints += path.points.size();
        if (totalPoints > params.budget.maxTotalPoints) {
            data.diagnostics.reason = Diagnostics::Reason::BudgetExceeded;
            data.diagnostics.detail = "total point budget exceeded";
            return data;
        }
        path.arcLength.reserve(path.points.size());
        path.arcLength.push_back(0.0f);
        for (size_t i = 1; i < path.points.size(); ++i)
            path.arcLength.push_back(path.arcLength.back() + glm::length(path.points[i] - path.points[i-1]));
        path.totalLength = path.arcLength.back();
        if (path.totalLength < 1e-12f) continue; // degenerate: all points coincide
        path.pathMaxMag = 0.0f;
        for (float s : path.speed) path.pathMaxMag = std::max(path.pathMaxMag, s);
        data.paths.push_back(std::move(path));
    }

    data.stats.pathsProduced = (int)data.paths.size();
    if (data.paths.empty()) {
        data.diagnostics.reason = Diagnostics::Reason::NoPaths;
        data.diagnostics.detail = "integration produced no path with >= 2 points";
    }
    // Ranges from speeds (magnitude) — component ranges are filled by the
    // geometry stage from the actual field vectors it emits.
    float mn = 1e30f, mx = -1e30f;
    for (auto& p : data.paths) for (float s : p.speed) { mn = std::min(mn, s); mx = std::max(mx, s); }
    if (mn <= mx) { data.ranges.magMin = mn; data.ranges.magMax = mx; }
    return data;
}

} // namespace streamline
