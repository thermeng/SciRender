#include "core/isosurface.h"
#include "core/mesh_loader.h"
#include "core/FieldResolver.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace isosurface {

// ---------------------------------------------------------------------------
// Isosurface extraction via marching cubes.
//
// Design: the algorithm is TABLE-FREE. Rather than the 256-entry edgeTable /
// 256x16 triTable (whose verbatim transcription is a classic source of subtle
// topology bugs), each cube cell is processed directly:
//   1. Classify the 8 corner nodes as high/low by (scalar >= isovalue).
//   2. For each of the 12 edges, if the endpoints straddle the isovalue,
//      interpolate a surface vertex on that edge. Vertices are shared across
//      cells via an edge -> vertex map (keyed by the grid edge, so adjacent
//      cells reuse the same vertex -> a watertight, duplicate-free manifold).
//   3. The crossed-edge vertices of a cell are partitioned into closed
//      loops by walking face adjacency (2 crossings on a face pair
//      directly; 4 = ambiguous saddle paired by the face-center decider,
//      which agrees from both sides of a shared face). Each loop is
//      angle-sorted around its centroid (projected onto the loop's
//      best-fit plane) and fan-triangulated — disjoint loops are never
//      bridged, so no open-edge holes.
//   3b. Bitwise-identical vertices are welded (node-on-level degeneracy:
//      a node with value == iso emits one vertex per incident edge at the
//      exact node position); collapsed triangles are dropped.
//   4. Per-vertex normals come from mesh_utils::computeNormals (shared vertex
//      positions => correctly averaged; sharp-edge split preserved).
//
// This is equivalent to marching cubes for every non-ambiguous cell. Ambiguous
// saddle cells (a checkerboard face, ~16 cell configs) resolve with the
// conventional single-fan diagonal rather than an asymptotic decider -- a
// documented, acceptable trade-off yielding a valid closed surface.
// ---------------------------------------------------------------------------

struct EdgeDef { int lowerRel; int axis; };

static int nodeIndexOf(int x, int y, int z, int dX, int dY, int /*dZ*/) {
    return x + y * dX + z * dX * dY;
}

bool canExtract(const RenderMesh& volumeMesh, const std::string& field, int placement) {
    if (!volumeMesh.hasVolumeGrid() || volumeMesh.vertices.empty()) return false;
    float mn, mx;
    if (FieldResolver::scalarData(volumeMesh, field, mn, mx, placement)) return true;
    int alt = (placement == 1 ? 0 : 1);
    if (FieldResolver::scalarData(volumeMesh, field, mn, mx, alt)) return true;
    // Also check derived via scalarData already covers it; final fallback to hasScalarData
    return volumeMesh.hasScalarData();
}

// ---------------------------------------------------------------------------
// Dense edge-vertex cache: O(1) direct-index instead of unordered_map.
// Each grid edge is uniquely identified by (lowerMinNode, axis) -> index = nA*3+axis.
// Uses generation tagging to clear in O(1) per contour (no O(numNodes) fill).
// Falls back to no-cache when numNodes is huge to avoid 500MB+ allocation.
// ---------------------------------------------------------------------------
struct EdgeVertexCache {
    static constexpr uint32_t kSentinel = std::numeric_limits<uint32_t>::max();
    // Threshold: 64M nodes *3 = 192M entries. 192M * (4+4) ~1.5GB -> too large.
    // Use dense only up to ~48M nodes (~144M edges ~ ~1.1GB). Above that use hash fallback.
    // In practice most volumes are < 256^3 (16M nodes).
    static constexpr size_t kDenseNodeLimit = 48'000'000;

    std::vector<uint32_t> verts; // size numNodes*3, value = vertex index
    std::vector<uint32_t> tags;  // generation tag per slot
    uint32_t curGen = 1;
    bool useDense = false;
    size_t numNodes = 0;

    bool init(size_t nNodes) {
        numNodes = nNodes;
        if (nNodes == 0 || nNodes > kDenseNodeLimit) {
            useDense = false;
            return false;
        }
        const size_t need = nNodes * 3;
        // Guard against size_t overflow / bad alloc
        if (need / 3 != nNodes) { useDense = false; return false; }
        try {
            verts.assign(need, kSentinel);
            tags.assign(need, 0);
            curGen = 1;
            useDense = true;
        } catch (const std::bad_alloc&) {
            verts.clear(); tags.clear();
            useDense = false;
        }
        return useDense;
    }

