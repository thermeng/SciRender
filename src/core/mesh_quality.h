#pragma once
#include "core/mesh_loader.h"   // public RenderMesh only; no parser internals (SRP)
#include <cstdint>
#include <cmath>
#include <unordered_map>
#include <vector>
#include <algorithm>

struct MeshQuality {
    int degenerateFaces = 0;
    int openEdges = 0;
    int nonManifoldEdges = 0;     // Shared by != 2 faces or invalid winding
    int nonManifoldVerts = 0;     // Disjoint triangle fans at a single vertex
    int zeroLengthEdges = 0;      // Edges collapsed to zero length post-weld
    bool watertight = false;

    std::vector<float> degenerateTriVerts;   // 18 floats / degen face
    std::vector<float> openEdgeVerts;        // 6 floats / open edge
    std::vector<float> nonManifoldEdgeVerts; // 6 floats / non-manifold edge
};

inline MeshQuality analyzeMeshQuality(const RenderMesh& mesh) {
    MeshQuality q;
    const auto& fv = mesh.flatVerts;   // raw per-corner positions (9 floats/tri)
    const size_t nt = fv.size() / 9;
    if (nt == 0) return q;

    // -------------------------------------------------------------------------
    // 1. Quantize & Weld Vertices (Trimesh tol.merge = 1e-8)
    // -------------------------------------------------------------------------
    auto quant = [](float f) -> int64_t {
        return static_cast<int64_t>(std::llround(static_cast<double>(f) * 1e8));
    };

    struct QKey {
        int64_t x, y, z;
        bool operator==(const QKey& o) const { return x == o.x && y == o.y && z == o.z; }
    };

    // Fast xor-shift mixer hash for 64-bit quantized keys
    struct QKeyHash {
        size_t operator()(const QKey& k) const {
            uint64_t hx = static_cast<uint64_t>(k.x) * 0x9e3779b97f4a7c15ULL;
            uint64_t hy = static_cast<uint64_t>(k.y) * 0xbf58476d1ce4e5b9ULL;
            uint64_t hz = static_cast<uint64_t>(k.z) * 0x94d049bb133111ebULL;
            return static_cast<size_t>(hx ^ (hy >> 16) ^ (hz << 8));
        }
    };

    std::unordered_map<QKey, uint32_t, QKeyHash> keyToWelded;
    keyToWelded.reserve(nt * 3);

    std::vector<uint32_t> weld(nt * 3);
    std::vector<uint32_t> weldedCorner;
    weldedCorner.reserve(nt * 3);

    for (size_t i = 0; i < nt * 3; ++i) {
        const float* p = &fv[i * 3];
        QKey k{ quant(p[0]), quant(p[1]), quant(p[2]) };
        auto it = keyToWelded.find(k);
        if (it == keyToWelded.end()) {
            uint32_t newIdx = static_cast<uint32_t>(weldedCorner.size());
            it = keyToWelded.emplace(k, newIdx).first;
            weldedCorner.push_back(static_cast<uint32_t>(i));
        }
        weld[i] = it->second;
    }
    const uint32_t nv = static_cast<uint32_t>(weldedCorner.size());

    auto cornerPos = [&](uint32_t c, float* out) {
        const float* p = &fv[c * 3];
        out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
    };

    // -------------------------------------------------------------------------
    // 2. Classify Degenerate & Zero-Length Faces
    // -------------------------------------------------------------------------
    std::vector<char> isDegen(nt, 0);

    for (size_t t = 0; t < nt; ++t) {
        uint32_t a = weld[t * 3], b = weld[t * 3 + 1], c = weld[t * 3 + 2];

        if (a == b || b == c || c == a) {
            isDegen[t] = 1;
            q.degenerateFaces++;
            q.zeroLengthEdges++;
        } else {
            float pa[3], pb[3], pc[3];
            cornerPos(weldedCorner[a], pa);
            cornerPos(weldedCorner[b], pb);
            cornerPos(weldedCorner[c], pc);

            double e1x = pb[0] - pa[0], e1y = pb[1] - pa[1], e1z = pb[2] - pa[2];
            double e2x = pc[0] - pa[0], e2y = pc[1] - pa[1], e2z = pc[2] - pa[2];

            double cx = e1y * e2z - e1z * e2y;
            double cy = e1z * e2x - e1x * e2z;
            double cz = e1x * e2y - e1y * e2x;
            double area2 = 0.25 * (cx * cx + cy * cy + cz * cz);

            if (area2 < 1e-16) {
                isDegen[t] = 1;
                q.degenerateFaces++;
            }
        }

        if (isDegen[t]) {
            float pa[3], pb[3], pc[3];
            cornerPos(weldedCorner[a], pa);
            cornerPos(weldedCorner[b], pb);
            cornerPos(weldedCorner[c], pc);

            q.degenerateTriVerts.insert(q.degenerateTriVerts.end(), {
                pa[0],pa[1],pa[2], pb[0],pb[1],pb[2],
                pb[0],pb[1],pb[2], pc[0],pc[1],pc[2],
                pc[0],pc[1],pc[2], pa[0],pa[1],pa[2]
            });
        }
    }

    // -------------------------------------------------------------------------
    // 3. Flat Cache-Friendly Edge Sorting (Replaces std::unordered_map)
    // -------------------------------------------------------------------------
    struct DirectedEdge {
        uint64_t key;
        uint32_t u;
        uint32_t v;

        bool operator<(const DirectedEdge& o) const {
            return key < o.key;
        }
    };

    std::vector<DirectedEdge> flatEdges;
    flatEdges.reserve(nt * 3);

    std::vector<std::vector<uint32_t>> vertSurvivingTris(nv);

    for (size_t t = 0; t < nt; ++t) {
        if (isDegen[t]) continue;

        uint32_t corners[3] = { weld[t * 3], weld[t * 3 + 1], weld[t * 3 + 2] };
        vertSurvivingTris[corners[0]].push_back(static_cast<uint32_t>(t));
        vertSurvivingTris[corners[1]].push_back(static_cast<uint32_t>(t));
        vertSurvivingTris[corners[2]].push_back(static_cast<uint32_t>(t));

        for (int i = 0; i < 3; ++i) {
            uint32_t u = corners[i];
            uint32_t v = corners[(i + 1) % 3];
            uint32_t x = u < v ? u : v;
            uint32_t y = u < v ? v : u;
            uint64_t k = (static_cast<uint64_t>(x) << 32) | y;

            flatEdges.push_back({ k, u, v });
        }
    }

    // Contiguous memory sort (Order of magnitude faster than map insertions)
    std::sort(flatEdges.begin(), flatEdges.end());

    auto pushEdge = [&](uint32_t wx, uint32_t wy, std::vector<float>& dst) {
        float px[3], py[3];
        cornerPos(weldedCorner[wx], px);
        cornerPos(weldedCorner[wy], py);
        dst.insert(dst.end(), { px[0],px[1],px[2], py[0],py[1],py[2] });
    };

    // Sequential linear scan over sorted edge buffer
    size_t edgeIdx = 0;
    const size_t numFlat = flatEdges.size();

    while (edgeIdx < numFlat) {
        size_t runEnd = edgeIdx + 1;
        while (runEnd < numFlat && flatEdges[runEnd].key == flatEdges[edgeIdx].key) {
            runEnd++;
        }

        int count = static_cast<int>(runEnd - edgeIdx);
        int forwardCount = 0;
        int reverseCount = 0;

        for (size_t i = edgeIdx; i < runEnd; ++i) {
            if (flatEdges[i].u < flatEdges[i].v) forwardCount++;
            else reverseCount++;
        }

        uint32_t u = static_cast<uint32_t>(flatEdges[edgeIdx].key >> 32);
        uint32_t v = static_cast<uint32_t>(flatEdges[edgeIdx].key & 0xFFFFFFFFu);

        if (count == 1) {
            q.openEdges++;
            pushEdge(u, v, q.openEdgeVerts);
        } else if (count > 2 || (count == 2 && (forwardCount != 1 || reverseCount != 1))) {
            q.nonManifoldEdges++;
            pushEdge(u, v, q.nonManifoldEdgeVerts);
        }

        edgeIdx = runEnd;
    }

    // -------------------------------------------------------------------------
    // 4. Linear Scan Local Star Traversal (Zero Re-Allocations)
    // -------------------------------------------------------------------------
    std::vector<uint32_t> linkStack;
    std::vector<char> linkVis;
    std::vector<uint32_t> neighbors;
    std::vector<std::vector<uint32_t>> link;

    for (uint32_t s = 0; s < nv; ++s) {
        const auto& tris = vertSurvivingTris[s];
        if (tris.empty()) continue;

        neighbors.clear();
        for (uint32_t t : tris) {
            uint32_t a = weld[t * 3], b = weld[t * 3 + 1], c = weld[t * 3 + 2];
            if (a != s) neighbors.push_back(a);
            if (b != s) neighbors.push_back(b);
            if (c != s) neighbors.push_back(c);
        }
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());

        const size_t numNb = neighbors.size();
        if (numNb == 0) continue;

        link.assign(numNb, {});

        // Fast linear scan over tiny local neighbor list (faster than binary search)
        auto getNeighborIdx = [&](uint32_t v) -> int {
            for (size_t i = 0; i < numNb; ++i) {
                if (neighbors[i] == v) return static_cast<int>(i);
            }
            return -1;
        };

        for (uint32_t t : tris) {
            uint32_t a = weld[t * 3], b = weld[t * 3 + 1], c = weld[t * 3 + 2];
            uint32_t x = 0, y = 0, found = 0;

            if (a != s) { x = a; found++; }
            if (b != s) { if (found == 0) { x = b; found++; } else y = b; }
            if (c != s) { if (found == 0) { x = c; found++; } else y = c; }

            int ix = getNeighborIdx(x);
            int iy = getNeighborIdx(y);
            if (ix != -1 && iy != -1) {
                link[ix].push_back(iy);
                link[iy].push_back(ix);
            }
        }

        linkVis.assign(numNb, 0);
        linkStack.clear();

        linkStack.push_back(0);
        linkVis[0] = 1;
        size_t reached = 0;

        while (!linkStack.empty()) {
            uint32_t curr = linkStack.back();
            linkStack.pop_back();
            reached++;

            for (uint32_t nxt : link[curr]) {
                if (!linkVis[nxt]) {
                    linkVis[nxt] = 1;
                    linkStack.push_back(nxt);
                }
            }
        }

        if (reached < numNb) {
            q.nonManifoldVerts++;
        }
    }

    q.watertight = (q.openEdges == 0 && q.nonManifoldEdges == 0 && q.nonManifoldVerts == 0 && q.degenerateFaces == 0);
    return q;
}