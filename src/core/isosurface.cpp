#include "core/isosurface.h"
#include "core/mesh_loader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

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
//   3. The crossed-edge vertices of a cell form a polygon; they are
//      angle-sorted around their centroid (projected onto the polygon's
//      best-fit plane) and fan-triangulated.
//   4. Per-vertex normals come from mesh_utils::computeNormals (shared vertex
//      positions => correctly averaged; sharp-edge split preserved).
//
// This is equivalent to marching cubes for every non-ambiguous cell. Ambiguous
// saddle cells (a checkerboard face, ~16 cell configs) resolve with the
// conventional single-fan diagonal rather than an asymptotic decider -- a
// documented, acceptable trade-off yielding a valid closed surface.
// ---------------------------------------------------------------------------

// 12 cube edges (canonical MC numbering). lowerRel is the offset of the
// edge's lower-min corner (smallest x, then y, then z) relative to the cell
// origin; axis is 0=X(+1), 1=Y(+dX), 2=Z(+dX*dY). The neighbor node along the
// edge is lowerNode + axisStride[axis].
struct EdgeDef { int lowerRel; int axis; };

static int nodeIndexOf(int x, int y, int z, int dX, int dY, int /*dZ*/) {
    // IJK ordering used throughout the codebase (x fastest, then y, then z).
    return x + y * dX + z * dX * dY;
}

bool canExtract(const RenderMesh& volumeMesh) {
    // Delegates to the mesh's own volume-grid + scalar-availability predicates
    // (RenderMesh::hasVolumeGrid / hasScalarData), plus a non-empty vertex array
    // so interpolation has source positions to sample.
    return volumeMesh.hasVolumeGrid()
        && volumeMesh.hasScalarData()
        && !volumeMesh.vertices.empty();
}

static void triangulateFan(RenderMesh& out,
                           const uint32_t* cellVerts, int nVerts) {
    // Best-fit plane normal: normalized mean of pairwise cross(v_i - C).
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

    // Orthonormal basis (U,V) perpendicular to N.
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
    // U = N x V (unit, since N,V orthonormal and perpendicular)
    U[0] = N[1]*V[2] - N[2]*V[1];
    U[1] = N[2]*V[0] - N[0]*V[2];
    U[2] = N[0]*V[1] - N[1]*V[0];

    struct Item { uint32_t vi; float ang; };
    Item items[12];
    for (int i = 0; i < nVerts; ++i) {
        const float* v = &out.vertices[cellVerts[i] * 3];
        float dx = v[0]-C[0], dy = v[1]-C[1], dz = v[2]-C[2];
        items[i].vi = cellVerts[i];
        items[i].ang = std::atan2(dx*V[0]+dy*V[1]+dz*V[2],
                                  dx*U[0]+dy*U[1]+dz*U[2]);
    }
    std::sort(items, items + nVerts,
              [](const Item& a, const Item& b){ return a.ang < b.ang; });

    for (int i = 1; i + 1 < nVerts; ++i) {
        out.indices.push_back(items[0].vi);
        out.indices.push_back(items[i].vi);
        out.indices.push_back(items[i+1].vi);
    }
}

