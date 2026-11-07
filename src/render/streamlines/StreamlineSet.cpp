#include "render/streamlines/StreamlineSet.h"
#include "render/streamlines/domain/FieldDomain.h"
#include "render/streamlines/domain/Sampling.h"
#include "render/streamlines/integrate/Integration.h"
#include "render/streamlines/geometry/GeometryBuilder.h"
#include <atomic>
#include "core/mesh_loader.h"
#include "core/FieldStore.h"

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <limits>
#include <random>
#include <chrono>
#include <vector>

float StreamlineSet::magSq(const glm::vec3& v) {
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

void StreamlineSet::samplePath(const StreamlinePath& path, float t, glm::vec3& outPos, float& outMag) {
    outPos = path.points.empty() ? glm::vec3(0.0f) : path.points.front();
    outMag = 0.0f;
    if (path.points.size() < 2) return;

    const auto& pts = path.points;
    const auto& cum = path.cumulativeLength;
    const int n = static_cast<int>(pts.size());

    float total = std::max(path.totalLength, 1e-6f);
    float s = std::clamp(t, 0.0f, 1.0f) * total;

    int idx = 0;
    if (cum.size() == static_cast<size_t>(n)) {
        idx = static_cast<int>(std::upper_bound(cum.begin(), cum.end(), s) - cum.begin()) - 1;
    } else {
        idx = static_cast<int>(s / total * (n - 1));
    }
    idx = std::clamp(idx, 0, n - 2);

    float segStart = 0.0f, segEnd = total;
    if (cum.size() == static_cast<size_t>(n)) {
        segStart = cum[idx];
        segEnd = cum[idx + 1];
    } else {
        // Fallback: uniform parameterization by index.
        segStart = static_cast<float>(idx) / static_cast<float>(n - 1) * total;
        segEnd = static_cast<float>(idx + 1) / static_cast<float>(n - 1) * total;
    }
    float segLen = std::max(segEnd - segStart, 1e-12f);
    float frac = std::clamp((s - segStart) / segLen, 0.0f, 1.0f);

    outPos = glm::mix(pts[idx], pts[idx + 1], frac);

    if (!path.speedAtPoint.empty()) {
        int si = std::min(idx, static_cast<int>(path.speedAtPoint.size()) - 1);
        int sj = std::min(idx + 1, static_cast<int>(path.speedAtPoint.size()) - 1);
        outMag = path.speedAtPoint[si] * (1.0f - frac) + path.speedAtPoint[sj] * frac;
    }
}

// Uniform-grid spatial index for O(1) average nearest-neighbor lookups.
// Built once per mesh, reused across all streamline integations.
struct UniformGrid {
    glm::vec3 origin;
    float cellSize;
    int nx, ny, nz;
    std::vector<std::vector<int>> cells;

    UniformGrid(const float* verts, int count, float avgSpacing, const glm::vec3& minB, const glm::vec3& maxB) {
        // cellSize >= cutoff diameter (3*spacing) so 3x3x3 search is sufficient.
        cellSize = std::max(avgSpacing * 3.0f, 1e-6f);
        origin = minB;
        glm::vec3 extent = maxB - minB;
        nx = std::max(1, static_cast<int>(std::ceil(extent.x / cellSize)));
        ny = std::max(1, static_cast<int>(std::ceil(extent.y / cellSize)));
        nz = std::max(1, static_cast<int>(std::ceil(extent.z / cellSize)));
        // Cap resolution to avoid runaway allocations on extreme inputs.
        nx = std::min(nx, 256); ny = std::min(ny, 256); nz = std::min(nz, 256);
        // Cap total cell count (not just each axis) to bound memory.
        constexpr size_t kMaxCells = 1u << 20;
        while (static_cast<size_t>(nx) * ny * nz > kMaxCells && (nx > 1 || ny > 1 || nz > 1)) {
            if (nx >= ny && nx >= nz && nx > 1) --nx;
            else if (ny >= nz && ny > 1) --ny;
            else if (nz > 1) --nz;
        }
        cells.resize(static_cast<size_t>(nx) * ny * nz);
        for (int i = 0; i < count; ++i) {
            int cx = std::clamp(static_cast<int>((verts[i * 3 + 0] - origin.x) / cellSize), 0, nx - 1);
            int cy = std::clamp(static_cast<int>((verts[i * 3 + 1] - origin.y) / cellSize), 0, ny - 1);
            int cz = std::clamp(static_cast<int>((verts[i * 3 + 2] - origin.z) / cellSize), 0, nz - 1);
            cells[static_cast<size_t>(cx) + cy * nx + cz * nx * ny].push_back(i);
        }
    }

    // Find nearest vertex index within maxDistSq, or -1 if none found.
    int nearest(const glm::vec3& pos, const float* verts, int count, float maxDistSq) const {
        int cx = static_cast<int>((pos.x - origin.x) / cellSize);
        int cy = static_cast<int>((pos.y - origin.y) / cellSize);
        int cz = static_cast<int>((pos.z - origin.z) / cellSize);
        cx = std::clamp(cx, 0, nx - 1);
        cy = std::clamp(cy, 0, ny - 1);
        cz = std::clamp(cz, 0, nz - 1);

        float bestD2 = maxDistSq;
        int bestIdx = -1;

        // Search 3x3x3 neighborhood of cells
        for (int dz = -1; dz <= 1; ++dz) {
            int z = cz + dz;
            if (z < 0 || z >= nz) continue;
            for (int dy = -1; dy <= 1; ++dy) {
                int y = cy + dy;
                if (y < 0 || y >= ny) continue;
                for (int dx = -1; dx <= 1; ++dx) {
                    int x = cx + dx;
                    if (x < 0 || x >= nx) continue;
                    const auto& cell = cells[static_cast<size_t>(x) + y * nx + z * nx * ny];
                    for (int idx : cell) {
                        int vi = idx * 3;
                        if (vi + 3 > count * 3) continue;
                        float dx2 = verts[vi + 0] - pos.x;
                        float dy2 = verts[vi + 1] - pos.y;
                        float dz2 = verts[vi + 2] - pos.z;
                        float d2 = dx2 * dx2 + dy2 * dy2 + dz2 * dz2;
                        if (d2 < bestD2) { bestD2 = d2; bestIdx = idx; }
                    }
                }
            }
        }
        return bestIdx;
    }
};

glm::vec3 StreamlineSet::evalFieldNearest(const RenderMesh& mesh, const glm::vec3& pos, const glm::vec3* data, int count, int searchCount,
                                          const std::vector<glm::vec3>* positionsOverride) {
    if (!data || count <= 0) return glm::vec3(0.0f);

    const std::vector<glm::vec3>* positions = positionsOverride;
    int numPts = 0;
    if (positions) numPts = static_cast<int>(positions->size());
    else numPts = static_cast<int>(mesh.vertices.size() / 3);
    if (numPts <= 0) return glm::vec3(0.0f);
    int n = std::min(numPts, count);

    float minX, minY, minZ, maxX, maxY, maxZ;
    if (positions && !positions->empty()) {
        minX = maxX = (*positions)[0].x; minY = maxY = (*positions)[0].y; minZ = maxZ = (*positions)[0].z;
        for (const auto& p : *positions) {
            minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
            minZ = std::min(minZ, p.z); maxZ = std::max(maxZ, p.z);
        }
    } else {
        minX = static_cast<float>(mesh.bounds.minX); minY = static_cast<float>(mesh.bounds.minY); minZ = static_cast<float>(mesh.bounds.minZ);
        maxX = static_cast<float>(mesh.bounds.maxX); maxY = static_cast<float>(mesh.bounds.maxY); maxZ = static_cast<float>(mesh.bounds.maxZ);
    }

    const float vol = std::abs((maxX - minX) * (maxY - minY) * (maxZ - minZ));
    const float avgSpacing = (n > 0 && vol > 0.0f)
        ? std::pow(vol / static_cast<float>(n), 1.0f / 3.0f)
        : static_cast<float>(mesh.bounds.extent > 0 ? mesh.bounds.extent : 1.0);

    const float cutoffDistSq = 9.0f * avgSpacing * avgSpacing;

    glm::vec3 minB(minX, minY, minZ);
    glm::vec3 maxB(maxX, maxY, maxZ);

    std::vector<float> posFloat;
    posFloat.reserve(static_cast<size_t>(n) * 3);
    if (positions) {
        for (int i = 0; i < n; ++i) {
            posFloat.push_back((*positions)[i].x);
            posFloat.push_back((*positions)[i].y);
            posFloat.push_back((*positions)[i].z);
        }
    } else {
        const float* verts = mesh.vertices.data();
        for (int i = 0; i < n; ++i) {
            posFloat.push_back(verts[i * 3 + 0]);
            posFloat.push_back(verts[i * 3 + 1]);
            posFloat.push_back(verts[i * 3 + 2]);
        }
    }

    const UniformGrid grid(posFloat.data(), n, avgSpacing, minB, maxB);

    int bestIdx = grid.nearest(pos, posFloat.data(), n, cutoffDistSq);
    if (bestIdx < 0) return glm::vec3(0.0f);
    if (bestIdx >= count) return glm::vec3(0.0f);
    if (magSq(data[bestIdx]) < 1e-12f) return glm::vec3(0.0f);
    return data[bestIdx];
}

StreamlineSet::StructuredGridInfo StreamlineSet::buildStructuredGridInfo(const RenderMesh& mesh, const glm::vec3* data, int count) {
    std::vector<glm::vec3> positions;
    positions.reserve(mesh.vertices.size() / 3);
    for (size_t i = 0; i + 2 < mesh.vertices.size(); i += 3)
        positions.emplace_back(mesh.vertices[i], mesh.vertices[i + 1], mesh.vertices[i + 2]);
    return buildStructuredGridInfo(positions, mesh.gridDimX, mesh.gridDimY, mesh.gridDimZ, data, count);
}

StreamlineSet::StructuredGridInfo StreamlineSet::buildStructuredGridInfo(const std::vector<glm::vec3>& positions, int dimX, int dimY, int dimZ,
                                                                       const glm::vec3* data, int count) {
    StreamlineSet::StructuredGridInfo info;
    info.data = data;
    info.count = count;

    if (dimX <= 0 || dimY <= 0 || dimZ <= 0) return info;
    if (positions.empty()) return info;

    int limit = std::min(static_cast<int>(positions.size()), count);
    if (limit <= 0 || static_cast<long long>(dimX) * dimY * dimZ != limit) return info;
    // Guard against huge allocations.
    if (static_cast<long long>(dimX) * dimY * dimZ > 4000000) return info;

    std::vector<float> allX, allY, allZ;
    allX.reserve(limit);
    allY.reserve(limit);
    allZ.reserve(limit);
    for (int i = 0; i < limit; ++i) {
        allX.push_back(positions[i].x);
        allY.push_back(positions[i].y);
        allZ.push_back(positions[i].z);
    }

    std::sort(allX.begin(), allX.end());
    std::sort(allY.begin(), allY.end());
    std::sort(allZ.begin(), allZ.end());

    auto uniqueTolerance = [](std::vector<float>& vec) {
        if (vec.empty()) return;
        float minVal = vec.front();
        float maxVal = vec.back();
        float range = maxVal - minVal;
        float eps = range * 1e-5f;
        if (eps < 1e-12f || range < 1e-12f) eps = 1e-12f;
        auto it = std::unique(vec.begin(), vec.end(), [eps](float a, float b) {
            return std::abs(a - b) < eps;
        });
        vec.erase(it, vec.end());
    };

    uniqueTolerance(allX);
    uniqueTolerance(allY);
    uniqueTolerance(allZ);

    if (static_cast<int>(allX.size()) != dimX ||
        static_cast<int>(allY.size()) != dimY ||
        static_cast<int>(allZ.size()) != dimZ) {
        return info;
    }

    bool monotonic = true;
    for (int t = 1; t < dimX && monotonic; ++t)
        if (allX[t] < allX[t - 1]) monotonic = false;
    for (int t = 1; t < dimY && monotonic; ++t)
        if (allY[t] < allY[t - 1]) monotonic = false;
    for (int t = 1; t < dimZ && monotonic; ++t)
        if (allZ[t] < allZ[t - 1]) monotonic = false;
    if (!monotonic) return info;

    info.xs = std::move(allX);
    info.ys = std::move(allY);
    info.zs = std::move(allZ);
    info.dimX = dimX;
    info.dimY = dimY;
    info.dimZ = dimZ;

    // Build per-cell activity mask
    const int cX = dimX - 1, cY = dimY - 1, cZ = dimZ - 1;
    if (cX <= 0 || cY <= 0 || cZ <= 0) return info;
    info.cellActive.resize(static_cast<size_t>(cX) * cY * cZ, false);
    auto cellIdx = [cX, cY](int i, int j, int k) { return i + j * cX + k * cX * cY; };
    auto vertIdx = [dimX, dimY](int i, int j, int k) { return i + j * dimX + k * dimX * dimY; };
    for (int k = 0; k < cZ; ++k) {
        for (int j = 0; j < cY; ++j) {
            for (int i = 0; i < cX; ++i) {
                bool active = true;
                for (int dk = 0; dk <= 1 && active; ++dk)
                    for (int dj = 0; dj <= 1 && active; ++dj)
                        for (int di = 0; di <= 1 && active; ++di) {
                            int vi = vertIdx(i + di, j + dj, k + dk);
                            if (vi < 0 || vi >= limit) { active = false; break; }
                            const glm::vec3& v = data[vi];
                            if (magSq(v) == 0.0f) active = false;
                        }
                info.cellActive[cellIdx(i, j, k)] = active;
            }
        }
    }

    info.posStorage.resize(static_cast<size_t>(limit) * 3);
    for (int i = 0; i < limit; ++i) {
        info.posStorage[i * 3 + 0] = positions[i].x;
        info.posStorage[i * 3 + 1] = positions[i].y;
        info.posStorage[i * 3 + 2] = positions[i].z;
    }
    info.verts = info.posStorage.data();
    info.vertCount = limit;

    // avgSpacing
    glm::vec3 minB = positions[0], maxB = positions[0];
    for (int i = 0; i < limit; ++i) {
        minB = glm::min(minB, positions[i]);
        maxB = glm::max(maxB, positions[i]);
    }
    const float vol = std::abs((maxB.x - minB.x) * (maxB.y - minB.y) * (maxB.z - minB.z));
    info.avgSpacing = (limit > 0 && vol > 0.0f)
        ? std::pow(vol / static_cast<float>(limit), 1.0f / 3.0f)
        : 1.0f;

    return info;
}

glm::vec3 StreamlineSet::evalFieldTrilinear(const StreamlineSet::StructuredGridInfo& info, const glm::vec3& pos) {
    if (!info.data) return glm::vec3(0.0f);
    if (info.xs.empty() || info.ys.empty() || info.zs.empty()) return glm::vec3(0.0f);

    if (info.dimX <= 1 || info.dimY <= 1 || info.dimZ <= 1) {
        // Degenerate dimension(s): fall back to nearest-neighbor sampling.
        int i = std::clamp(static_cast<int>(
            std::upper_bound(info.xs.begin(), info.xs.end(), pos.x) - info.xs.begin()) - 1, 0, info.dimX - 1);
        int j = std::clamp(static_cast<int>(
            std::upper_bound(info.ys.begin(), info.ys.end(), pos.y) - info.ys.begin()) - 1, 0, info.dimY - 1);
        int k = std::clamp(static_cast<int>(
            std::upper_bound(info.zs.begin(), info.zs.end(), pos.z) - info.zs.begin()) - 1, 0, info.dimZ - 1);
        auto idx = [&](int ii, int jj, int kk) { return ii + jj * info.dimX + kk * info.dimX * info.dimY; };
        int flat = idx(i, j, k);
        if (flat < 0 || flat >= info.count || !info.data) return glm::vec3(0.0f);
        return info.data[flat];
    }

    if (pos.x < info.xs.front() || pos.x > info.xs.back() ||
        pos.y < info.ys.front() || pos.y > info.ys.back() ||
        pos.z < info.zs.front() || pos.z > info.zs.back()) {
        return glm::vec3(0.0f);
    }

    int i0 = static_cast<int>(std::upper_bound(info.xs.begin(), info.xs.end(), pos.x) - info.xs.begin()) - 1;
    int j0 = static_cast<int>(std::upper_bound(info.ys.begin(), info.ys.end(), pos.y) - info.ys.begin()) - 1;
    int k0 = static_cast<int>(std::upper_bound(info.zs.begin(), info.zs.end(), pos.z) - info.zs.begin()) - 1;

    // Prevent out-of-bounds access when interpolation index is at the end
    if (i0 < 0) i0 = 0; if (j0 < 0) j0 = 0; if (k0 < 0) k0 = 0;
    if (i0 >= info.dimX - 1) i0 = info.dimX - 2; if (j0 >= info.dimY - 1) j0 = info.dimY - 2; if (k0 >= info.dimZ - 1) k0 = info.dimZ - 2;

    int i1 = i0 + 1;
    int j1 = j0 + 1;
    int k1 = k0 + 1;

    float fx = info.xs[i1] > info.xs[i0] ? (pos.x - info.xs[i0]) / (info.xs[i1] - info.xs[i0]) : 0.0f;
    float fy = info.ys[j1] > info.ys[j0] ? (pos.y - info.ys[j0]) / (info.ys[j1] - info.ys[j0]) : 0.0f;
    float fz = info.zs[k1] > info.zs[k0] ? (pos.z - info.zs[k0]) / (info.zs[k1] - info.zs[k0]) : 0.0f;

    auto inRange = [&](int ii, int jj, int kk) -> bool {
        int f = ii + jj * info.dimX + kk * info.dimX * info.dimY;
        return f >= 0 && f < info.count && info.data != nullptr;
    };
    if (!inRange(i0, j0, k0) || !inRange(i1, j1, k1)) return glm::vec3(0.0f);

    auto idx = [&](int i, int j, int k) { return i + j * info.dimX + k * info.dimX * info.dimY; };

    const glm::vec3& v000 = info.data[idx(i0, j0, k0)];
    const glm::vec3& v100 = info.data[idx(i1, j0, k0)];
    const glm::vec3& v010 = info.data[idx(i0, j1, k0)];
    const glm::vec3& v110 = info.data[idx(i1, j1, k0)];
    const glm::vec3& v001 = info.data[idx(i0, j0, k1)];
    const glm::vec3& v101 = info.data[idx(i1, j0, k1)];
    const glm::vec3& v011 = info.data[idx(i0, j1, k1)];
    const glm::vec3& v111 = info.data[idx(i1, j1, k1)];

    auto lerp = [](const glm::vec3& a, const glm::vec3& b, float t) {
        return glm::vec3(
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t
        );
    };

    glm::vec3 c00 = lerp(v000, v100, fx);
    glm::vec3 c10 = lerp(v010, v110, fx);
    glm::vec3 c01 = lerp(v001, v101, fx);
    glm::vec3 c11 = lerp(v011, v111, fx);

    glm::vec3 c0 = lerp(c00, c10, fy);
    glm::vec3 c1 = lerp(c01, c11, fy);

    return lerp(c0, c1, fz);
}

bool StreamlineSet::isInsideDomain(const StreamlineSet::StructuredGridInfo& info, const glm::vec3& pos) {
    if (info.cellActive.empty()) return true;
    if (info.dimX <= 1 || info.dimY <= 1 || info.dimZ <= 1) return true;

    // Cartesian path: binary search on sorted axis arrays
    if (!info.xs.empty()) {
        if (pos.x < info.xs.front() || pos.x > info.xs.back() ||
            pos.y < info.ys.front() || pos.y > info.ys.back() ||
            pos.z < info.zs.front() || pos.z > info.zs.back()) {
            return false;
        }

        int i0 = static_cast<int>(std::upper_bound(info.xs.begin(), info.xs.end(), pos.x) - info.xs.begin()) - 1;
        int j0 = static_cast<int>(std::upper_bound(info.ys.begin(), info.ys.end(), pos.y) - info.ys.begin()) - 1;
        int k0 = static_cast<int>(std::upper_bound(info.zs.begin(), info.zs.end(), pos.z) - info.zs.begin()) - 1;

        if (i0 < 0) i0 = 0; if (j0 < 0) j0 = 0; if (k0 < 0) k0 = 0;
        if (i0 >= info.dimX - 1) i0 = info.dimX - 2;
        if (j0 >= info.dimY - 1) j0 = info.dimY - 2;
        if (k0 >= info.dimZ - 1) k0 = info.dimZ - 2;

        int cX = info.dimX - 1;
        int cY = info.dimY - 1;
        return info.cellActive[i0 + j0 * cX + k0 * cX * cY];
    }

    // Non-Cartesian fallback: find nearest vertex, check grid-boundary
    // containment via half-space tests, then verify cell activity.
    //
    // For non-Cartesian structured grids (e.g. cylindrical), a point can
    // be geometrically close to a boundary vertex yet lie OUTSIDE the
    // domain (e.g. inside a cylinder's hole).  The fix: when the nearest
    // vertex sits on a grid boundary (gi==0, gi==dimX-1, etc.), project
    // the point onto the local grid-step vector.  If the projection points
    // OUTWARD the point is outside the mesh.
    //
    // Axes whose first and last vertices coincide (periodic axes such as
    // theta in a full-annulus cylindrical grid) skip the boundary test.
    if (!info.verts || info.vertCount <= 0) return true;

    const int dX = info.dimX;
    const int dY = info.dimY;
    const int dZ = info.dimZ;

    auto vPos = [&](int i, int j, int k) -> glm::vec3 {
        int idx = i + j * dX + k * dX * dY;
        if (idx < 0 || idx >= info.vertCount) return glm::vec3(0.0f);
        const float* v = &info.verts[idx * 3];
        return glm::vec3(v[0], v[1], v[2]);
    };

    // --- nearest-vertex search (linear scan, capped at 4096 samples) ---
    const int maxSamples = 4096;
    const int stride = std::max(1, info.vertCount / maxSamples);
    float bestD2 = std::numeric_limits<float>::max();
    int bestIdx = -1;
    for (int i = 0; i < info.vertCount; i += stride) {
        int vi = i * 3;
        float dx = info.verts[vi + 0] - pos.x;
        float dy = info.verts[vi + 1] - pos.y;
        float dz = info.verts[vi + 2] - pos.z;
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; bestIdx = i; }
    }
    if (bestIdx < 0) return false;

    // Safety distance cutoff (generous — boundary checks do the real work).
    if (bestD2 > info.avgSpacing * info.avgSpacing * 4.0f) return false;

    // Map flat index to (gi, gj, gk).
    int gi = bestIdx % dX;
    int gj = (bestIdx / dX) % dY;
    int gk = bestIdx / (dX * dY);

    // --- detect periodic axes (first and last vertices coincide) ---
    // Sample a few (j,k) pairs to be robust against stride artifacts.
    auto axisIsPeriodic = [&](int axis) -> bool {
        int n = (axis == 0) ? dX : (axis == 1) ? dY : dZ;
        if (n <= 1) return false;
        // Compare the first and last vertices along this axis.
        int i0 = (axis == 0) ? 0 : 0;
        int j0 = (axis == 1) ? 0 : 0;
        int k0 = (axis == 2) ? 0 : 0;
        int i1 = (axis == 0) ? dX - 1 : i0;
        int j1 = (axis == 1) ? dY - 1 : j0;
        int k1 = (axis == 2) ? dZ - 1 : k0;
        glm::vec3 p0 = vPos(i0, j0, k0);
        glm::vec3 p1 = vPos(i1, j1, k1);
        float dist = glm::length(p1 - p0);
        // Average spacing along this axis.
        float span = 0.0f;
        if (axis == 0) span = glm::length(vPos(dX-1, 0, 0) - vPos(0, 0, 0));
        else if (axis == 1) span = glm::length(vPos(0, dY-1, 0) - vPos(0, 0, 0));
        else span = glm::length(vPos(0, 0, dZ-1) - vPos(0, 0, 0));
        float spacing = span / static_cast<float>(std::max(1, n - 1));
        if (dist < 1e-6f) return true;
        return dist < spacing * 0.5f;
    };

    const bool iPeriodic = axisIsPeriodic(0);
    const bool jPeriodic = axisIsPeriodic(1);
    const bool kPeriodic = axisIsPeriodic(2);

    // --- half-space boundary checks ---
    // When the nearest vertex is on a grid boundary, the test point must
    // lie on the INTERIOR side.  The interior direction is the vector
    // from the boundary vertex toward its interior neighbour.
    glm::vec3 V = vPos(gi, gj, gk);

    if (!iPeriodic) {
        if (gi == 0) {
            glm::vec3 inward = vPos(1, gj, gk) - V;
            if (glm::dot(pos - V, inward) < 0.0f) return false;
        } else if (gi == dX - 1) {
            glm::vec3 inward = vPos(dX - 2, gj, gk) - V;
            if (glm::dot(pos - V, inward) < 0.0f) return false;
        }
    }
    if (!jPeriodic) {
        if (gj == 0) {
            glm::vec3 inward = vPos(gi, 1, gk) - V;
            if (glm::dot(pos - V, inward) < 0.0f) return false;
        } else if (gj == dY - 1) {
            glm::vec3 inward = vPos(gi, dY - 2, gk) - V;
            if (glm::dot(pos - V, inward) < 0.0f) return false;
        }
    }
    if (!kPeriodic) {
        if (gk == 0) {
            glm::vec3 inward = vPos(gi, gj, 1) - V;
            if (glm::dot(pos - V, inward) < 0.0f) return false;
        } else if (gk == dZ - 1) {
            glm::vec3 inward = vPos(gi, gj, dZ - 2) - V;
            if (glm::dot(pos - V, inward) < 0.0f) return false;
        }
    }

    // --- cell activity check ---
    int ci = std::min(gi, dX - 2);
    int cj = std::min(gj, dY - 2);
    int ck = std::min(gk, dZ - 2);
    if (ci < 0) ci = 0; if (cj < 0) cj = 0; if (ck < 0) ck = 0;

    int cX = dX - 1;
    int cY = dY - 1;
    size_t ciIdx = static_cast<size_t>(ci)
                 + static_cast<size_t>(cj) * static_cast<size_t>(cX)
                 + static_cast<size_t>(ck) * static_cast<size_t>(cX) * static_cast<size_t>(cY);
    if (ciIdx >= info.cellActive.size()) return true;
    return info.cellActive[ciIdx];
}

