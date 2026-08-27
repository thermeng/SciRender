#include "render/streamlines/StreamlineSet.h"
#include "core/mesh_loader.h"
#include "core/FieldResolver.h"

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

glm::vec3 StreamlineSet::evalFieldNearest(const RenderMesh& mesh, const glm::vec3& pos, const glm::vec3* data, int count, int searchCount) {
    if (!data || count <= 0) return glm::vec3(0.0f);

    const float* verts = mesh.vertices.data();
    const int maxSamples = 4096;
    const int stride = std::max(1, count / maxSamples);

    // Estimate characteristic point spacing from bounding box volume and point count.
    // Use 2x this spacing as the distance cutoff: points within the mesh should be
    // closer than this, while points in holes/gaps will be farther away.
    const float vol = std::abs(static_cast<float>(
        (mesh.bounds.maxX - mesh.bounds.minX) *
        (mesh.bounds.maxY - mesh.bounds.minY) *
        (mesh.bounds.maxZ - mesh.bounds.minZ)));
    const float avgSpacing = (count > 0 && vol > 0.0f)
        ? std::pow(vol / static_cast<float>(count), 1.0f / 3.0f)
        : static_cast<float>(mesh.bounds.extent);
    const float cutoffDistSq = 4.0f * avgSpacing * avgSpacing;

    float bestD2 = std::numeric_limits<float>::max();
    int bestIdx = -1;
    for (int i = 0; i < count; i += stride) {
        int vi = i * 3;
        if (vi + 2 >= static_cast<int>(mesh.vertices.size())) break;
        float dx = verts[vi + 0] - pos.x;
        float dy = verts[vi + 1] - pos.y;
        float dz = verts[vi + 2] - pos.z;
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD2) {
            bestD2 = d2;
            bestIdx = i;
        }
    }

    if (bestIdx < 0 || bestD2 > cutoffDistSq) return glm::vec3(0.0f);
    if (magSq(data[bestIdx]) < 1e-12f) return glm::vec3(0.0f);
    return data[bestIdx];
}