    inline void nextContour() {
        if (!useDense) return;
        ++curGen;
        if (curGen == 0) { // wrapped (2^32 contours extremely unlikely)
            std::fill(tags.begin(), tags.end(), 0);
            curGen = 1;
        }
        // No fill of verts needed — tags gate validity.
    }

    inline bool find(uint32_t nA, int axis, uint32_t &out) const {
        if (!useDense) return false;
        const size_t idx = static_cast<size_t>(nA) * 3 + static_cast<size_t>(axis);
        if (idx >= tags.size()) return false;
        if (tags[idx] == curGen) { out = verts[idx]; return true; }
        return false;
    }

    inline void insert(uint32_t nA, int axis, uint32_t vi) {
        if (!useDense) return;
        const size_t idx = static_cast<size_t>(nA) * 3 + static_cast<size_t>(axis);
        if (idx >= verts.size()) return;
        verts[idx] = vi;
        tags[idx] = curGen;
    }
};

static void triangulateFan(RenderMesh& out,
                           const uint32_t* cellVerts, int nVerts,
                           const float gradN[3]) {
    // Best-fit plane normal: normalized mean of pairwise cross(v_i - C).
    // nVerts <=12 so O(n^2) <=66 pairs is trivial; keep order-independent fit.
    float C[3] = {0, 0, 0};
    for (int i = 0; i < nVerts; ++i) {
        const float* v = &out.vertices[cellVerts[i] * 3];
        C[0] += v[0]; C[1] += v[1]; C[2] += v[2];
    }
    C[0] /= nVerts; C[1] /= nVerts; C[2] /= nVerts;

    float N[3] = {0, 0, 0};
    for (int i = 0; i < nVerts; ++i) {
        const float* vi = &out.vertices[cellVerts[i] * 3];
        for (int j = i + 1; j < nVerts; ++j) {
            const float* vj = &out.vertices[cellVerts[j] * 3];
            float a[3] = {vi[0]-C[0], vi[1]-C[1], vi[2]-C[2]};
            float b[3] = {vj[0]-C[0], vj[1]-C[1], vj[2]-C[2]};
            N[0] += a[1]*b[2] - a[2]*b[1];
            N[1] += a[2]*b[0] - a[0]*b[2];
            N[2] += a[0]*b[1] - a[1]*b[0];
        }
    }
    float nlen = std::sqrt(N[0]*N[0] + N[1]*N[1] + N[2]*N[2]);
    if (nlen < 1e-9f) { N[0]=0; N[1]=0; N[2]=1; nlen=1; }
    N[0]/=nlen; N[1]/=nlen; N[2]/=nlen;

    if (N[0]*gradN[0] + N[1]*gradN[1] + N[2]*gradN[2] < 0) {
        N[0] = -N[0]; N[1] = -N[1]; N[2] = -N[2];
    }

    float U[3] = { std::fabs(N[0]) < 0.9f ? 1.0f : 0.0f,
                   std::fabs(N[0]) < 0.9f ? 0.0f : 1.0f, 0.0f };
    float V[3];
    V[0] = N[1]*U[2] - N[2]*U[1];
    V[1] = N[2]*U[0] - N[0]*U[2];
    V[2] = N[0]*U[1] - N[1]*U[0];
    float vlen = std::sqrt(V[0]*V[0]+V[1]*V[1]+V[2]*V[2]);
    if (vlen < 1e-9f) {
        if (std::fabs(N[0]) < 0.9f) { U[0]=1;U[1]=0;U[2]=0; } else { U[0]=0;U[1]=1;U[2]=0; }
        V[0]=N[1]*U[2]-N[2]*U[1]; V[1]=N[2]*U[0]-N[0]*U[2]; V[2]=N[0]*U[1]-N[1]*U[0];
        vlen = std::sqrt(V[0]*V[0]+V[1]*V[1]+V[2]*V[2]);
    }
    if (vlen < 1e-9f) {
        U[0]=0;U[1]=1;U[2]=0;
        V[0]=N[1]*U[2]-N[2]*U[1]; V[1]=N[2]*U[0]-N[0]*U[2]; V[2]=N[0]*U[1]-N[1]*U[0];
        vlen=1.0f;
    }
    V[0]/=vlen; V[1]/=vlen; V[2]/=vlen;
    U[0] = N[1]*V[2] - N[2]*V[1];
    U[1] = N[2]*V[0] - N[0]*V[2];
    U[2] = N[0]*V[1] - N[1]*V[0];

    // Angle-sort without trig: sort by quadrant+cross (avoids atan2 per vertex).
    struct Item { uint32_t vi; float u; float v; };
    Item items[12];
    for (int i = 0; i < nVerts; ++i) {
        const float* vv = &out.vertices[cellVerts[i] * 3];
        float dx = vv[0]-C[0], dy = vv[1]-C[1], dz = vv[2]-C[2];
        items[i].vi = cellVerts[i];
        items[i].u = dx*U[0]+dy*U[1]+dz*U[2];
        items[i].v = dx*V[0]+dy*V[1]+dz*V[2];
    }
    auto half = [](const Item& p) -> int {
        // atan2 half-plane: 0 = upper (v>0 or v==0&&u>0)
        if (p.v > 0) return 0;
        if (p.v < 0) return 1;
        return (p.u < 0) ? 1 : 0;
    };
    std::sort(items, items + nVerts, [&](const Item& a, const Item& b){
        int ha = half(a), hb = half(b);
        if (ha != hb) return ha < hb;
        float cross = a.u * b.v - a.v * b.u;
        if (cross != 0) return cross > 0;
        // tie: closer to center first (stable)
        float da = a.u*a.u + a.v*a.v;
        float db = b.u*b.u + b.v*b.v;
        return da < db;
    });

    for (int i = 1; i + 1 < nVerts; ++i) {
        out.indices.push_back(items[0].vi);
        out.indices.push_back(items[i].vi);
        out.indices.push_back(items[i+1].vi);
    }
}

