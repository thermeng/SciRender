#pragma once
// Shared sampler for probe/line-plot — eliminates ~120L duplication between
// main_window.cpp sampleOne and plot_window.cpp recomputeLine.
// Structured fast path: brute cell bbox + trilinear. Unstructured fallback: nearest vertex / cell center.

#include "core/mesh_loader.h"
#include "core/FieldStore.h"
#include <glm/glm.hpp>
#include <optional>
#include <limits>
#include <algorithm>

namespace FieldSampler {

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
