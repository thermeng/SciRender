#pragma once
// Shared sampler for probe/line-plot — eliminates ~120L duplication between
// main_window.cpp sampleOne and plot_window.cpp recomputeLine.
// Structured fast path: brute cell bbox + trilinear. Unstructured fallback: nearest vertex / cell center.

#include "core/mesh_loader.h"
#include "core/FieldStore.h"
#include "core/cell_topology.h"
#include <glm/glm.hpp>
#include <optional>
#include <limits>
#include <algorithm>
#include <vector>

namespace FieldSampler {

// True when pos lies inside the mesh volume — not just its axis-aligned bbox.
// Surface meshes without volume cells fall back to the bbox test.
inline bool isInsideMesh(const RenderMesh& rm, const glm::vec3& pos) {
    const float eps = 1e-6f;
    if (pos.x < rm.bounds.minX - eps || pos.x > rm.bounds.maxX + eps
        || pos.y < rm.bounds.minY - eps || pos.y > rm.bounds.maxY + eps) return false;
    const bool hasZ = (rm.bounds.maxZ - rm.bounds.minZ) > eps;
    if (hasZ && (pos.z < rm.bounds.minZ - eps || pos.z > rm.bounds.maxZ + eps)) return false;

    if (!rm.hasUnstructuredVolumeCells() || rm.cellOffsets.size() != rm.cellTypes.size()) return true;
    const std::size_t nVert = rm.vertices.size() / 3;
    std::vector<std::array<std::uint32_t, 4>> tets;
    for (std::size_t c = 0; c < rm.cellOffsets.size(); ++c) {
        const std::uint8_t type = rm.cellTypes[c];
        if (!cell_topology::isVolumeCell(type)) continue;
        const std::size_t begin = c ? rm.cellOffsets[c - 1] : 0;
        const std::size_t end = rm.cellOffsets[c];
        const int corners = cell_topology::cornerCount(type);
        if (end <= begin || end - begin < static_cast<std::size_t>(corners)) continue;

        // Corner positions plus the cell centroid (the tets() center slot).
        std::vector<glm::vec3> pts(static_cast<std::size_t>(corners) + 1);
        bool ok = true;
        for (int k = 0; k < corners; ++k) {
            const std::uint32_t id = rm.cellConnectivity[begin + static_cast<std::size_t>(k)];
            if (id >= nVert) { ok = false; break; }
            pts[static_cast<std::size_t>(k)] = glm::vec3(rm.vertices[id * 3], rm.vertices[id * 3 + 1], rm.vertices[id * 3 + 2]);
        }
        if (!ok) continue;
        pts[static_cast<std::size_t>(corners)] = glm::vec3(0.0f);
        for (int k = 0; k < corners; ++k) pts[static_cast<std::size_t>(corners)] += pts[static_cast<std::size_t>(k)];
        pts[static_cast<std::size_t>(corners)] /= static_cast<float>(corners);

        tets.clear();
        cell_topology::tets(type, std::span<const std::uint32_t>(
            rm.cellConnectivity.data() + begin, static_cast<std::size_t>(corners)), tets);
        for (const auto& tet : tets) {
            const glm::vec3& a = pts[tet[0]];
            const glm::vec3& b = pts[tet[1]];
            const glm::vec3& c3 = pts[tet[2]];
            const glm::vec3& d = pts[tet[3]];
            const glm::vec3 v0 = b - a, v1 = c3 - a, v2 = d - a, vp = pos - a;
            const float det = glm::dot(v0, glm::cross(v1, v2));
            if (std::abs(det) < 1e-12f) continue;
            const float inv = 1.0f / det;
            const float u = glm::dot(glm::cross(v1, v2), vp) * inv;
            const float v = glm::dot(glm::cross(v2, v0), vp) * inv;
            const float w = glm::dot(glm::cross(v0, v1), vp) * inv;
            if (u >= -1e-4f && v >= -1e-4f && w >= -1e-4f && u + v + w <= 1.0f + 1e-4f) return true;
        }
    }
    return false;
}

// Sample scalar at world position using FieldStore::Context (snapshot-aware).
// Returns nullopt when field not found; NaN when outside and no fallback.
inline std::optional<float> sample(const FieldStore::Context& ctx, const std::string& field,
                                   FieldStore::Placement placement, const glm::vec3& pos) {
    if (!ctx.valid()) return std::nullopt;
    const RenderMesh& rm = *ctx.mesh;
    float mn, mx;
    const std::vector<float>* src = nullptr;
    FieldStore::ScalarField sf;
    // Prefer requested placement, fall back internally via FieldStore resolution.
    if (placement == FieldStore::Placement::CellCenter) {
        sf = ctx.scalarForVolume({field, placement});
        if (!sf.data) sf = ctx.scalar({field, placement});
    } else {
        sf = ctx.scalar({field, placement});
    }
    if (!sf.data || sf.data->empty()) return std::nullopt;
    src = sf.data;
    bool isCell = sf.isCell;

    // Structured fast path
    bool isStructured = rm.gridDimX > 1 && rm.gridDimY > 1;
    if (isStructured && rm.gridDimX > 0 && rm.gridDimY > 0 && rm.gridDimZ > 0 && !rm.vertices.empty()) {
        int dX = rm.gridDimX, dY = rm.gridDimY, dZ = rm.gridDimZ;
        const float* verts = rm.vertices.data();
        auto idx = [&](int x, int y, int z) { return x + y * dX + z * dX * dY; };
        int cdX = std::max(1, dX - 1), cdY = std::max(1, dY - 1), cdZ = std::max(1, dZ - 1);
        const float eps = 1e-6f;
        for (int cz = 0; cz < cdZ; ++cz) for (int cy = 0; cy < cdY; ++cy) for (int cx = 0; cx < cdX; ++cx) {
            float minx = 1e30f, miny = 1e30f, minz = 1e30f, maxx = -1e30f, maxy = -1e30f, maxz = -1e30f;
            int z0 = cz, z1 = std::min(cz + 1, dZ - 1);
            int y0 = cy, y1 = std::min(cy + 1, dY - 1);
            int x0 = cx, x1 = std::min(cx + 1, dX - 1);
            int corners[8][3] = {{x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},{x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}};
            for (int k = 0; k < 8; ++k) {
                int n = idx(corners[k][0], corners[k][1], corners[k][2]);
                if (n < 0 || size_t(n * 3 + 2) >= rm.vertices.size()) continue;
                float px = verts[n * 3 + 0], py = verts[n * 3 + 1], pz = verts[n * 3 + 2];
                minx = std::min(minx, px); maxx = std::max(maxx, px);
                miny = std::min(miny, py); maxy = std::max(maxy, py);
                minz = std::min(minz, pz); maxz = std::max(maxz, pz);
            }
            if (pos.x < minx - eps || pos.x > maxx + eps) continue;
            if (pos.y < miny - eps || pos.y > maxy + eps) continue;
            if (dZ > 1 && (pos.z < minz - eps || pos.z > maxz + eps)) continue;
            if (isCell) {
                int cIdx = cx + cy * cdX + cz * cdX * cdY;
                if (cIdx >= 0 && size_t(cIdx) < src->size()) return (*src)[cIdx];
                // fall through to trilinear if cell index out of range
            }
            // trilinear from 8 corner point values
            float s[8]; bool ok = true;
            for (int k = 0; k < 8; ++k) {
                int n = idx(corners[k][0], corners[k][1], corners[k][2]);
                if (n < 0 || size_t(n) >= src->size()) { ok = false; break; }
                s[k] = (*src)[n];
                if (!std::isfinite(s[k])) ok = false;
            }
            if (!ok) continue;
            float dx = (maxx - minx) > 1e-12f ? (pos.x - minx) / (maxx - minx) : 0;
            float dy = (maxy - miny) > 1e-12f ? (pos.y - miny) / (maxy - miny) : 0;
            float dz = (dZ > 1 && (maxz - minz) > 1e-12f) ? (pos.z - minz) / (maxz - minz) : 0;
            dx = std::clamp(dx, 0.0f, 1.0f); dy = std::clamp(dy, 0.0f, 1.0f); dz = std::clamp(dz, 0.0f, 1.0f);
            float c00 = s[0] * (1 - dx) + s[1] * dx;
            float c10 = s[3] * (1 - dx) + s[2] * dx;
            float c01 = s[4] * (1 - dx) + s[5] * dx;
            float c11 = s[7] * (1 - dx) + s[6] * dx;
            float c0 = c00 * (1 - dy) + c10 * dy;
            float c1 = c01 * (1 - dy) + c11 * dy;
            return c0 * (1 - dz) + c1 * dz;
        }
        // Outside structured domain: nearest vertex fallback
        float bestD = 1e30f; int best = -1;
        size_t nPt = rm.vertices.size() / 3;
        for (size_t n = 0; n < nPt; ++n) {
            float dx = verts[n * 3 + 0] - pos.x, dy = verts[n * 3 + 1] - pos.y, dz = verts[n * 3 + 2] - pos.z;
            float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < bestD) { bestD = d2; best = int(n); }
        }
        if (best >= 0 && size_t(best) < src->size()) return (*src)[best];
    } else if (!isCell) {
        // Unstructured point fallback: nearest vertex
        float bestD = 1e30f; int best = -1;
        size_t nPt = rm.vertices.size() / 3;
        const float* verts = rm.vertices.data();
        for (size_t n = 0; n < nPt; ++n) {
            float dx = verts[n * 3 + 0] - pos.x, dy = verts[n * 3 + 1] - pos.y, dz = verts[n * 3 + 2] - pos.z;
            float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < bestD) { bestD = d2; best = int(n); }
        }
        if (best >= 0 && size_t(best) < src->size()) return (*src)[best];
        if (!rm.cellCenters.empty()) {
            float bestC = 1e30f; int bestCidx = -1;
            for (size_t c = 0; c < rm.cellCenters.size(); ++c) {
                float dx = rm.cellCenters[c].x - pos.x, dy = rm.cellCenters[c].y - pos.y, dz = rm.cellCenters[c].z - pos.z;
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < bestC) { bestC = d2; bestCidx = int(c); }
            }
            if (bestCidx >= 0 && size_t(bestCidx) < src->size()) return (*src)[bestCidx];
        }
    } else {
        // Cell unstructured nearest center
        if (!rm.cellCenters.empty()) {
            float bestC = 1e30f; int bestCidx = -1;
            for (size_t c = 0; c < rm.cellCenters.size(); ++c) {
                float dx = rm.cellCenters[c].x - pos.x, dy = rm.cellCenters[c].y - pos.y, dz = rm.cellCenters[c].z - pos.z;
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < bestC) { bestC = d2; bestCidx = int(c); }
            }
            if (bestCidx >= 0 && size_t(bestCidx) < src->size()) return (*src)[bestCidx];
        }
    }
    return std::numeric_limits<float>::quiet_NaN();
}

} // namespace FieldSampler