StreamlineSet::StructuredGridInfo StreamlineSet::buildStructuredGridInfo(const RenderMesh& mesh, const glm::vec3* data, int count) {
    StreamlineSet::StructuredGridInfo info;
    info.data = data;
    info.count = count;

    int dimX = mesh.gridDimX;
    int dimY = mesh.gridDimY;
    int dimZ = mesh.gridDimZ;
    if (dimX <= 0 || dimY <= 0 || dimZ <= 0) return info;

    int numVerts = static_cast<int>(mesh.vertices.size() / 3);
    int limit = std::min(numVerts, count);
    if (limit <= 0 || dimX * dimY * dimZ != limit) return info;

    const float* verts = mesh.vertices.data();

    std::vector<float> allX, allY, allZ;
    allX.reserve(limit);
    allY.reserve(limit);
    allZ.reserve(limit);
    for (int i = 0; i < limit; ++i) {
        allX.push_back(verts[i * 3 + 0]);
        allY.push_back(verts[i * 3 + 1]);
        allZ.push_back(verts[i * 3 + 2]);
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

    info.xs = std::move(allX);
    info.ys = std::move(allY);
    info.zs = std::move(allZ);
    info.dimX = dimX;
    info.dimY = dimY;
    info.dimZ = dimZ;

    // Build per-cell activity mask: a cell is active iff ALL 8 corner vertices
    // have non-zero field magnitude.  This lets streamlines respect non-rectangular
    // domain boundaries (cylinders, L-shapes, etc.) stored as structured grids.
    const int cX = dimX - 1, cY = dimY - 1, cZ = dimZ - 1;
    info.cellActive.resize(static_cast<size_t>(cX) * cY * cZ, false);
    auto cellIdx = [cX, cY](int i, int j, int k) { return i + j * cX + k * cX * cY; };
    auto vertIdx = [dimX, dimY](int i, int j, int k) { return i + j * dimX + k * dimX * dimY; };
    const float magThreshSq = 1e-6f * 1e-6f;
    for (int k = 0; k < cZ; ++k) {
        for (int j = 0; j < cY; ++j) {
            for (int i = 0; i < cX; ++i) {
                bool active = true;
                for (int dk = 0; dk <= 1 && active; ++dk)
                    for (int dj = 0; dj <= 1 && active; ++dj)
                        for (int di = 0; di <= 1 && active; ++di) {
                            const glm::vec3& v = data[vertIdx(i + di, j + dj, k + dk)];
                            if (magSq(v) < magThreshSq) active = false;
                        }
                info.cellActive[cellIdx(i, j, k)] = active;
            }
        }
    }

    return info;
}

glm::vec3 StreamlineSet::evalFieldTrilinear(const StreamlineSet::StructuredGridInfo& info, const glm::vec3& pos) {
    if (!info.data) return glm::vec3(0.0f);

    if (info.dimX <= 1 || info.dimY <= 1 || info.dimZ <= 1) {
        // Degenerate dimension(s): fall back to nearest-neighbor sampling.
        int i = std::clamp(static_cast<int>(
            std::upper_bound(info.xs.begin(), info.xs.end(), pos.x) - info.xs.begin()) - 1, 0, info.dimX - 1);
        int j = std::clamp(static_cast<int>(
            std::upper_bound(info.ys.begin(), info.ys.end(), pos.y) - info.ys.begin()) - 1, 0, info.dimY - 1);
        int k = std::clamp(static_cast<int>(
            std::upper_bound(info.zs.begin(), info.zs.end(), pos.z) - info.zs.begin()) - 1, 0, info.dimZ - 1);
        auto idx = [&](int ii, int jj, int kk) { return ii + jj * info.dimX + kk * info.dimX * info.dimY; };
        return info.data[idx(i, j, k)];
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
    return info.cellActive[static_cast<size_t>(ci) + cj * cX + ck * cX * cY];
}

std::vector<glm::vec3> StreamlineSet::generateSeeds(const RenderMesh& mesh, int seedCount, const std::string& mode, double planePos, double jitter, int planeCountU, int planeCountV) {
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
        int numVerts = static_cast<int>(mesh.vertices.size() / 3);
        int step = std::max(1, numVerts / seedCount);
        for (int i = 0; i < numVerts && static_cast<int>(seeds.size()) < seedCount; i += step) {
            float x = mesh.vertices[i * 3 + 0];
            float y = mesh.vertices[i * 3 + 1];
            float z = mesh.vertices[i * 3 + 2];
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
        float sx = maxX - minX;
        float sy = maxY - minY;
        float sz = maxZ - minZ;

        for (int ix = 0; ix < nU && static_cast<int>(seeds.size()) < nU * nV; ++ix) {
            for (int iy = 0; iy < nV && static_cast<int>(seeds.size()) < nU * nV; ++iy) {
                float tx = (ix + 0.5f) / nU;
                float ty = (iy + 0.5f) / nV;
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
                    float dx = (maxX - minX) * 0.05f * jitterDist(rng) * static_cast<float>(jitter);
                    float dy = (maxY - minY) * 0.05f * jitterDist(rng) * static_cast<float>(jitter);
                    float dz = (maxZ - minZ) * 0.05f * jitterDist(rng) * static_cast<float>(jitter);
                    x = std::clamp(x + dx, minX, maxX);
                    y = std::clamp(y + dy, minY, maxY);
                    z = std::clamp(z + dz, minZ, maxZ);
                }

                seeds.emplace_back(x, y, z);
            }
        }
        return seeds;
    }

    int n = static_cast<int>(std::ceil(std::cbrt(static_cast<double>(seedCount))));
    n = std::max(1, n);
    float sx = maxX - minX;
    float sy = maxY - minY;
    float sz = maxZ - minZ;

    for (int ix = 0; ix < n && static_cast<int>(seeds.size()) < seedCount; ++ix) {
        for (int iy = 0; iy < n && static_cast<int>(seeds.size()) < seedCount; ++iy) {
            for (int iz = 0; iz < n && static_cast<int>(seeds.size()) < seedCount; ++iz) {
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
            }
        }
    }
    return seeds;
}

glm::mat3 StreamlineSet::buildFrame(const glm::vec3& dir) {
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
    std::uniform_int_distribution<int> pathDist(0, static_cast<int>(paths.size()) - 1);
    std::uniform_real_distribution<float> tDist(0.0f, 1.0f);
    particles.reserve(count);
    for (int i = 0; i < count; ++i) {
        particles.push_back({ pathDist(particleRng), tDist(particleRng) });
    }
}

void StreamlineSet::updateParticles(float dt, float speed) {
    for (auto& p : particles) {
        if (p.pathIndex < 0 || p.pathIndex >= static_cast<int>(paths.size())) continue;
        const auto& path = paths[p.pathIndex];
        if (path.points.size() < 2) continue;
        int n = static_cast<int>(path.points.size());

        if (p.t < 0.0f || p.t >= 1.0f) p.t = p.t - std::floor(p.t);

        float scaledT = p.t * static_cast<float>(n - 1);
        int idx = std::min(static_cast<int>(scaledT), n - 2);
        if (idx < 0) idx = 0;
        float frac = scaledT - static_cast<float>(idx);
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        float localSpeed = path.speedAtPoint[idx] * (1.0f - frac) + path.speedAtPoint[idx + 1] * frac;
        p.t += (dt * speed * localSpeed) / std::max(path.totalLength, 1e-6f);

        if (p.t >= 1.0f) p.t -= std::floor(p.t);
    }
}

void StreamlineSet::buildParticleVertices(std::vector<float>& outVerts) {
    outVerts.clear();
    outVerts.reserve(particles.size() * 5);
    for (const auto& p : particles) {
        if (p.pathIndex < 0 || p.pathIndex >= static_cast<int>(paths.size())) continue;
        const auto& path = paths[p.pathIndex];
        const auto& pts = path.points;
        const auto& spd = path.speedAtPoint;
        if (pts.size() < 2) continue;

        float scaledT = p.t * static_cast<float>(pts.size() - 1);
        int idx = static_cast<int>(scaledT);
        float frac = scaledT - static_cast<float>(idx);
        if (idx >= static_cast<int>(pts.size()) - 1) {
            idx = static_cast<int>(pts.size()) - 2;
            frac = 1.0f;
        }
        glm::vec3 pos = glm::mix(pts[idx], pts[idx + 1], frac);

        float mag = 0.0f;
        if (!spd.empty()) {
            float sA = spd[std::min(idx, static_cast<int>(spd.size()) - 1)];
            float sB = spd[std::min(idx + 1, static_cast<int>(spd.size()) - 1)];
            mag = sA + (sB - sA) * frac;
        }

        outVerts.push_back(pos.x);
        outVerts.push_back(pos.y);
        outVerts.push_back(pos.z);
        outVerts.push_back(mag);
        outVerts.push_back(p.t);   // path parameter for end-fade in particle.frag
    }
}

void StreamlineSet::teardownParticles() {
    particles.clear();
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
}

void StreamlineSet::shutdown() {
    teardownGL();
    teardownParticles();
}

StreamlineSet::StreamlineResult StreamlineSet::compute(const RenderMesh& mesh, int seedCountParam, float stepSize, int maxSteps,
                                 const std::string& fieldName, const std::string& mode, const std::string& direction,
                                 double planePos, double jitter, int planeCountU, int planeCountV,
                                 bool showArrows, float arrowSpacingFrac, float arrowSize,
                                 float ribbonWidth, float taperFactor) {
    StreamlineResult result;

    auto field = FieldResolver::resolveVector(mesh, fieldName, 0);
    if (!field.data || field.count == 0) return result;
    const glm::vec3* data = field.data;
    size_t count = field.count;

    int numVerts = static_cast<int>(mesh.vertices.size() / 3);
    const int limit = std::min(numVerts, static_cast<int>(count));
    if (limit <= 0) return result;

    const float extent = static_cast<float>(mesh.bounds.extent);
    const float h = std::max(stepSize, 1e-8f);
    const float magThresh = 1e-6f;

    StructuredGridInfo grid = buildStructuredGridInfo(mesh, data, limit);
    bool hasGrid = grid.dimX > 0;
    bool nonCartesianGrid = false;

    if (!hasGrid && mesh.gridDimX > 0 && mesh.gridDimY > 0 && mesh.gridDimZ > 0
        && mesh.gridDimX * mesh.gridDimY * mesh.gridDimZ == limit) {
        grid.dimX = mesh.gridDimX;
        grid.dimY = mesh.gridDimY;
        grid.dimZ = mesh.gridDimZ;
        grid.data = data;
        grid.count = limit;
        grid.verts = mesh.vertices.data();
        grid.vertCount = limit;

        const int cX = grid.dimX - 1, cY = grid.dimY - 1, cZ = grid.dimZ - 1;
        grid.cellActive.resize(static_cast<size_t>(cX) * cY * cZ, false);
        auto vIdx = [dX = grid.dimX, dY = grid.dimY](int i, int j, int k) {
            return i + j * dX + k * dX * dY;
        };
        const float magThreshSq = 1e-6f * 1e-6f;
        for (int k = 0; k < cZ; ++k)
            for (int j = 0; j < cY; ++j)
                for (int i = 0; i < cX; ++i) {
                    bool active = true;
                    for (int dk = 0; dk <= 1 && active; ++dk)
                        for (int dj = 0; dj <= 1 && active; ++dj)
                            for (int di = 0; di <= 1 && active; ++di)
                                if (magSq(data[vIdx(i + di, j + dj, k + dk)]) < magThreshSq)
                                    active = false;
                    grid.cellActive[i + j * cX + k * cX * cY] = active;
                }
        hasGrid = true;
        nonCartesianGrid = true;

        const float vol = std::abs(static_cast<float>(
            (mesh.bounds.maxX - mesh.bounds.minX) *
            (mesh.bounds.maxY - mesh.bounds.minY) *
            (mesh.bounds.maxZ - mesh.bounds.minZ)));
        grid.avgSpacing = (limit > 0 && vol > 0.0f)
            ? std::pow(vol / static_cast<float>(limit), 1.0f / 3.0f)
            : 1.0f;
    }

    auto evalField = [&](const glm::vec3& pos) -> glm::vec3 {
        if (hasGrid) {
            if (!nonCartesianGrid)
                return evalFieldTrilinear(grid, pos);
            glm::vec3 v = evalFieldNearest(mesh, pos, data, limit, limit);
            if (!isInsideDomain(grid, pos)) return glm::vec3(0.0f);
            return v;
        }
        return evalFieldNearest(mesh, pos, data, limit, limit);
    };

    std::vector<glm::vec3> seeds = generateSeeds(mesh, seedCountParam, mode, planePos, jitter, planeCountU, planeCountV);
    if (seeds.empty()) return result;

    if (hasGrid) {
        const float magThreshSq = magThresh * magThresh;
        seeds.erase(
            std::remove_if(seeds.begin(), seeds.end(), [&](const glm::vec3& s) {
                if (!isInsideDomain(grid, s)) return true;
                glm::vec3 v = evalField(s);
                return magSq(v) < magThreshSq;
            }),
            seeds.end());
        if (seeds.empty()) return result;
    } else {
        const float magThreshSq = magThresh * magThresh;
        seeds.erase(
            std::remove_if(seeds.begin(), seeds.end(), [&](const glm::vec3& s) {
                glm::vec3 v = evalFieldNearest(mesh, s, data, limit, limit);
                return magSq(v) < magThreshSq;
            }),
            seeds.end());
        if (seeds.empty()) return result;
    }

    float mn = std::numeric_limits<float>::max();
    float mx = -std::numeric_limits<float>::max();
    float cMin[3] = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
    float cMax[3] = { -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };

    std::vector<glm::vec3> arrowPositions;
    std::vector<glm::vec3> arrowDirections;
    std::vector<float> arrowMagnitudes;
    std::vector<glm::vec3> arrowFieldVecs;

    const size_t estimatedSegments = seeds.size() * 2 * maxSteps;
    result.verts.reserve(estimatedSegments * 6 * 7);
    result.seedVerts.reserve(seeds.size() * 3);
    if (showArrows && arrowSpacingFrac > 0.0f) {
        // Rough upper bound: path length budget (maxSteps * h) divided by the
        // minimum world spacing implied by the extent fraction.
        const float minSpacing = std::max(arrowSpacingFrac * extent, 2.0f * arrowSize * extent);
        const size_t arrowsPerPath = minSpacing > 1e-12f
            ? static_cast<size_t>(static_cast<float>(2 * std::max(maxSteps, 1)) * h / minSpacing) + 1
            : 0;
        const size_t estimatedArrows = seeds.size() * arrowsPerPath;
        arrowPositions.reserve(estimatedArrows);
        arrowDirections.reserve(estimatedArrows);
        arrowMagnitudes.reserve(estimatedArrows);
        arrowFieldVecs.reserve(estimatedArrows);
    }

    for (const auto& seed : seeds) {
        result.seedVerts.push_back(seed.x);
        result.seedVerts.push_back(seed.y);
        result.seedVerts.push_back(seed.z);

        std::vector<glm::vec3> forwardPts;
        std::vector<glm::vec3> forwardFieldVecs;
        std::vector<glm::vec3> backwardPts;
        std::vector<glm::vec3> backwardFieldVecs;

        auto integrateDir = [&](int dir, std::vector<glm::vec3>& outPts, std::vector<glm::vec3>& outFieldVecs) {
            glm::vec3 pos = seed;
            outPts.push_back(pos);
            outFieldVecs.push_back(evalField(pos));

            for (int iter = 0; iter < maxSteps; ++iter) {
                auto stepDir = [&](const glm::vec3& p) -> glm::vec3 {
                    glm::vec3 v = evalField(p);
                    float m = std::sqrt(magSq(v));
                    if (m < magThresh) return glm::vec3(0.0f);
                    return static_cast<float>(dir) * (v / m);
                };

                glm::vec3 k1 = stepDir(pos);
                if (glm::length(k1) < 1e-12f) break;

                glm::vec3 p2 = pos + 0.5f * h * k1;
                glm::vec3 k2 = stepDir(p2);
                if (glm::length(k2) < 1e-12f) break;

                glm::vec3 p3 = pos + 0.5f * h * k2;
                glm::vec3 k3 = stepDir(p3);
                if (glm::length(k3) < 1e-12f) break;

                glm::vec3 p4 = pos + h * k3;
                glm::vec3 k4 = stepDir(p4);
                if (glm::length(k4) < 1e-12f) break;

                glm::vec3 newPos = pos + (h / 6.0f) * (k1 + 2.0f * k2 + 2.0f * k3 + k4);

                {
                    glm::vec3 nv = evalField(newPos);
                    float nm = std::sqrt(magSq(nv));
                    if (nm < magThresh) break;

                    if (newPos.x < mesh.bounds.minX || newPos.x > mesh.bounds.maxX ||
                        newPos.y < mesh.bounds.minY || newPos.y > mesh.bounds.maxY ||
                        newPos.z < mesh.bounds.minZ || newPos.z > mesh.bounds.maxZ) {
                        break;
                    }

                    if (hasGrid && !isInsideDomain(grid, newPos)) break;

                    outFieldVecs.push_back(nv);
                }

                outPts.push_back(newPos);
                pos = newPos;
            }
        };

        bool doForward = (direction == "Forward" || direction == "Both");
        bool doBackward = (direction == "Backward" || direction == "Both");

        if (doForward) {
            integrateDir(+1, forwardPts, forwardFieldVecs);
        }
        if (doBackward) {
            integrateDir(-1, backwardPts, backwardFieldVecs);
        }

        std::vector<glm::vec3> mergedPts;
        std::vector<float> mergedSpeeds;
        std::vector<glm::vec3> mergedFieldVecs;

        if (direction == "Both" && !forwardPts.empty() && !backwardPts.empty()) {
            mergedPts.reserve(backwardPts.size() + forwardPts.size() - 1);
            mergedPts.insert(mergedPts.end(), backwardPts.rbegin() + 1, backwardPts.rend());
            mergedPts.insert(mergedPts.end(), forwardPts.begin(), forwardPts.end());

            auto speedsFromField = [&](const std::vector<glm::vec3>& fv) {
                std::vector<float> s;
                s.reserve(fv.size());
                for (const auto& v : fv) s.push_back(std::sqrt(magSq(v)));
                return s;
            };

            std::vector<float> backwardSpeeds = speedsFromField(backwardFieldVecs);
            std::vector<float> forwardSpeeds = speedsFromField(forwardFieldVecs);

            mergedSpeeds.reserve(backwardSpeeds.size() + forwardSpeeds.size() - 1);
            mergedSpeeds.insert(mergedSpeeds.end(), backwardSpeeds.rbegin() + 1, backwardSpeeds.rend());
            mergedSpeeds.insert(mergedSpeeds.end(), forwardSpeeds.begin(), forwardSpeeds.end());

            mergedFieldVecs.reserve(backwardFieldVecs.size() + forwardFieldVecs.size() - 1);
            mergedFieldVecs.insert(mergedFieldVecs.end(), backwardFieldVecs.rbegin() + 1, backwardFieldVecs.rend());
            mergedFieldVecs.insert(mergedFieldVecs.end(), forwardFieldVecs.begin(), forwardFieldVecs.end());
        } else if (!forwardPts.empty()) {
            mergedPts = forwardPts;
            for (const auto& fv : forwardFieldVecs) {
                mergedSpeeds.push_back(std::sqrt(magSq(fv)));
            }
            mergedFieldVecs = forwardFieldVecs;
        } else if (!backwardPts.empty()) {
            mergedPts = backwardPts;
            for (const auto& fv : backwardFieldVecs) {
                mergedSpeeds.push_back(std::sqrt(magSq(fv)));
            }
            mergedFieldVecs = backwardFieldVecs;
        }

        if (mergedPts.size() < 3) continue;

        float totalLength = 0.0f;
        for (size_t i = 0; i + 1 < mergedPts.size(); ++i) {
            totalLength += glm::length(mergedPts[i + 1] - mergedPts[i]);
        }
        if (totalLength < 1e-6f) totalLength = 1.0f;

        result.paths.push_back({ mergedPts, mergedSpeeds, totalLength });

        const float baseWidth = static_cast<float>(extent * ribbonWidth);

        for (size_t i = 0; i + 1 < mergedPts.size(); ++i) {
            glm::vec3 a = mergedPts[i];
            glm::vec3 b = mergedPts[i + 1];
            glm::vec3 tangent = b - a;
            float segLen = glm::length(tangent);
            if (segLen < 1e-12f) continue;
            tangent /= segLen;

            float tY = std::abs(tangent.y);
            float upThreshold = 0.9f;
            glm::vec3 side = tY < upThreshold ?
                glm::normalize(glm::cross(tangent, glm::vec3(0.0f, 1.0f, 0.0f))) :
                glm::normalize(glm::cross(tangent, glm::vec3(1.0f, 0.0f, 0.0f)));
            if (std::abs(glm::length(side)) < 1e-12f) {
                side = glm::vec3(0.0f, 0.0f, 1.0f);
            }
            glm::vec3 normal = glm::normalize(glm::cross(tangent, side));

            float magA = mergedSpeeds[i];
            float magB = mergedSpeeds[i + 1];

            float tA = (i) / static_cast<float>(mergedPts.size());
            float tB = (i + 1) / static_cast<float>(mergedPts.size());
            float taperA = 1.0f - taperFactor * std::abs(2.0f * tA - 1.0f);
            float taperB = 1.0f - taperFactor * std::abs(2.0f * tB - 1.0f);

            glm::vec3 va = a - side * (baseWidth * taperA);
            glm::vec3 vb = a + side * (baseWidth * taperA);
            glm::vec3 vc = b - side * (baseWidth * taperB);
            glm::vec3 vd = b + side * (baseWidth * taperB);

            auto pushQuadVertex = [&](const glm::vec3& p, float rawMag, const glm::vec3& rawComp) {
                result.verts.push_back(p.x); result.verts.push_back(p.y); result.verts.push_back(p.z);
                result.verts.push_back(rawMag);
                result.verts.push_back(rawComp.x); result.verts.push_back(rawComp.y); result.verts.push_back(rawComp.z);
                result.verts.push_back(normal.x); result.verts.push_back(normal.y); result.verts.push_back(normal.z);
                if (rawMag < mn) mn = rawMag;
                if (rawMag > mx) mx = rawMag;
                for (int c = 0; c < 3; ++c) {
                    if (rawComp[c] < cMin[c]) cMin[c] = rawComp[c];
                    if (rawComp[c] > cMax[c]) cMax[c] = rawComp[c];
                }
            };

            pushQuadVertex(va, magA, mergedFieldVecs[i]);
            pushQuadVertex(vb, magA, mergedFieldVecs[i]);
            pushQuadVertex(vc, magB, mergedFieldVecs[i + 1]);

            pushQuadVertex(vb, magA, mergedFieldVecs[i]);
            pushQuadVertex(vd, magB, mergedFieldVecs[i + 1]);
            pushQuadVertex(vc, magB, mergedFieldVecs[i + 1]);
        }

        if (showArrows && arrowSpacingFrac > 0.0f && mergedPts.size() >= 2) {
            // [A2+A3] Logical placement: arrows evenly distributed by arc
            // length, symmetric about the path, with tip/taper margins and an
            // anti-overlap floor (see computeArrowPlacement).
            const std::vector<float> arcPos =
                computeArrowPlacement(totalLength, extent, arrowSpacingFrac, arrowSize, taperFactor);
            if (!arcPos.empty()) {
                // Cumulative arc length along the merged path.
                std::vector<float> cum(mergedPts.size(), 0.0f);
                for (size_t i = 1; i < mergedPts.size(); ++i)
                    cum[i] = cum[i - 1] + glm::length(mergedPts[i] - mergedPts[i - 1]);

                size_t seg = 0;
                for (float s : arcPos) {
                    while (seg + 2 < mergedPts.size() && cum[seg + 1] < s) ++seg;
                    const float span = std::max(cum[seg + 1] - cum[seg], 1e-12f);
                    const float frac = std::min(std::max((s - cum[seg]) / span, 0.0f), 1.0f);
                    const glm::vec3 fieldVec = glm::mix(mergedFieldVecs[seg], mergedFieldVecs[seg + 1], frac);
                    const float mag = glm::length(fieldVec);
                    if (mag > 1e-12f) {
                        arrowPositions.push_back(glm::mix(mergedPts[seg], mergedPts[seg + 1], frac));
                        arrowDirections.push_back(fieldVec / mag);
                        arrowMagnitudes.push_back(mag);
                        arrowFieldVecs.push_back(fieldVec);
                    }
                }
            }
        }
    }

    if (mn > mx) { mn = 0.0f; mx = 0.0f; }
    result.magMin = mn;
    result.magMax = mx;
    for (int c = 0; c < 3; ++c) {
        result.compMin[c] = cMin[c];
        result.compMax[c] = cMax[c];
    }
    result.lineCount = static_cast<int>(result.verts.size() / 10);
    result.seedCount = static_cast<int>(seeds.size());

    // Generate arrowhead vertices on the CPU (no GL needed).
    if (showArrows && !arrowPositions.empty()) {
        const float arrowHeight = static_cast<float>(arrowSize * extent);
        const float arrowRadius = arrowHeight * 0.35f;
        const int segments = 16;

        const size_t floatsPerArrow = segments * 6 * 10;
        result.arrowVerts.reserve(arrowPositions.size() * floatsPerArrow);

        for (size_t i = 0; i < arrowPositions.size(); ++i) {
            auto piece = generateArrowhead(arrowPositions[i], arrowDirections[i], arrowHeight, arrowRadius, segments, arrowMagnitudes[i], arrowFieldVecs[i]);
            result.arrowVerts.insert(result.arrowVerts.end(), piece.begin(), piece.end());
        }
        result.arrowCount = static_cast<int>(result.arrowVerts.size() / 10);
    }

    return result;
}

void StreamlineSet::uploadGL(StreamlineSet::StreamlineResult&& res, bool showArrows, float arrowSize) {
    teardownGL();

    magMin = res.magMin;
    magMax = res.magMax;
    for (int c = 0; c < 3; ++c) {
        compMin[c] = res.compMin[c];
        compMax[c] = res.compMax[c];
    }
    lineCount = res.lineCount;
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
                                 float ribbonWidth, float taperFactor) {
    auto result = compute(mesh, seedCountParam, stepSize, maxSteps, fieldName, mode, direction,
                          planePos, jitter, planeCountU, planeCountV,
                          showArrows, arrowSpacingFrac, arrowSize, ribbonWidth, taperFactor);
    uploadGL(std::move(result), showArrows, arrowSize);
}
