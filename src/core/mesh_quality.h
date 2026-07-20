#pragma once
#include "core/mesh_loader.h"   // public RenderMesh only; no parser internals (SRP)
#include <cstdint>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>

// Mesh-quality analysis — separate concern from loading/parsing (SRP).
// Reference model: libigl is_edge_manifold / is_vertex_manifold + CGAL is_manifold.
//   1. weld coincident positions (parsers de-index shared topology)
//   2. edge incidence: ==1 boundary, >2 non-manifold edge
//   3. vertex link must be ONE connected 1-ring (cycle if closed, chain if boundary);
//      >1 component == non-manifold vertex (T-junction / fan) even if every edge used <=2x
struct MeshQuality {
    int degenerateFaces = 0;
    int openEdges = 0;
    int nonManifoldEdges = 0;     // non-manifold edges (shared by >2 tris)
    int nonManifoldVerts = 0;      // non-manifold vertices (link splits into >1 component)
    bool watertight = false;
    std::vector<float> degenerateTriVerts;  // 18 floats / degenerate tri (its 3 edges)
    std::vector<float> openEdgeVerts;        // 6 floats / open edge
    std::vector<float> nonManifoldEdgeVerts; // 6 floats / non-manifold edge
};

inline MeshQuality analyzeMeshQuality(const RenderMesh& mesh) {
    MeshQuality q;
    const auto& v = mesh.vertices;
    const auto& idx = mesh.indices;
    const size_t nt = idx.size() / 3;
    if (v.empty() || idx.size() < 3) return q;

    // ---- 1. weld coincident verts (tol 1e-7 mesh units) ----
    // ponytail: only merges truly coincident verts (matches validated reference).
    // 1/4096 was 24000x too loose -> fake non-manifold edges (bodykit 1207 vs real 16).
    auto quant = [](float f) -> int64_t {
        const int64_t Q = 10000000; // 1e-7
        int64_t i = static_cast<int64_t>(std::llround(f * static_cast<double>(Q)));
        const int64_t lim = (int64_t(1) << 30) - 1;
        return i > lim ? lim : (i < -lim ? -lim : i);
    };
    // ponytail: key on the exact quantized (x,y,z) triple. Old code bit-packed into
    // uint64 with a 25-bit mask; coords*1e-7 exceed 2^25 so the mask wrapped and the
    // weld failed to merge coincident points on large meshes (56 zero-length edges
    // survived on bodykit, corrupting degenerate/open/non-manifold counts).
    struct QKey { int64_t x, y, z; bool operator==(const QKey& o) const { return x==o.x&&y==o.y&&z==o.z; } };
    struct QKeyHash { size_t operator()(const QKey& k) const {
        size_t h = std::hash<int64_t>{}(k.x); h ^= std::hash<int64_t>{}(k.y) + 0x9e3779b9 + (h<<6) + (h>>2);
        h ^= std::hash<int64_t>{}(k.z) + 0x9e3779b9 + (h<<6) + (h>>2); return h; } };
    auto posKey = [&](uint32_t vi) -> QKey {
        const float* p = &v[vi*3];
        return QKey{ quant(p[0]), quant(p[1]), quant(p[2]) };
    };
    std::unordered_map<QKey, uint32_t, QKeyHash> keyToWelded;
    std::vector<uint32_t> weldedToRaw;
    std::vector<uint32_t> weld(idx.size());
    for (size_t i = 0; i < idx.size(); ++i) {
        QKey k = posKey(idx[i]);
        auto it = keyToWelded.find(k);
        if (it == keyToWelded.end()) {
            it = keyToWelded.emplace(k, static_cast<uint32_t>(weldedToRaw.size())).first;
            weldedToRaw.push_back(idx[i]);
        }
        weld[i] = it->second;
    }
    const uint32_t nv = static_cast<uint32_t>(weldedToRaw.size());

    // ---- 2. classify triangles: degenerate vs real ----
    // ponytail: degenerate = coincident verts OR area ~0 relative to mesh scale.
    // Absolute area cutoff (old cr<1e-12f) fired on 303 near-flat faces; script.py
    // uses a relative threshold and reports 0 here. Match it: area < 1e-9*maxArea.
    double maxArea = 0.0;
    for (size_t t = 0; t < nt; ++t) {
        const float* pa = &v[idx[t*3]*3];
        const float* pb = &v[idx[t*3+1]*3];
        const float* pc = &v[idx[t*3+2]*3];
        float e1x = pb[0]-pa[0], e1y = pb[1]-pa[1], e1z = pb[2]-pa[2];
        float e2x = pc[0]-pa[0], e2y = pc[1]-pa[1], e2z = pc[2]-pa[2];
        float cr = (e1y*e2z - e1z*e2y)*(e1y*e2z - e1z*e2y)
                 + (e1z*e2x - e1x*e2z)*(e1z*e2x - e1x*e2z)
                 + (e1x*e2y - e1y*e2x)*(e1x*e2y - e1y*e2x);
        double area = 0.5 * std::sqrt(static_cast<double>(cr));
        if (area > maxArea) maxArea = area;
    }
    const double degenThresh = 1e-9 * maxArea;
    std::vector<char> isDegen(nt, 0);
    for (size_t t = 0; t < nt; ++t) {
        uint32_t a = weld[t*3], b = weld[t*3+1], c = weld[t*3+2];
        const float* pa = &v[idx[t*3]*3];
        const float* pb = &v[idx[t*3+1]*3];
        const float* pc = &v[idx[t*3+2]*3];
        float e1x = pb[0]-pa[0], e1y = pb[1]-pa[1], e1z = pb[2]-pa[2];
        float e2x = pc[0]-pa[0], e2y = pc[1]-pa[1], e2z = pc[2]-pa[2];
        float cr = (e1y*e2z - e1z*e2y)*(e1y*e2z - e1z*e2y)
                 + (e1z*e2x - e1x*e2z)*(e1z*e2x - e1x*e2z)
                 + (e1x*e2y - e1y*e2x)*(e1x*e2y - e1y*e2x);
        double area = 0.5 * std::sqrt(static_cast<double>(cr));
        if (area < degenThresh) {
            q.degenerateFaces++;
            isDegen[t] = 1;
            q.degenerateTriVerts.insert(q.degenerateTriVerts.end(), {
                pa[0],pa[1],pa[2], pb[0],pb[1],pb[2],
                pb[0],pb[1],pb[2], pc[0],pc[1],pc[2],
                pc[0],pc[1],pc[2], pa[0],pa[1],pa[2] });
        }
    }

    // ---- 3. edge incidence over REAL tris only ----
    auto edgeKey = [](uint32_t x, uint32_t y) -> uint64_t {
        if (x > y) std::swap(x, y);
        return (static_cast<uint64_t>(x) << 32) | y;
    };
    std::unordered_map<uint64_t, int> edgeCount;     // ALL tris (incl degenerate) -> open edges
    std::unordered_map<uint64_t, int> edgeCountNM;   // NON-degenerate tris only -> non-manifold edges
    std::vector<std::vector<uint32_t>> vertEdges(nv);  // neighbors of each vertex
    std::vector<std::vector<uint32_t>> vertTris(nv);   // triangles incident to each vertex
    auto pushEdge = [&](uint32_t wx, uint32_t wy, std::vector<float>& dst) {
        const float* px = &v[weldedToRaw[wx]*3];
        const float* py = &v[weldedToRaw[wy]*3];
        dst.insert(dst.end(), {px[0],px[1],px[2], py[0],py[1],py[2]});
    };
    for (size_t t = 0; t < nt; ++t) {
        uint32_t a = weld[t*3], b = weld[t*3+1], c = weld[t*3+2];
        // edgeCount: ALL tris, skip per-edge zero-length (a==b) so a degenerate tri's
        // SHARED real edges stay shared -> correct open count (matches reference tool).
        // edgeCountNM: only NON-degenerate tris (degenerate faces aren't real surface,
        // so they must not contribute to non-manifold edge counts).
        if (a != b) { edgeCount[edgeKey(a,b)]++; vertEdges[a].push_back(b); vertEdges[b].push_back(a); vertTris[a].push_back((uint32_t)t); vertTris[b].push_back((uint32_t)t); }
        if (b != c) { edgeCount[edgeKey(b,c)]++; vertEdges[b].push_back(c); vertEdges[c].push_back(b); vertTris[b].push_back((uint32_t)t); vertTris[c].push_back((uint32_t)t); }
        if (c != a) { edgeCount[edgeKey(c,a)]++; vertEdges[c].push_back(a); vertEdges[a].push_back(c); vertTris[c].push_back((uint32_t)t); vertTris[a].push_back((uint32_t)t); }
        if (!isDegen[t]) {
            if (a != b) edgeCountNM[edgeKey(a,b)]++;
            if (b != c) edgeCountNM[edgeKey(b,c)]++;
            if (c != a) edgeCountNM[edgeKey(c,a)]++;
        }
    }
    for (const auto& kv : edgeCount) {
        if (kv.second == 1) {
            q.openEdges++;
            pushEdge(static_cast<uint32_t>(kv.first >> 32),
                     static_cast<uint32_t>(kv.first & 0xFFFFFFFFu), q.openEdgeVerts);
        }
    }
    for (const auto& kv : edgeCountNM) {
        if (kv.second > 2) {
            q.nonManifoldEdges++;
            pushEdge(static_cast<uint32_t>(kv.first >> 32),
                     static_cast<uint32_t>(kv.first & 0xFFFFFFFFu), q.nonManifoldEdgeVerts);
        }
    }


    // ---- 4. vertex manifoldness via the LINK graph (not a mesh-wide BFS) ----
    // ponytail: the link of s = its neighbors, with a link-edge between two neighbors
    // that share a triangle with s. On an OPEN surface a mesh-wide BFS from s would
    // traverse the whole sheet -> O(V^2) freeze (bodykit ~5s). The link graph has
    // size = degree(s) (<=~10), so connectivity check is O(degree^2), negligible.
    for (uint32_t s = 0; s < nv; ++s) {
        if (vertTris[s].empty()) continue;
        // unique neighbors of s
        std::vector<uint32_t> nb;
        for (uint32_t u : vertEdges[s]) if (u != s) nb.push_back(u);
        std::sort(nb.begin(), nb.end());
        nb.erase(std::unique(nb.begin(), nb.end()), nb.end());
        if (nb.empty()) continue;
        // map neighbor -> local index in nb
        std::unordered_map<uint32_t, uint32_t> nbi;
        for (uint32_t i = 0; i < nb.size(); ++i) nbi[nb[i]] = i;
        // link adjacency among nb
        std::vector<std::vector<uint32_t>> link(nb.size());
        for (uint32_t t : vertTris[s]) {
            uint32_t a = weld[t*3], b = weld[t*3+1], c = weld[t*3+2];
            uint32_t x = 0, y = 0, n = 0;
            if (a != s) { x = a; n++; } if (b != s) { if (n==0){x=b;n++;} else y=b; }
            if (c != s) { if (n==0){x=c;n++;} else y=c; }
            auto ix = nbi.find(x), iy = nbi.find(y);
            if (ix != nbi.end() && iy != nbi.end()) {
                link[ix->second].push_back(iy->second);
                link[iy->second].push_back(ix->second);
            }
        }
        // BFS over link graph from nb[0]
        std::vector<char> vis(nb.size(), 0);
        std::vector<uint32_t> stack{ 0 };
        vis[0] = 1; size_t reached = 0;
        while (!stack.empty()) {
            uint32_t cur = stack.back(); stack.pop_back(); reached++;
            for (uint32_t w : link[cur]) if (!vis[w]) { vis[w] = 1; stack.push_back(w); }
        }
        if (reached < nb.size()) {
            for (size_t i = 0; i < nb.size(); ++i)
                pushEdge(nb[i], nb[(i+1) % nb.size()], q.nonManifoldEdgeVerts);
                q.nonManifoldVerts++;
        }
    }

    q.watertight = (q.openEdges == 0 && q.nonManifoldEdges == 0 && q.nonManifoldVerts == 0 && q.degenerateFaces == 0);
    return q;
}
