#include "render/StreamlineSet.h"
#include "core/mesh_loader.h"

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
    if (!data || count <= 0 || searchCount <= 0) return glm::vec3(0.0f);

    float bestD2 = std::numeric_limits<float>::max();
    int bestIdx = 0;
    const float* verts = mesh.vertices.data();
    for (int i = 0; i < searchCount; ++i) {
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
    if (bestIdx >= count) return glm::vec3(0.0f);
    return data[bestIdx];
}

struct StructuredGridInfo {
    std::vector<float> xs, ys, zs;
    int dimX = 0, dimY = 0, dimZ = 0;
    const glm::vec3* data = nullptr;
    int count = 0;
};

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
    return info;
}

glm::vec3 StreamlineSet::evalFieldTrilinear(const StreamlineSet::StructuredGridInfo& info, const glm::vec3& pos) {
    if (info.dimX <= 1 || info.dimY <= 1 || info.dimZ <= 1 || !info.data) {
        return glm::vec3(0.0f);
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

std::vector<glm::vec3> StreamlineSet::generateSeeds(const RenderMesh& mesh, int seedCount, const std::string& mode, double planePos, double jitter) {
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
        int n = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(seedCount))));
        n = std::max(1, n);
        float sx = maxX - minX;
        float sy = maxY - minY;
        float sz = maxZ - minZ;

        for (int ix = 0; ix < n && static_cast<int>(seeds.size()) < seedCount; ++ix) {
            for (int iy = 0; iy < n && static_cast<int>(seeds.size()) < seedCount; ++iy) {
                float tx = (ix + 0.5f) / n;
                float ty = (iy + 0.5f) / n;
                float x = minX + tx * sx;
                float y = minY + ty * sy;
                float z = minZ + pos * sz;

                if (mode == "PlaneXZ") {
                    y = minY + pos * sy;
                    z = minZ + ty * sz;
                } else if (mode == "PlaneYZ") {
                    x = minX + pos * sx;
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

std::vector<float> StreamlineSet::generateArrowhead(const glm::vec3& pos, const glm::vec3& dir, float height, float radius, int segments, float mag) {
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

    // Arrowhead verts carry dashFlag=0.0 and u=0.0 so the fragment shader
    // skips dashing for arrowheads regardless of the global dash setting.
    auto push = [&](const glm::vec3& p, const glm::vec3& n) {
        verts.push_back(p.x); verts.push_back(p.y); verts.push_back(p.z);
        verts.push_back(mag);
        verts.push_back(n.x); verts.push_back(n.y); verts.push_back(n.z);
        verts.push_back(0.0f); // dashFlag
        verts.push_back(0.0f); // u
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

void StreamlineSet::teardownGL() {
    if (vao) glDeleteVertexArrays(1, &vao);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (seedVao) glDeleteVertexArrays(1, &seedVao);
    if (seedVbo) glDeleteBuffers(1, &seedVbo);
    if (arrowVao) glDeleteVertexArrays(1, &arrowVao);
    if (arrowVbo) glDeleteBuffers(1, &arrowVbo);
    vao = 0; vbo = 0; lineCount = 0;
    seedVao = 0; seedVbo = 0; seedCount = 0;
    arrowVao = 0; arrowVbo = 0; arrowCount = 0;
}

void StreamlineSet::shutdown() {
    teardownGL();
}

void StreamlineSet::rebuild(const RenderMesh& mesh, int seedCount, float stepSize, int maxSteps,
                                const std::string& fieldName, const std::string& mode,
                                double planePos, double jitter, bool showArrows, int arrowSpacing, float arrowSize,
                                float ribbonWidth, float taperFactor) {
    teardownGL();

    if (mesh.pointVectorsData.empty()) return;

    size_t count = 0;
    const glm::vec3* data = nullptr;
    auto tryField = [&](const std::string& name) -> bool {
        if (name.empty()) return false;
        size_t c = 0;
        const glm::vec3* d = mesh.vectorFieldData(name, c);
        if (d && c > 0) { data = d; count = c; return true; }
        return false;
    };
    if (!tryField(fieldName) && !tryField(mesh.vectorName) &&
        !(mesh.availableVectorNames.empty() ? false : tryField(mesh.availableVectorNames.front()))) {
        return;
    }

    int numVerts = static_cast<int>(mesh.vertices.size() / 3);
    const int limit = std::min(numVerts, static_cast<int>(count));
    if (limit <= 0) return;

    const float extent = static_cast<float>(mesh.bounds.extent);
    const float h = std::max(stepSize, 1e-8f);
    const float magThresh = 1e-6f;

    StructuredGridInfo grid = buildStructuredGridInfo(mesh, data, limit);
    const bool hasGrid = grid.dimX > 0;

    auto evalField = [&](const glm::vec3& pos) -> glm::vec3 {
        if (hasGrid) {
            return evalFieldTrilinear(grid, pos);
        }
        return evalFieldNearest(mesh, pos, data, limit, limit);
    };

    std::vector<glm::vec3> seeds = generateSeeds(mesh, seedCount, mode, planePos, jitter);
    if (seeds.empty()) return;

    float mn = std::numeric_limits<float>::max();
    float mx = -std::numeric_limits<float>::max();

    std::vector<float> verts; // interleaved [x,y,z,mag,normalX,normalY,normalZ,u,v]
    std::vector<float> seedVerts; // interleaved [x,y,z]
    std::vector<glm::vec3> arrowPositions;
    std::vector<glm::vec3> arrowDirections;
    std::vector<float> arrowMagnitudes;

    const size_t estimatedSegments = seeds.size() * 2 * maxSteps;
    verts.reserve(estimatedSegments * 6 * 9); // 6 vertices per quad segment
    seedVerts.reserve(seeds.size() * 3);
    if (showArrows && arrowSpacing > 0) {
        size_t estimatedArrows = estimatedSegments / arrowSpacing;
        arrowPositions.reserve(estimatedArrows);
        arrowDirections.reserve(estimatedArrows);
        arrowMagnitudes.reserve(estimatedArrows);
    }

    for (const auto& seed : seeds) {
        seedVerts.push_back(seed.x);
        seedVerts.push_back(seed.y);
        seedVerts.push_back(seed.z);

        for (int dir = -1; dir <= 1; dir += 2) {
            glm::vec3 pos = seed;
            std::vector<glm::vec3> pts;
            pts.push_back(pos);

            for (int iter = 0; iter < maxSteps; ++iter) {
                auto direction = [&](const glm::vec3& p) -> glm::vec3 {
                    glm::vec3 v = evalField(p);
                    float m = std::sqrt(magSq(v));
                    if (m < magThresh) return glm::vec3(0.0f);
                    return static_cast<float>(dir) * (v / m);
                };

                glm::vec3 k1 = direction(pos);
                if (glm::length(k1) < 1e-12f) break;

                glm::vec3 p2 = pos + 0.5f * h * k1;
                glm::vec3 k2 = direction(p2);
                if (glm::length(k2) < 1e-12f) break;

                glm::vec3 p3 = pos + 0.5f * h * k2;
                glm::vec3 k3 = direction(p3);
                if (glm::length(k3) < 1e-12f) break;

                glm::vec3 p4 = pos + h * k3;
                glm::vec3 k4 = direction(p4);
                if (glm::length(k4) < 1e-12f) break;

                glm::vec3 newPos = pos + (h / 6.0f) * (k1 + 2.0f * k2 + 2.0f * k3 + k4);

                if (newPos.x < mesh.bounds.minX || newPos.x > mesh.bounds.maxX ||
                    newPos.y < mesh.bounds.minY || newPos.y > mesh.bounds.maxY ||
                    newPos.z < mesh.bounds.minZ || newPos.z > mesh.bounds.maxZ) {
                    pts.push_back(newPos);
                    break;
                }

                pts.push_back(newPos);
                pos = newPos;
            }

            if (pts.size() < 3) continue;

            std::vector<glm::vec3> sampled;
            sampled.reserve(pts.size());
            for (size_t i = 0; i < pts.size(); i += 2) sampled.push_back(pts[i]);
            if (sampled.size() >= 2 && sampled.back() != pts.back()) sampled.push_back(pts.back());

            // Compute cumulative total length for continuous U-mapping down the streamline
            float totalLength = 0.0f;
            for (size_t i = 0; i + 1 < sampled.size(); ++i) {
                totalLength += glm::length(sampled[i + 1] - sampled[i]);
            }
            if (totalLength < 1e-6f) totalLength = 1.0f;

            float currentLength = 0.0f;
            const float baseWidth = static_cast<float>(extent * ribbonWidth);

            for (size_t i = 0; i + 1 < sampled.size(); ++i) {
                glm::vec3 a = sampled[i];
                glm::vec3 b = sampled[i + 1];
                glm::vec3 tangent = b - a;
                float segLen = glm::length(tangent);
                if (segLen < 1e-12f) continue;
                tangent /= segLen;

    // Smooth local frame orientation
    glm::vec3 upVecLocal = std::abs(tangent.y) < 0.95f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 side = glm::normalize(glm::cross(tangent, upVecLocal));
    if (std::abs(glm::length(side)) < 1e-12f) {
        side = glm::vec3(0.0f, 0.0f, 1.0f);
    }
    glm::vec3 normal = glm::normalize(glm::cross(tangent, side));

                float magA = std::sqrt(magSq(evalField(a)));
                float magB = std::sqrt(magSq(evalField(b)));

                float tA = (i) / static_cast<float>(sampled.size());
                float tB = (i + 1) / static_cast<float>(sampled.size());
                float taperA = 1.0f - taperFactor * std::abs(2.0f * tA - 1.0f);
                float taperB = 1.0f - taperFactor * std::abs(2.0f * tB - 1.0f);

                glm::vec3 va = a - side * (baseWidth * taperA);
                glm::vec3 vb = a + side * (baseWidth * taperA);
                glm::vec3 vc = b - side * (baseWidth * taperB);
                glm::vec3 vd = b + side * (baseWidth * taperB);

                // Continuous U coordinate along the ribbon
                float uA = currentLength / totalLength;
                currentLength += segLen;
                float uB = currentLength / totalLength;

                auto pushQuadVertex = [&](const glm::vec3& p, float rawMag, float u, float) {
                    verts.push_back(p.x); verts.push_back(p.y); verts.push_back(p.z);
                    verts.push_back(rawMag);
                    verts.push_back(normal.x); verts.push_back(normal.y); verts.push_back(normal.z);
                    verts.push_back(1.0f); // dashFlag
                    verts.push_back(u);    // u
                    if (rawMag < mn) mn = rawMag;
                    if (rawMag > mx) mx = rawMag;
                };

                // Triangle 1: va -> vb -> vc
                pushQuadVertex(va, magA, uA, 0.0f);
                pushQuadVertex(vb, magA, uA, 1.0f);
                pushQuadVertex(vc, magB, uB, 0.0f);

                // Triangle 2: vb -> vd -> vc
                pushQuadVertex(vb, magA, uA, 1.0f);
                pushQuadVertex(vd, magB, uB, 1.0f);
                pushQuadVertex(vc, magB, uB, 0.0f);
            }

            if (showArrows && arrowSpacing > 0 && sampled.size() > static_cast<size_t>(arrowSpacing + 1)) {
                for (size_t i = arrowSpacing; i + 1 < sampled.size(); i += arrowSpacing) {
                    glm::vec3 tangent = sampled[i + 1] - sampled[i];
                    float len = glm::length(tangent);
                    if (len > 1e-12f) {
                        arrowPositions.push_back(sampled[i]);
                        arrowDirections.push_back(tangent / len);
                        arrowMagnitudes.push_back(std::sqrt(magSq(evalField(sampled[i]))));
                    }
                }
            }
        }
    }

    if (verts.empty()) return;
    if (mn > mx) { mn = 0.0f; mx = 0.0f; }

    magMin = mn;
    magMax = mx;
    lineCount = static_cast<int>(verts.size() / 9);

    // Streamline ribbon VAO/VBO setup
    glCreateVertexArrays(1, &vao);
    glCreateBuffers(1, &vbo);

    glEnableVertexArrayAttrib(vao, 0);
    glVertexArrayAttribFormat(vao, 0, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(vao, 0, 0);

    glEnableVertexArrayAttrib(vao, 1);
    glVertexArrayAttribFormat(vao, 1, 1, GL_FLOAT, GL_FALSE, 3 * sizeof(float));
    glVertexArrayAttribBinding(vao, 1, 0);

    glEnableVertexArrayAttrib(vao, 2);
    glVertexArrayAttribFormat(vao, 2, 3, GL_FLOAT, GL_FALSE, 4 * sizeof(float));
    glVertexArrayAttribBinding(vao, 2, 0);

    glEnableVertexArrayAttrib(vao, 3);
    glVertexArrayAttribFormat(vao, 3, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float));
    glVertexArrayAttribBinding(vao, 3, 0);

    glEnableVertexArrayAttrib(vao, 4);
    glVertexArrayAttribFormat(vao, 4, 1, GL_FLOAT, GL_FALSE, 8 * sizeof(float));
    glVertexArrayAttribBinding(vao, 4, 0);

    glNamedBufferData(vbo, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
    glVertexArrayVertexBuffer(vao, 0, vbo, 0, 9 * sizeof(float));

    // Seed points VAO/VBO setup
    seedCount = static_cast<int>(seeds.size());
    if (!seedVerts.empty()) {
        glCreateVertexArrays(1, &seedVao);
        glCreateBuffers(1, &seedVbo);
        glEnableVertexArrayAttrib(seedVao, 0);
        glVertexArrayAttribFormat(seedVao, 0, 3, GL_FLOAT, GL_FALSE, 0);
        glVertexArrayAttribBinding(seedVao, 0, 0);
        glNamedBufferData(seedVbo, seedVerts.size() * sizeof(float), seedVerts.data(), GL_STATIC_DRAW);
        glVertexArrayVertexBuffer(seedVao, 0, seedVbo, 0, 3 * sizeof(float));
    }

    // Arrowhead VAO/VBO setup
    if (showArrows && !arrowPositions.empty()) {
        std::vector<float> arrowVerts;
        const float arrowHeight = static_cast<float>(arrowSize * extent);
        const float arrowRadius = arrowHeight * 0.35f;
        const int segments = 16;
        
        const size_t floatsPerArrow = segments * 6 * 9;  // 6 verts per segment (3 side + 3 cap)
        arrowVerts.reserve(arrowPositions.size() * floatsPerArrow);

        for (size_t i = 0; i < arrowPositions.size(); ++i) {
            auto piece = generateArrowhead(arrowPositions[i], arrowDirections[i], arrowHeight, arrowRadius, segments, arrowMagnitudes[i]);
            arrowVerts.insert(arrowVerts.end(), piece.begin(), piece.end());
        }

        arrowCount = static_cast<int>(arrowVerts.size() / 9);
        if (arrowCount > 0) {
            glCreateVertexArrays(1, &arrowVao);
            glCreateBuffers(1, &arrowVbo);

            glEnableVertexArrayAttrib(arrowVao, 0);
            glVertexArrayAttribFormat(arrowVao, 0, 3, GL_FLOAT, GL_FALSE, 0);
            glVertexArrayAttribBinding(arrowVao, 0, 0);

            glEnableVertexArrayAttrib(arrowVao, 1);
            glVertexArrayAttribFormat(arrowVao, 1, 1, GL_FLOAT, GL_FALSE, 3 * sizeof(float));
            glVertexArrayAttribBinding(arrowVao, 1, 0);

            glEnableVertexArrayAttrib(arrowVao, 2);
            glVertexArrayAttribFormat(arrowVao, 2, 3, GL_FLOAT, GL_FALSE, 4 * sizeof(float));
            glVertexArrayAttribBinding(arrowVao, 2, 0);

            glEnableVertexArrayAttrib(arrowVao, 3);
            glVertexArrayAttribFormat(arrowVao, 3, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float));
            glVertexArrayAttribBinding(arrowVao, 3, 0);

            glEnableVertexArrayAttrib(arrowVao, 4);
            glVertexArrayAttribFormat(arrowVao, 4, 1, GL_FLOAT, GL_FALSE, 8 * sizeof(float));
            glVertexArrayAttribBinding(arrowVao, 4, 0);

            glNamedBufferData(arrowVbo, arrowVerts.size() * sizeof(float), arrowVerts.data(), GL_STATIC_DRAW);
            glVertexArrayVertexBuffer(arrowVao, 0, arrowVbo, 0, 9 * sizeof(float));
        }
    }
}