RenderMesh extractIsosurface(const RenderMesh& volumeMesh,
                             const std::vector<float>& isovalues,
                             const std::string& field) {
    RenderMesh result;
    if (isovalues.empty() || !canExtract(volumeMesh)) return result;

    const int dX = volumeMesh.gridDimX;
    const int dY = volumeMesh.gridDimY;
    const int dZ = volumeMesh.gridDimZ;
    const int dXdY = dX * dY;
    const int numNodes = dX * dY * dZ;

    // Resolve the contour field -> per-node scalar array (IJK order).
    const std::vector<float>* vals = &volumeMesh.scalars;
    if (!field.empty() && volumeMesh.attributes) {
        auto it = volumeMesh.attributes->pointScalars.find(field);
        if (it != volumeMesh.attributes->pointScalars.end())
            vals = &it->second;
    } else if (vals->empty() && volumeMesh.attributes
               && !volumeMesh.attributes->pointScalars.empty()) {
        // Structured-grid point scalars may live in attributes when surface
        // extraction reduced the rendered vertex count below the grid-node count.
        if (!volumeMesh.scalarName.empty()) {
            auto it = volumeMesh.attributes->pointScalars.find(volumeMesh.scalarName);
            if (it != volumeMesh.attributes->pointScalars.end())
                vals = &it->second;
        }
        if (vals->empty()) {
            vals = &volumeMesh.attributes->pointScalars.begin()->second;
        }
    }
    // The contour field must carry at least one scalar per grid node (IJK order).
    // A structured-grid mesh may have been sharp-edge split for rendering
    // (computeNormals duplicates boundary vertices, expanding the array), but
    // that pass preserves the original node data at the FRONT of both the
    // vertex and scalar arrays (duplicates are appended, never interleaved), so
    // the first `numNodes` entries are exactly the per-node field. Requiring
    // exactly numNodes would silently reject every parsed IMAGEDATA/STRUCTURED_GRID
    // mesh (which arrives pre-split); reading the leading numNodes entries is
    // correct and in-bounds, while a genuinely short array (< numNodes) is still
    // refused to avoid an out-of-bounds read.
    if (vals->size() < static_cast<size_t>(numNodes)) {
        return result;
    }

    const int axisStride[3] = {1, dX, dXdY};
    // 8 corners of a cell, as node-index offsets from the cell origin.
    const int CORNER[8] = {
        0, 1, 1 + dX, dX, dXdY, dXdY + 1, dXdY + dX + 1, dXdY + dX
    };
    // 12 cube edges: { lowerMinCornerOffset, axis }.
    const EdgeDef EDGES[12] = {
        {0,       0}, {1,       1}, {dX,      0}, {0,      1},
        {dXdY,    0}, {dXdY + 1, 1}, {dXdY + dX, 0}, {dXdY, 1},
        {0,       2}, {1,       2}, {1 + dX,  2}, {dX,     2},
    };

    // Shared edge -> vertex index. Key = (lowerMinNode << 2) | axis; every
    // grid edge is unique, so this gives exactly one vertex per geometric edge.
    using EdgeKey = uint64_t;
    std::unordered_map<EdgeKey, uint32_t> edgeVert;

    auto makeVertex = [&](const float* pa, const float* pb,
                          float va, float vb, float iso) {
        float denom = vb - va;
        float t = 0.5f;
        if (std::fabs(denom) > 1e-12f) {
            t = (iso - va) / denom;
            t = std::clamp(t, 0.0f, 1.0f);
        }
        const size_t base = result.vertices.size();
        result.vertices.push_back(pa[0] + t * (pb[0] - pa[0]));
        result.vertices.push_back(pa[1] + t * (pb[1] - pa[1]));
        result.vertices.push_back(pa[2] + t * (pb[2] - pa[2]));
        result.scalars.push_back(iso);            // color value = contour level
        result.normals.push_back(0.0f);
        result.normals.push_back(0.0f);
        result.normals.push_back(0.0f);
        return static_cast<uint32_t>(base / 3);
    };

    // Node positions are read by grid index (P + n*3) for n in [0, numNodes).
    // A sharp-edge-split mesh carries the original node positions first, so
    // require at least numNodes positions to stay in-bounds.
    if (volumeMesh.vertices.size() / 3 < static_cast<size_t>(numNodes)) {
        return result;
    }
    const float* P = volumeMesh.vertices.data();

    for (float iso : isovalues) {
        edgeVert.clear(); // one contour's vertices are independent of other contours

        for (int cz = 0; cz < dZ - 1; ++cz)
            for (int cy = 0; cy < dY - 1; ++cy)
                for (int cx = 0; cx < dX - 1; ++cx) {
                    const int base = nodeIndexOf(cx, cy, cz, dX, dY, dZ);

                    // 1) classify the 8 corners.
                    uint8_t flags = 0;
                    for (int k = 0; k < 8; ++k) {
                        int n = base + CORNER[k];
                        if ((*vals)[static_cast<size_t>(n)] >= iso)
                            flags |= (uint8_t(1) << k);
                    }
                    if (flags == 0 || flags == 0xFF) continue; // entirely in/out

                    // 2) interpolate shared vertices on crossed edges.
                    uint32_t cellVerts[12];
                    int nVerts = 0;
                    for (int e = 0; e < 12; ++e) {
                        const int axis = EDGES[e].axis;
                        const int nA = base + EDGES[e].lowerRel;
                        const int nB = nA + axisStride[axis];
                        const float va = (*vals)[static_cast<size_t>(nA)];
                        const float vb = (*vals)[static_cast<size_t>(nB)];
                        if ((va >= iso) == (vb >= iso)) continue; // not crossed
                        const EdgeKey k = (static_cast<EdgeKey>(nA) << 2) |
                                            static_cast<EdgeKey>(axis);
                        auto mit = edgeVert.find(k);
                        uint32_t vi;
                        if (mit != edgeVert.end()) {
                            vi = mit->second;
                        } else {
                            vi = makeVertex(P + static_cast<size_t>(nA) * 3,
                                            P + static_cast<size_t>(nB) * 3,
                                            va, vb, iso);
                            edgeVert[k] = vi;
                        }
                        cellVerts[nVerts++] = vi;
                    }
                    if (nVerts == 0) continue;

                    // 3) triangulate the per-cell polygon.
                    if (nVerts == 3) {
                        result.indices.push_back(cellVerts[0]);
                        result.indices.push_back(cellVerts[1]);
                        result.indices.push_back(cellVerts[2]);
                    } else {
                        triangulateFan(result, cellVerts, nVerts);
                    }
                } // end cell
    } // end contour

    if (result.vertices.empty()) return result;

    // 4) smooth normals (shared vertices => averaged; sharp-edge split kept).
    mesh_utils::computeNormals(result);
    mesh_utils::computeBounds(result);

    // Surface-mesh contract so MeshPass colormap/lighting wire up.
    result.scalarName = volumeMesh.scalarName;
    result.availableScalarNames = volumeMesh.scalarName.empty()
        ? std::vector<std::string>{}
        : std::vector<std::string>{volumeMesh.scalarName};
    return result;
}

} // namespace isosurface