std::vector<glm::vec3> StreamlineSet::generateSeeds(const RenderMesh& mesh, int seedCount, const std::string& mode, double planePos, double jitter, int planeCountU, int planeCountV,
                                                    const std::vector<glm::vec3>* positionsOverride) {
    std::vector<glm::vec3> seeds;
    if (seedCount <= 0) return seeds;

    float minX = static_cast<float>(mesh.bounds.minX);
    float minY = static_cast<float>(mesh.bounds.minY);
    float minZ = static_cast<float>(mesh.bounds.minZ);
    float maxX = static_cast<float>(mesh.bounds.maxX);
    float maxY = static_cast<float>(mesh.bounds.maxY);
    float maxZ = static_cast<float>(mesh.bounds.maxZ);

    std::mt19937 rng(static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::uniform_real_distribution<float> jitterDist(-1.0f, 1.0f);

    if (mode == "Surface") {
        int numVerts = positionsOverride ? static_cast<int>(positionsOverride->size())
                                       : static_cast<int>(mesh.vertices.size() / 3);
        if (numVerts <= 0) return seeds;
        int actualCount = std::min(seedCount, numVerts);
        float stride = static_cast<float>(numVerts) / static_cast<float>(actualCount);
        for (int i = 0; i < actualCount; ++i) {
            int idx = static_cast<int>(i * stride);
            if (idx < 0) idx = 0;
            if (idx >= numVerts) idx = numVerts - 1;
            float x, y, z;
            if (positionsOverride) {
                x = (*positionsOverride)[idx].x;
                y = (*positionsOverride)[idx].y;
                z = (*positionsOverride)[idx].z;
            } else {
                x = mesh.vertices[idx * 3 + 0];
                y = mesh.vertices[idx * 3 + 1];
                z = mesh.vertices[idx * 3 + 2];
            }
            if (jitter > 0.0) {
                float dx = (maxX - minX) * 0.05f * jitterDist(rng) * static_cast<float>(jitter);
                float dy = (maxY - minY) * 0.05f * jitterDist(rng) * static_cast<float>(jitter);
                float dz = (maxZ - minZ) * 0.05f * jitterDist(rng) * static_cast<float>(jitter);
                x = std::clamp(x + dx, minX, maxX);
                y = std::clamp(y + dy, minY, maxY);
                z = std::clamp(z + dz, minZ, maxZ);
            }
            seeds.emplace_back(x, y, z);
        }
        return seeds;
    }

    if (mode == "PlaneXY" || mode == "PlaneXZ" || mode == "PlaneYZ") {
float pos = static_cast<float>(planePos);
        int nU = std::max(1, planeCountU);
        int nV = std::max(1, planeCountV);
        int totalGrid = nU * nV;
        int actualCount = totalGrid;
        if (actualCount <= 0) return seeds;
        float stride = static_cast<float>(totalGrid) / static_cast<float>(actualCount);

        float sx = maxX - minX;
        float sy = maxY - minY;
        float sz = maxZ - minZ;

        int count = 0;
        for (int iv = 0; iv < nV && count < actualCount; ++iv) {
            for (int iu = 0; iu < nU && count < actualCount; ++iu) {
                if ((iv * nU + iu) % std::max(1, static_cast<int>(stride)) != 0) continue;

                float tx = (iu + 0.5f) / static_cast<float>(nU);
                float ty = (iv + 0.5f) / static_cast<float>(nV);

                float x = minX + tx * sx;
                float y = minY + ty * sy;
                float z = minZ + pos * sz;

                if (mode == "PlaneXZ") {
                    y = minY + pos * sy;
                    z = minZ + ty * sz;
                } else if (mode == "PlaneYZ") {
                    x = minX + pos * sx;
                    y = minY + ty * sy;
                    z = minZ + tx * sz;
                }

                if (jitter > 0.0) {
                    float dx = 0.0f, dy = 0.0f, dz = 0.0f;
                    if (mode != "PlaneXZ" && mode != "PlaneYZ")
                        dx = sx * 0.05f * jitterDist(rng) * static_cast<float>(jitter);
                    if (mode != "PlaneXY" && mode != "PlaneYZ")
                        dy = sy * 0.05f * jitterDist(rng) * static_cast<float>(jitter);
                    if (mode != "PlaneXY" && mode != "PlaneXZ")
                        dz = sz * 0.05f * jitterDist(rng) * static_cast<float>(jitter);
                    if (dx != 0.0f) x = std::clamp(x + dx, minX, maxX);
                    if (dy != 0.0f) y = std::clamp(y + dy, minY, maxY);
                    if (dz != 0.0f) z = std::clamp(z + dz, minZ, maxZ);
                }

                seeds.emplace_back(x, y, z);
                ++count;
            }
        }
        return seeds;
    }

    int n = static_cast<int>(std::ceil(std::cbrt(static_cast<double>(seedCount))));
    n = std::max(1, n);
    int totalPts = n * n * n;
    int actualCount = std::min(seedCount, totalPts);
    if (actualCount <= 0) return seeds;

    float sx = maxX - minX;
    float sy = maxY - minY;
    float sz = maxZ - minZ;

    int nOut = std::min(n, static_cast<int>(std::ceil(std::cbrt(static_cast<double>(actualCount)))));
    if (nOut < 1) nOut = 1;

    std::vector<int> ixs, iys, izs;
    for (int i = 0; i < nOut; ++i) {
        int ix = (nOut == 1) ? (n / 2) : (i * (n - 1)) / (nOut - 1);
        ixs.push_back(ix);
        iys.push_back(ix);
        izs.push_back(ix);
    }
    // use same count for each axis
    iys = ixs;
    izs = ixs;

    int count = 0;
    for (int iz : izs) {
        for (int iy : iys) {
            for (int ix : ixs) {
                if (count >= actualCount) break;
                float tx = (ix + 0.5f) / n;
                float ty = (iy + 0.5f) / n;
                float tz = (iz + 0.5f) / n;
                float x = minX + tx * sx;
                float y = minY + ty * sy;
                float z = minZ + tz * sz;

                if (jitter > 0.0) {
                    float dx = sx * 0.05f * jitterDist(rng) * static_cast<float>(jitter);
                    float dy = sy * 0.05f * jitterDist(rng) * static_cast<float>(jitter);
                    float dz = sz * 0.05f * jitterDist(rng) * static_cast<float>(jitter);
                    x = std::clamp(x + dx, minX, maxX);
                    y = std::clamp(y + dy, minY, maxY);
                    z = std::clamp(z + dz, minZ, maxZ);
                }

                seeds.emplace_back(x, y, z);
                ++count;
            }
            if (count >= actualCount) break;
        }
        if (count >= actualCount) break;
    }
    return seeds;
}

glm::mat3 StreamlineSet::buildFrame(const glm::vec3& dir) {
    if (glm::length(dir) < 1e-8f) return glm::mat3(1.0f);
    glm::vec3 t = glm::normalize(dir);
    glm::vec3 up = std::abs(t.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 n = glm::normalize(glm::cross(t, up));
    glm::vec3 b = glm::cross(t, n);
    return glm::mat3(n, b, t);
}

std::vector<float> StreamlineSet::generateArrowhead(const glm::vec3& pos, const glm::vec3& dir, float height, float radius, int segments, float mag, const glm::vec3& comp) {
    std::vector<float> verts;
    float dirLen = glm::length(dir);
    if (dirLen < 1e-8f || height <= 0.0f || radius <= 0.0f || segments < 3) return verts;
    verts.reserve(static_cast<size_t>(segments) * 6 * 10);

    glm::mat3 frame = buildFrame(dir);
    glm::vec3 apex = pos + frame[2] * height;
    glm::vec3 baseCenter = pos - frame[2] * (height * 0.15f);

    std::vector<glm::vec3> base;
    base.reserve(segments + 1);
    for (int i = 0; i <= segments; ++i) {
        float angle = 2.0f * static_cast<float>(M_PI) * i / segments;
        base.push_back(baseCenter + (frame[0] * std::cos(angle) + frame[1] * std::sin(angle)) * radius);
    }

    auto push = [&](const glm::vec3& p, const glm::vec3& n) {
        verts.push_back(p.x); verts.push_back(p.y); verts.push_back(p.z);
        verts.push_back(mag);
        verts.push_back(comp.x); verts.push_back(comp.y); verts.push_back(comp.z);
        verts.push_back(n.x); verts.push_back(n.y); verts.push_back(n.z);
    };

    glm::vec3 coneDir = glm::normalize(dir);

    // Calculate correct slanted face normal for smooth cone shading
    float slantAngle = std::atan2(radius, height);
    for (int i = 0; i < segments; ++i) {
        float midAngle = 2.0f * static_cast<float>(M_PI) * (i + 0.5f) / segments;
        glm::vec3 radial = glm::normalize(frame[0] * std::cos(midAngle) + frame[1] * std::sin(midAngle));
        glm::vec3 faceNormal = glm::normalize(radial * std::cos(slantAngle) + frame[2] * std::sin(slantAngle));

        push(apex, faceNormal);
        push(base[i], faceNormal);
        push(base[i + 1], faceNormal);
    }

    // Base cap triangles
    for (int i = 0; i < segments; ++i) {
        push(baseCenter, -coneDir);
        push(base[i], -coneDir);
        push(base[i + 1], -coneDir);
    }

    return verts;
}

std::vector<float> StreamlineSet::computeArrowPlacement(float pathLength, float extent,
                                                        float spacingFraction, float arrowSize,
                                                        float taperFactor) {
    std::vector<float> positions;
    if (!(pathLength > 0.0f) || !(extent > 0.0f) || spacingFraction <= 0.0f) return positions;

    const float arrowHeight = arrowSize * extent;
    // Arrow would visually dominate the path.
    if (pathLength < 2.0f * arrowHeight) return positions;

    // Anti-overlap floor: arrows never closer than twice their height.
    const float targetSpacing = std::max(spacingFraction * extent, 2.0f * arrowHeight);
    // Keep arrows off the raw ends and out of the needle-thin tapered tips
    // (ribbon half-width shrinks with taperFactor towards both ends).
    const float margin = arrowHeight * (1.0f + taperFactor);
    const float usable = pathLength - 2.0f * margin;
    if (usable <= 0.0f) return positions;

    const int count = std::max(1, static_cast<int>(std::lround(usable / targetSpacing)));
    if (count == 1) {
        // Lone arrow goes mid-path, not at the start margin.
        positions.push_back(0.5f * pathLength);
        return positions;
    }
    const float actual = usable / static_cast<float>(count);
    positions.reserve(static_cast<size_t>(count));
    for (int k = 0; k < count; ++k)
        positions.push_back(margin + actual * static_cast<float>(k));
    return positions;
}

void StreamlineSet::initParticles(int count) {
    particles.clear();
    if (paths.empty() || count <= 0) return;
    count = std::min(count, 2000);

    // Weight path selection by arc length so longer paths get more particles.
    std::vector<float> weights;
    weights.reserve(paths.size());
    float totalWeight = 0.0f;
    for (const auto& p : paths) {
        float w = std::max(p.totalLength, 1e-6f);
        weights.push_back(w);
        totalWeight += w;
    }

    std::uniform_real_distribution<float> tDist(0.0f, 1.0f);
    particles.reserve(count);
    for (int i = 0; i < count; ++i) {
        float r = tDist(particleRng) * totalWeight;
        int pathIndex = 0;
        float acc = weights[0];
        while (pathIndex + 1 < static_cast<int>(paths.size()) && r > acc) {
            ++pathIndex;
            acc += weights[pathIndex];
        }
        particles.push_back({ pathIndex, tDist(particleRng) });
    }
}

void StreamlineSet::updateParticles(float dt, float speed) {
    for (auto& p : particles) {
        if (p.pathIndex < 0 || p.pathIndex >= static_cast<int>(paths.size())) continue;
        const auto& path = paths[p.pathIndex];
        if (path.points.size() < 2) continue;

        if (p.t < 0.0f || p.t >= 1.0f) p.t = p.t - std::floor(p.t);

        glm::vec3 dummyPos(0.0f);
        float localSpeed = 0.0f;
        samplePath(path, p.t, dummyPos, localSpeed);
        if (path.speedAtPoint.empty()) localSpeed = 1.0f;

        float dtNorm = (dt * speed * localSpeed) / std::max(path.totalLength, 1e-6f);
        if (!std::isfinite(dtNorm)) continue;
        // Clamp per-frame advance to avoid teleporting/skipping path features.
        dtNorm = std::clamp(dtNorm, -0.05f, 0.05f);
        p.t += dtNorm;

        p.t = p.t - std::floor(p.t);
    }
}

void StreamlineSet::buildParticleVertices(std::vector<float>& outVerts) {
    outVerts.clear();
    outVerts.reserve(particles.size() * 9);
    // Cache per-path max magnitude once per call (was rescanned per particle).
    std::vector<float> pathMaxCache(paths.size(), 0.0f);
    for (size_t pi = 0; pi < paths.size(); ++pi) {
        float m = 0.0f;
        for (float v : paths[pi].speedAtPoint) m = std::max(m, v);
        pathMaxCache[pi] = (m < 1e-12f) ? 1.0f : m;
    }
    for (const auto& p : particles) {
        if (p.pathIndex < 0 || p.pathIndex >= static_cast<int>(paths.size())) continue;
        const auto& path = paths[p.pathIndex];
        if (path.points.size() < 2) continue;

        glm::vec3 pos(0.0f);
        float mag = 0.0f;
        samplePath(path, p.t, pos, mag);

        // Tangent along the path for oriented particles/cones.
        glm::vec3 tangent(0.0f, 0.0f, 1.0f);
        if (path.points.size() >= 2) {
            const auto& cum = path.cumulativeLength;
            const int n = static_cast<int>(path.points.size());
            float total = std::max(path.totalLength, 1e-6f);
            float s = std::clamp(p.t, 0.0f, 1.0f) * total;
            int idx = 0;
            if (cum.size() == static_cast<size_t>(n)) {
                idx = static_cast<int>(std::upper_bound(cum.begin(), cum.end(), s) - cum.begin()) - 1;
            } else {
                idx = static_cast<int>(s / total * (n - 1));
            }
            idx = std::clamp(idx, 0, n - 2);
            tangent = path.points[idx + 1] - path.points[idx];
            float tlen = glm::length(tangent);
            if (tlen > 1e-12f) tangent /= tlen;
        }

        float pathMaxMag = pathMaxCache[p.pathIndex];
        float radiusFactor = std::clamp(mag / pathMaxMag, 0.15f, 2.0f);

        outVerts.push_back(pos.x);
        outVerts.push_back(pos.y);
        outVerts.push_back(pos.z);
        outVerts.push_back(mag);
        outVerts.push_back(p.t);   // path parameter for end-fade in particle.frag
        outVerts.push_back(tangent.x);
        outVerts.push_back(tangent.y);
        outVerts.push_back(tangent.z);
        outVerts.push_back(radiusFactor);
    }
}

void StreamlineSet::clearParticles() {
    particles.clear();
}

void StreamlineSet::teardownParticles() {
    clearParticles();
    paths.clear();
}

void StreamlineSet::teardownGL() {
    vao.reset();
    vbo.reset();
    seedVao.reset();
    seedVbo.reset();
    arrowVao.reset();
    arrowVbo.reset();
    lineCount = 0;
    seedCount = 0;
    arrowCount = 0;
    renderAsLines = false;
}

void StreamlineSet::shutdown() {
    teardownGL();
    teardownParticles();
}

StreamlineSet::StreamlineResult StreamlineSet::computeStatic(
        const RenderMesh& mesh, int seedCountParam, float stepSize, int maxSteps,
        const std::string& fieldName, const std::string& mode, const std::string& direction,
        double planePos, double jitter, int planeCountU, int planeCountV,
        bool showArrows, float arrowSpacingFrac, float arrowSize,
        float ribbonWidth, float taperFactor, int style, float maxPathLength,
        const postfields::ComputedSnapshot* snap) {
    StreamlineResult result;
    try {
        auto field = FieldStore::resolveVector(mesh, fieldName, FieldStore::Placement::Vertex, snap);
        if (!field.data || field.count == 0)
            field = FieldStore::resolveVector(mesh, fieldName, FieldStore::Placement::CellCenter, snap);
        if (!field.data || field.count == 0) return result;
        const bool isCell = field.isCell;

        std::vector<glm::vec3> positions;
        if (isCell) positions = mesh.cellCenters;
        else {
            positions.reserve(mesh.vertices.size() / 3);
            for (size_t i = 0; i + 2 < mesh.vertices.size(); i += 3)
                positions.emplace_back(mesh.vertices[i], mesh.vertices[i+1], mesh.vertices[i+2]);
        }
        int limit = std::min((int)positions.size(), (int)field.count);
        if (limit <= 0) return result;
        positions.resize(limit);
        std::vector<glm::vec3> values(field.data, field.data + limit);

        std::optional<streamline::StructuredDims> dims;
        if (mesh.gridDimX > 0 && mesh.gridDimY > 0 && mesh.gridDimZ > 0) {
            long long prod = (long long)mesh.gridDimX * mesh.gridDimY * mesh.gridDimZ;
            if (!isCell && prod == limit) dims = {mesh.gridDimX, mesh.gridDimY, mesh.gridDimZ};
            else if (isCell && mesh.gridDimX > 1 && mesh.gridDimY > 1 && mesh.gridDimZ > 1) {
                int cx = mesh.gridDimX - 1, cy = mesh.gridDimY - 1, cz = mesh.gridDimZ - 1;
                if ((long long)cx * cy * cz == limit) dims = {cx, cy, cz};
            }
        }
        auto domainExp = streamline::FieldDomain::build(std::move(positions), std::move(values), dims);
        if (!domainExp) return result; // invalid domain: empty result, no UB
        const auto& domain = *domainExp;

        streamline::IntegrationParameters params;
        params.stepSize = stepSize;
        params.maxSteps = maxSteps;
        params.maxPathLength = maxPathLength;
        params.direction = (direction == "Forward") ? streamline::Direction::Forward
                         : (direction == "Backward") ? streamline::Direction::Backward
                         : streamline::Direction::Both;
        params.seedMode = (mode == "Surface") ? streamline::SeedMode::Surface
                      : (mode == "PlaneXY") ? streamline::SeedMode::PlaneXY
                      : (mode == "PlaneXZ") ? streamline::SeedMode::PlaneXZ
                      : (mode == "PlaneYZ") ? streamline::SeedMode::PlaneYZ
                      : streamline::SeedMode::Volume;
        params.seedCount = seedCountParam;
        params.planePos = planePos; params.jitter = jitter;
        params.planeCountU = planeCountU; params.planeCountV = planeCountV;
        auto validated = streamline::validateParameters(params, domain);
        if (!validated) return result;
        params = *validated;

        std::atomic<bool> cancel{false};
        auto data = streamline::integrateAll(domain, params, cancel);
        if (data.empty()) return result;

        const float extent = (float)mesh.bounds.extent;
        streamline::GeometryConfig gcfg;
        gcfg.style = style;
        gcfg.baseWidth = extent * ribbonWidth;
        gcfg.taperFactor = taperFactor;
        gcfg.showArrows = showArrows;
        gcfg.arrowSize = arrowSize;
        gcfg.arrowSpacingFrac = arrowSpacingFrac;
        gcfg.extent = extent;
        auto geom = streamline::buildGeometry(data, gcfg);
        if (geom.budgetExceeded) return result;

        result.verts = std::move(geom.verts);
        result.seedVerts = std::move(geom.seedVerts);
        result.arrowVerts = std::move(geom.arrowVerts);
        result.lineCount = geom.lineCount;
        result.seedCount = geom.seedCount;
        result.arrowCount = geom.arrowCount;
        result.magMin = geom.ranges.magMin; result.magMax = geom.ranges.magMax;
        for (int c = 0; c < 3; ++c) {
            result.compMin[c] = geom.ranges.compMin[c];
            result.compMax[c] = geom.ranges.compMax[c];
        }
        result.tubeBaseWidth = gcfg.baseWidth;
        result.style = style;
        result.paths.reserve(data.paths.size());
        for (auto& np : data.paths) {
            StreamlinePath lp;
            lp.points = np.points;
            lp.speedAtPoint = np.speed;
            lp.cumulativeLength = np.arcLength;
            lp.totalLength = np.totalLength;
            result.paths.push_back(std::move(lp));
        }
        return result;
    } catch (...) {
        return StreamlineResult{}; // never let a worker thread terminate
    }
}

StreamlineSet::StreamlineResult StreamlineSet::compute(const RenderMesh& mesh, int seedCountParam, float stepSize, int maxSteps,
                                  const std::string& fieldName, const std::string& mode, const std::string& direction,
                                  double planePos, double jitter, int planeCountU, int planeCountV,
                                  bool showArrows, float arrowSpacingFrac, float arrowSize,
                                  float ribbonWidth, float taperFactor, int style, float maxPathLength,
                                  const postfields::ComputedSnapshot* snap) {
    return computeStatic(mesh, seedCountParam, stepSize, maxSteps, fieldName, mode, direction,
                         planePos, jitter, planeCountU, planeCountV, showArrows, arrowSpacingFrac,
                         arrowSize, ribbonWidth, taperFactor, style, maxPathLength, snap);
}

void StreamlineSet::uploadGL(StreamlineSet::StreamlineResult&& res, bool showArrows, float arrowSize) {
    teardownGL();

    magMin = res.magMin;
    magMax = res.magMax;
    for (int c = 0; c < 3; ++c) {
        compMin[c] = res.compMin[c];
        compMax[c] = res.compMax[c];
    }
    m_tubeBaseWidth = res.tubeBaseWidth;
    lineCount = res.lineCount;
    renderAsLines = (res.style == 0);
    seedCount = res.seedCount;
    paths = std::move(res.paths);
    particles.clear();

    if (!res.verts.empty()) {
        setupVertexBuffer(vao, vbo, res.verts.data(), res.verts.size() * sizeof(float), 10 * sizeof(float),
                          { { 0, 3, 0 }, { 1, 1, 3 * sizeof(float) }, { 2, 3, 4 * sizeof(float) }, { 3, 3, 7 * sizeof(float) } }, GL_STATIC_DRAW);
    }

    if (!res.seedVerts.empty()) {
        setupVertexBuffer(seedVao, seedVbo, res.seedVerts.data(), res.seedVerts.size() * sizeof(float), 3 * sizeof(float),
                          { { 0, 3, 0 } }, GL_STATIC_DRAW);
    }

    if (showArrows && !res.arrowVerts.empty()) {
        arrowCount = res.arrowCount;

        setupVertexBuffer(arrowVao, arrowVbo, res.arrowVerts.data(), res.arrowVerts.size() * sizeof(float), 10 * sizeof(float),
                          { { 0, 3, 0 }, { 1, 1, 3 * sizeof(float) }, { 2, 3, 4 * sizeof(float) }, { 3, 3, 7 * sizeof(float) } }, GL_STATIC_DRAW);
    }
}

void StreamlineSet::rebuild(const RenderMesh& mesh, int seedCountParam, float stepSize, int maxSteps,
                                  const std::string& fieldName, const std::string& mode, const std::string& direction,
                                  double planePos, double jitter, int planeCountU, int planeCountV,
                                  bool showArrows, float arrowSpacingFrac, float arrowSize,
                                  float ribbonWidth, float taperFactor, int style, float maxPathLength,
                                  const postfields::ComputedSnapshot* snap) {
    auto result = compute(mesh, seedCountParam, stepSize, maxSteps, fieldName, mode, direction,
                          planePos, jitter, planeCountU, planeCountV,
                          showArrows, arrowSpacingFrac, arrowSize, ribbonWidth, taperFactor, style, maxPathLength, snap);
    uploadGL(std::move(result), showArrows, arrowSize);
}