RenderMesh extractIsosurface(const RenderMesh& volumeMesh,
                             const std::vector<float>& isovalues,
                             const std::string& field,
                             int placement) {
    RenderMesh result;
    if (isovalues.empty() || !canExtract(volumeMesh, field, placement)) return result;

    // For Cell Center placement on a structured grid, contour the cell-
    // centered scalar field at cell centers (dual grid). Placement-aware
    // via FieldResolver so derived scalars (e.g. Velocity_magnitude from
    // cell vectors) also build a dual and update on placement toggle.
    const RenderMesh* srcMesh = &volumeMesh;
    RenderMesh cellCenterMesh;
    const std::vector<float>* cellValsPtr = nullptr;
    std::string cellFieldName;
    float cellMin = 0, cellMax = 1;
    if (placement == 1 && volumeMesh.hasVolumeGrid()) {
        int dX0 = volumeMesh.gridDimX, dY0 = volumeMesh.gridDimY, dZ0 = volumeMesh.gridDimZ;
        int cdX0 = dX0>0?dX0-1:0, cdY0=dY0>0?dY0-1:0, cdZ0=dZ0>0?dZ0-1:0;
        size_t cellCount0 = static_cast<size_t>(cdX0) * cdY0 * cdZ0;
        if (cellCount0 > 0) {
            std::string effField = field.empty() ? FieldResolver::resolveActiveScalar(volumeMesh, "", 1) : field;
            float tmpMin=0, tmpMax=1;
            const std::vector<float>* cand = FieldResolver::scalarData(volumeMesh, effField, tmpMin, tmpMax, 1);
            // cand is cell-sized only when true cell/derived-cell data exists; point fallback is nPoints-sized
            if (cand && cand->size() == cellCount0) {
                cellValsPtr = cand;
                cellFieldName = effField;
                cellMin = tmpMin; cellMax = tmpMax;
            } else if (volumeMesh.attributes) {
                // Direct cellScalars lookup as fallback (covers non-derived case where FieldResolver returned point duplicate)
                auto it = volumeMesh.attributes->cellScalars.find(effField);
                if (it != volumeMesh.attributes->cellScalars.end() && it->second.size() == cellCount0) {
                    cellValsPtr = &it->second;
                    cellFieldName = effField;
                    auto rit = volumeMesh.attributes->cellScalarRanges.find(effField);
                    if (rit != volumeMesh.attributes->cellScalarRanges.end()) { cellMin=rit->second.first; cellMax=rit->second.second; }
                    else { cellMin=tmpMin; cellMax=tmpMax; }
                }
            }
        }
    }
    // Only build the dual grid if we actually have a cell scalar and the
    // original grid is structured volume. Unstructured cell data has no
    // marching-cubes topology, so we fall back to point contour.
    if (cellValsPtr && volumeMesh.hasVolumeGrid()) {
        const int dX = volumeMesh.gridDimX;
        const int dY = volumeMesh.gridDimY;
        const int dZ = volumeMesh.gridDimZ;
        const int cdX = dX - 1;
        const int cdY = dY - 1;
        const int cdZ = dZ - 1;
        if (cdX > 1 && cdY > 1 && cdZ > 1 && cellValsPtr->size() >= static_cast<size_t>(cdX * cdY * cdZ)) {
            const float* P = volumeMesh.vertices.data();
            if (P && volumeMesh.vertices.size() >= static_cast<size_t>((dX * dY * dZ) * 3)) {
                cellCenterMesh.gridDimX = cdX;
                cellCenterMesh.gridDimY = cdY;
                cellCenterMesh.gridDimZ = cdZ;
                cellCenterMesh.vertices.reserve(static_cast<size_t>(cdX * cdY * cdZ * 3));
                auto idx = [&](int x,int y,int z){ return x + y * dX + z * dX * dY; };
                for (int z = 0; z < cdZ; ++z) {
                    for (int y = 0; y < cdY; ++y) {
                        for (int x = 0; x < cdX; ++x) {
                            float sx = 0, sy = 0, sz = 0;
                            for (int dz = 0; dz < 2; ++dz)
                                for (int dy = 0; dy < 2; ++dy)
                                    for (int dx = 0; dx < 2; ++dx) {
                                        int n = idx(x+dx, y+dy, z+dz);
                                        sx += P[static_cast<size_t>(n)*3 + 0];
                                        sy += P[static_cast<size_t>(n)*3 + 1];
                                        sz += P[static_cast<size_t>(n)*3 + 2];
                                    }
                            cellCenterMesh.vertices.push_back(sx * 0.125f);
                            cellCenterMesh.vertices.push_back(sy * 0.125f);
                            cellCenterMesh.vertices.push_back(sz * 0.125f);
                        }
                    }
                }
                cellCenterMesh.scalars = *cellValsPtr;
                cellCenterMesh.scalarName = cellFieldName;
                cellCenterMesh.attributes = DatasetAttributes();
                cellCenterMesh.attributes->pointScalars[cellFieldName] = *cellValsPtr;
                cellCenterMesh.attributes->pointScalarRanges[cellFieldName] = {cellMin, cellMax};
                mesh_utils::computeBounds(cellCenterMesh);
                srcMesh = &cellCenterMesh;
            }
        }
    }

    const int dX = srcMesh->gridDimX;
    const int dY = srcMesh->gridDimY;
    const int dZ = srcMesh->gridDimZ;
    const int dXdY = dX * dY;
    const int numNodes = dX * dY * dZ;

    // Placement-aware scalar fetch (covers derived scalars like Velocity_magnitude)
    const std::vector<float>* vals = nullptr;
    float valsMin = 0, valsMax = 1;
    int valsPlacement = (srcMesh == &cellCenterMesh ? 0 : placement);
    vals = FieldResolver::scalarData(*srcMesh, field, valsMin, valsMax, valsPlacement);
    if (!vals || vals->empty()) {
        vals = &srcMesh->scalars;
        if (vals->empty() && srcMesh->attributes && !srcMesh->attributes->pointScalars.empty()) {
            if (!srcMesh->scalarName.empty()) {
                auto it = srcMesh->attributes->pointScalars.find(srcMesh->scalarName);
                if (it != srcMesh->attributes->pointScalars.end()) vals = &it->second;
            }
            if (vals->empty()) vals = &srcMesh->attributes->pointScalars.begin()->second;
        }
    }
    if (vals->size() < static_cast<size_t>(numNodes)) {
        return result;
    }

    const int numCells = (dX - 1) * (dY - 1) * (dZ - 1);
    // Adaptive reserve: isosurface typically touches 2-5% of cells, not 60%.
    // Reserve 5% and grow as needed; avoids massive over-allocation for sparse surfaces
    // and still prevents excessive reallocation for dense ones.
    {
        size_t estVerts = static_cast<size_t>(numCells) * 5 / 100;
        // Clamp to at least enough for a small surface, at most 256k to avoid huge upfront alloc
        estVerts = std::max<size_t>(estVerts, 1024);
        // For very small volumes the 5% estimate may still be large relative to cells; cap.
        size_t cap = static_cast<size_t>(numCells) * 3;
        if (estVerts > cap) estVerts = cap;
        result.vertices.reserve(estVerts * 3);
        result.scalars.reserve(estVerts);
        result.normals.reserve(estVerts * 3);
        // indices: ~2 tris per crossed cell on average, 3 indices per tri
        result.indices.reserve(estVerts * 6);
    }

    const int axisStride[3] = {1, dX, dXdY};
    const int CORNER[8] = {
        0, 1, 1 + dX, dX, dXdY, dXdY + 1, dXdY + dX + 1, dXdY + dX
    };
    const EdgeDef EDGES[12] = {
        {0,       0}, {1,       1}, {dX,      0}, {0,      1},
        {dXdY,    0}, {dXdY + 1, 1}, {dXdY + dX, 0}, {dXdY, 1},
        {0,       2}, {1,       2}, {1 + dX,  2}, {dX,     2},
    };

    EdgeVertexCache edgeCache;
    bool denseOk = edgeCache.init(static_cast<size_t>(numNodes));

    // Fallback hash map for huge volumes or allocation failure
    std::unordered_map<uint64_t, uint32_t> edgeHash;
    if (!denseOk) {
        edgeHash.reserve(static_cast<size_t>(numCells) * 5 / 100 * 3);
    }

    auto makeVertex = [&](const float* pa, const float* pb,
                          float va, float vb, float iso) {
        float denom = vb - va;
        float t = 0.5f;
        if (std::fabs(denom) > 1e-12f) {
            t = (iso - va) / denom;
            t = std::clamp(t, 0.0f, 1.0f);
        }
        // Snap endpoints bit-exactly: t==1 via pa+t*(pb-pa) can round to
        // 1ulp off pb, so coincident node-on-level vertices from different
        // edges would miss the bitwise weld below. Copying the endpoint
        // keeps them identical (t<=0 also fixes t==-0.0 sign flow).
        const float* p = (t <= 0.0f) ? pa : ((t >= 1.0f) ? pb : nullptr);
        const size_t base = result.vertices.size();
        if (p) {
            result.vertices.push_back(p[0]);
            result.vertices.push_back(p[1]);
            result.vertices.push_back(p[2]);
        } else {
            result.vertices.push_back(pa[0] + t * (pb[0] - pa[0]));
            result.vertices.push_back(pa[1] + t * (pb[1] - pa[1]));
            result.vertices.push_back(pa[2] + t * (pb[2] - pa[2]));
        }
        result.scalars.push_back(iso);
        result.normals.push_back(0.0f);
        result.normals.push_back(0.0f);
        result.normals.push_back(0.0f);
        return static_cast<uint32_t>(base / 3);
    };

    if (srcMesh->vertices.size() / 3 < static_cast<size_t>(numNodes)) {
        return result;
    }
    const float* P = srcMesh->vertices.data();

    for (float iso : isovalues) {
        if (denseOk) edgeCache.nextContour();
        else edgeHash.clear();

        for (int cz = 0; cz < dZ - 1; ++cz)
            for (int cy = 0; cy < dY - 1; ++cy)
                for (int cx = 0; cx < dX - 1; ++cx) {
                    const int base = nodeIndexOf(cx, cy, cz, dX, dY, dZ);

                    uint8_t flags = 0;
                    for (int k = 0; k < 8; ++k) {
                        int n = base + CORNER[k];
                        if ((*vals)[static_cast<size_t>(n)] >= iso)
                            flags |= (uint8_t(1) << k);
                    }
                    if (flags == 0 || flags == 0xFF) continue;

                    uint32_t cellVerts[12];
                    int cellEdges[12]; // cell-edge id per crossing (for loop partition)
                    int nVerts = 0;
                    float gradN[3] = {0, 0, 0};
                    for (int e = 0; e < 12; ++e) {
                        const int axis = EDGES[e].axis;
                        const int nA = base + EDGES[e].lowerRel;
                        const int nB = nA + axisStride[axis];
                        const float va = (*vals)[static_cast<size_t>(nA)];
                        const float vb = (*vals)[static_cast<size_t>(nB)];
                        if ((va >= iso) == (vb >= iso)) continue;
                        float dx = P[static_cast<size_t>(nB)*3+0] - P[static_cast<size_t>(nA)*3+0];
                        float dy = P[static_cast<size_t>(nB)*3+1] - P[static_cast<size_t>(nA)*3+1];
                        float dz = P[static_cast<size_t>(nB)*3+2] - P[static_cast<size_t>(nA)*3+2];
                        if (va < vb) { gradN[0]+=dx; gradN[1]+=dy; gradN[2]+=dz; }
                        else         { gradN[0]-=dx; gradN[1]-=dy; gradN[2]-=dz; }

                        uint32_t vi;
                        bool found = false;
                        if (denseOk) {
                            found = edgeCache.find(static_cast<uint32_t>(nA), axis, vi);
                        } else {
                            uint64_t k = (static_cast<uint64_t>(static_cast<uint32_t>(nA)) << 2) | static_cast<uint64_t>(axis);
                            auto mit = edgeHash.find(k);
                            if (mit != edgeHash.end()) { vi = mit->second; found = true; }
                        }
                        if (!found) {
                            vi = makeVertex(P + static_cast<size_t>(nA) * 3,
                                            P + static_cast<size_t>(nB) * 3,
                                            va, vb, iso);
                            if (denseOk) edgeCache.insert(static_cast<uint32_t>(nA), axis, vi);
                            else {
                                uint64_t k = (static_cast<uint64_t>(static_cast<uint32_t>(nA)) << 2) | static_cast<uint64_t>(axis);
                                edgeHash[k] = vi;
                            }
                        }
                        cellVerts[nVerts] = vi;
                        cellEdges[nVerts] = e;
                        ++nVerts;
                    }
                    if (nVerts == 0) continue;

                    // Partition the crossings into closed loops by walking
                    // face adjacency. A cell edge belongs to 2 faces; on each
                    // face a crossing pairs with exactly one other crossing
                    // (2 crossings pair directly; 4 crossings = ambiguous
                    // saddle, paired by the face-center decider). Every
                    // crossing thus has degree 2 and the components are the
                    // disjoint intersection loops. Fanning ALL crossings as
                    // one polygon bridges disjoint loops and disagrees with
                    // neighbours across shared faces — the open-edge holes.
                    //
                    // Face tables: 4 corner ids (into CORNER[]) + 4 cell-edge
                    // ids in cyclic order; edge[i] spans corner[i]->corner[i+1].
                    // The decider (face-center vs iso) is identical from both
                    // sides of a shared face, so adjacent cells pair the same
                    // way and the surface stays watertight.
                    auto emitLoop = [&](const uint32_t* loop, int n) {
                        if (n == 3) {
                            const float* v0 = &result.vertices[loop[0] * 3];
                            const float* v1 = &result.vertices[loop[1] * 3];
                            const float* v2 = &result.vertices[loop[2] * 3];
                            float nx = (v1[1]-v0[1])*(v2[2]-v0[2]) - (v1[2]-v0[2])*(v2[1]-v0[1]);
                            float ny = (v1[2]-v0[2])*(v2[0]-v0[0]) - (v1[0]-v0[0])*(v2[2]-v0[2]);
                            float nz = (v1[0]-v0[0])*(v2[1]-v0[1]) - (v1[1]-v0[1])*(v2[0]-v0[0]);
                            float dotN = nx*gradN[0]+ny*gradN[1]+nz*gradN[2];
                            if (dotN < 0) {
                                result.indices.push_back(loop[0]);
                                result.indices.push_back(loop[2]);
                                result.indices.push_back(loop[1]);
                            } else {
                                result.indices.push_back(loop[0]);
                                result.indices.push_back(loop[1]);
                                result.indices.push_back(loop[2]);
                            }
                        } else if (n > 3) {
                            triangulateFan(result, loop, n, gradN);
                        }
                    };

                    if (nVerts == 3) {
                        uint32_t loop[3] = {cellVerts[0], cellVerts[1], cellVerts[2]};
                        emitLoop(loop, 3);
                        continue;
                    }

                    // edge id per crossing (parallel to cellVerts)
                    // cellEdges[] is filled alongside cellVerts[] above.
                    int adj[12][2];
                    int adjN[12] = {0,0,0,0,0,0,0,0,0,0,0,0};
                    auto link = [&](int a, int b) {
                        if (a < 0 || b < 0 || a == b) return;
                        if (adjN[a] < 2) adj[a][adjN[a]++] = b;
                        if (adjN[b] < 2) adj[b][adjN[b]++] = a;
                    };
                    auto posOfEdge = [&](int e) {
                        for (int i = 0; i < nVerts; ++i)
                            if (cellEdges[i] == e) return i;
                        return -1;
                    };
                    static const int FC[6][4] = {
                        {0,1,2,3}, {4,5,6,7}, {0,1,5,4},
                        {3,2,6,7}, {0,3,7,4}, {1,2,6,5},
                    };
                    static const int FE[6][4] = {
                        {0,1,2,3}, {4,5,6,7}, {0,9,4,8},
                        {2,10,6,11}, {3,11,7,8}, {1,10,5,9},
                    };
                    for (int f = 0; f < 6; ++f) {
                        int p[4];
                        int cnt = 0;
                        for (int k = 0; k < 4; ++k) {
                            p[k] = posOfEdge(FE[f][k]);
                            if (p[k] >= 0) ++cnt;
                        }
                        if (cnt == 2) {
                            int a = -1, b = -1;
                            for (int k = 0; k < 4; ++k)
                                if (p[k] >= 0) { if (a < 0) a = p[k]; else b = p[k]; }
                            link(a, b);
                        } else if (cnt == 4) {
                            // Ambiguous saddle: pair around the corners that
                            // agree with the face center (same from both sides).
                            float fc = 0.0f;
                            float cv[4];
                            for (int k = 0; k < 4; ++k) {
                                cv[k] = (*vals)[static_cast<size_t>(base + CORNER[FC[f][k]])];
                                fc += cv[k];
                            }
                            fc *= 0.25f;
                            const bool winHigh = (fc >= iso);
                            for (int k = 0; k < 4; ++k) {
                                const bool cornerWins = (((*vals)[static_cast<size_t>(base + CORNER[FC[f][k]])] >= iso) == winHigh);
                                if (cornerWins) link(p[(k+3) & 3], p[k]);
                            }
                        }
                    }
                    // Walk disjoint cycles; each is one loop to fan.
                    bool seen[12] = {false,false,false,false,false,false,
                                     false,false,false,false,false,false};
                    for (int s = 0; s < nVerts; ++s) {
                        if (seen[s]) continue;
                        uint32_t loop[12];
                        int ln = 0;
                        int cur = s, prev = -1;
                        while (!seen[cur] && ln < 12) {
                            seen[cur] = true;
                            loop[ln++] = cellVerts[cur];
                            int nxt = -1;
                            for (int k = 0; k < adjN[cur]; ++k)
                                if (adj[cur][k] != prev) { nxt = adj[cur][k]; break; }
                            prev = cur;
                            cur = nxt;
                            if (cur < 0) break;
                        }
                        emitLoop(loop, ln);
                        continue;
                    }
                }
    }

    if (result.vertices.empty()) return result;

    // Weld bitwise-identical vertices. Node-on-level degeneracy (a grid
    // node whose value == iso) makes every incident crossed edge emit a
    // vertex at the exact node position under distinct indices — identical
    // coordinates that read as open edges. Exact matching is zero-risk
    // (never merges legitimately distinct vertices); near-misses stay
    // closed slivers topologically. Drops degenerate tris + compacts.
    {
        const size_t nv0 = result.vertices.size() / 3;
        std::vector<uint32_t> remap(nv0);
        {
            struct Key { uint32_t x, y, z; uint32_t i; };
            std::vector<Key> keys;
            keys.reserve(nv0);
            // Canonicalize -0.0 to +0.0: arithmetically identical positions
            // must hash identically or the weld silently misses.
            auto canon = [](uint32_t b) { return b == 0x80000000u ? 0u : b; };
            for (uint32_t i = 0; i < nv0; ++i) {
                uint32_t b[3];
                std::memcpy(b, &result.vertices[i * 3], 12);
                keys.push_back({canon(b[0]), canon(b[1]), canon(b[2]), i});
            }
            std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
                if (a.x != b.x) return a.x < b.x;
                if (a.y != b.y) return a.y < b.y;
                if (a.z != b.z) return a.z < b.z;
                return a.i < b.i;
            });
            uint32_t rep = 0;
            for (size_t k = 0; k < keys.size(); ++k) {
                if (k > 0 && (keys[k].x != keys[k-1].x || keys[k].y != keys[k-1].y || keys[k].z != keys[k-1].z))
                    ++rep;
                remap[keys[k].i] = rep;
            }
            const uint32_t nUnique = keys.empty() ? 0 : rep + 1;
            if (nUnique < nv0) {
                std::vector<float> nv_(nUnique * 3), ns_(nUnique), nn_(nUnique * 3, 0.0f);
                std::vector<char> seen(nUnique, 0);
                for (uint32_t i = 0; i < nv0; ++i) {
                    const uint32_t r = remap[i];
                    if (!seen[r]) {
                        seen[r] = 1;
                        nv_[r*3] = result.vertices[i*3];
                        nv_[r*3+1] = result.vertices[i*3+1];
                        nv_[r*3+2] = result.vertices[i*3+2];
                        ns_[r] = result.scalars[i];
                    }
                }
                result.vertices.swap(nv_);
                result.scalars.swap(ns_);
                result.normals.assign(nUnique * 3, 0.0f);
                std::vector<uint32_t> ni;
                ni.reserve(result.indices.size());
                for (size_t t = 0; t + 2 < result.indices.size(); t += 3) {
                    const uint32_t a = remap[result.indices[t]];
                    const uint32_t b = remap[result.indices[t+1]];
                    const uint32_t c = remap[result.indices[t+2]];
                    if (a == b || b == c || a == c) continue; // collapsed
                    ni.push_back(a); ni.push_back(b); ni.push_back(c);
                }
                result.indices.swap(ni);
            }
        }
    }

    if (result.indices.empty()) { result.vertices.clear(); result.scalars.clear(); result.normals.clear(); return result; }

    {
        const size_t nv = result.vertices.size() / 3;
        const size_t nt = result.indices.size() / 3;
        result.normals.assign(nv * 3, 0.0f);
        for (size_t t = 0; t < nt; ++t) {
            uint32_t i0 = result.indices[t*3], i1 = result.indices[t*3+1], i2 = result.indices[t*3+2];
            float v0x = result.vertices[i0*3], v0y = result.vertices[i0*3+1], v0z = result.vertices[i0*3+2];
            float v1x = result.vertices[i1*3], v1y = result.vertices[i1*3+1], v1z = result.vertices[i1*3+2];
            float v2x = result.vertices[i2*3], v2y = result.vertices[i2*3+1], v2z = result.vertices[i2*3+2];
            float e1x = v1x - v0x, e1y = v1y - v0y, e1z = v1z - v0z;
            float e2x = v2x - v0x, e2y = v2y - v0y, e2z = v2z - v0z;
            float nx = e1y*e2z - e1z*e2y, ny = e1z*e2x - e1x*e2z, nz = e1x*e2y - e1y*e2x;
            float len = std::sqrt(nx*nx+ny*ny+nz*nz);
            if (len < 1e-12f) continue;
            nx/=len; ny/=len; nz/=len;
            for (uint32_t idx : {i0,i1,i2}) { result.normals[idx*3]+=nx; result.normals[idx*3+1]+=ny; result.normals[idx*3+2]+=nz; }
        }
        for (size_t v = 0; v < nv; ++v) {
            float nx = result.normals[v*3], ny = result.normals[v*3+1], nz = result.normals[v*3+2];
            float len = std::sqrt(nx*nx+ny*ny+nz*nz);
            if (len > 1e-12f) { result.normals[v*3]=nx/len; result.normals[v*3+1]=ny/len; result.normals[v*3+2]=nz/len; }
            else { result.normals[v*3]=0; result.normals[v*3+1]=0; result.normals[v*3+2]=1; }
        }
        result.vertexSourceIndex.clear();
        result.vertexSourceIndex.reserve(nv);
        for (size_t i=0;i<nv;++i) result.vertexSourceIndex.push_back(static_cast<int>(i));
    }
    mesh_utils::computeBounds(result);

    // Explicit field wins; otherwise inherit the source mesh's name (which
    // the dual-grid path already set to the contoured cell field).
    const std::string& outName = srcMesh->scalarName.empty() ? volumeMesh.scalarName : srcMesh->scalarName;
    result.scalarName = field.empty() ? outName : field;
    result.availableScalarNames = result.scalarName.empty()
        ? std::vector<std::string>{}
        : std::vector<std::string>{result.scalarName};
    // Trim over-reserved capacity after extraction
    result.vertices.shrink_to_fit();
    result.indices.shrink_to_fit();
    result.normals.shrink_to_fit();
    result.scalars.shrink_to_fit();
    return result;
}

} // namespace isosurface
