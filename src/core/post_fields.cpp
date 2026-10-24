#include "core/post_fields.h"
#include "core/gradient_estimator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <string>
#include <iostream>

namespace postfields {

namespace {

// Tolerance for coordinate recovery, mirroring StreamlineSet's
// range-relative epsilon (absolute floor for near-degenerate axes).
float coordEps(const std::vector<float>& sorted, size_t n) {
    if (n == 0) return 1e-6f;
    const float range = sorted.back() - sorted.front();
    return std::max(std::abs(range) * 1e-5f, 1e-9f);
}

// Unique-ify a sorted coordinate list; false when two distinct nodes share
// a coordinate (should not happen for valid structured input).
bool uniqueCoords(std::vector<float>& v, float eps) {
    std::sort(v.begin(), v.end());
    std::vector<float> u;
    u.reserve(v.size());
    for (float c : v) {
        if (u.empty() || std::abs(c - u.back()) > eps) u.push_back(c);
    }
    v.swap(u);
    return true;
}

} // anonymous

bool isCartesianGrid2D(const CartesianGrid& g) {
    return (g.dX==1 || g.dY==1 || g.dZ==1);
}

bool extractCartesianGrid(const RenderMesh& mesh, CartesianGrid& out) {
    out = CartesianGrid{};
    const int dX = mesh.gridDimX, dY = mesh.gridDimY, dZ = mesh.gridDimZ;
    const int dimsGT1 = (dX>1) + (dY>1) + (dZ>1);
    if (dimsGT1 < 2) return false; // need at least 2D
    // For 3D require all >1, for 2D allow one ==1
    if (dX < 1 || dY < 1 || dZ < 1) return false;
    const size_t nodes = (size_t)dX * (size_t)dY * (size_t)dZ;
    if (mesh.vertices.size() / 3 < nodes) return false;

    std::vector<float> xs, ys, zs;
    xs.reserve(nodes); ys.reserve(nodes); zs.reserve(nodes);
    for (size_t n = 0; n < nodes; ++n) {
        xs.push_back(mesh.vertices[n * 3 + 0]);
        ys.push_back(mesh.vertices[n * 3 + 1]);
        zs.push_back(mesh.vertices[n * 3 + 2]);
    }
    uniqueCoords(xs, coordEps(xs, nodes));
    uniqueCoords(ys, coordEps(ys, nodes));
    uniqueCoords(zs, coordEps(zs, nodes));
    if ((int)xs.size() != dX || (int)ys.size() != dY || (int)zs.size() != dZ)
        return false; // not axis-aligned (curvilinear) or degenerate

    // Positional proof: every vertex must lie on the (xs,ys,zs) lattice
    // (axis-aligned). This is order-independent so it survives the
    // post-load normal-split that reorders / duplicates vertices.
    {
        const float ex = coordEps(xs, xs.size()) + 1e-6f * std::max(1.0f, std::abs(xs.back()));
        const float ey = coordEps(ys, ys.size()) + 1e-6f * std::max(1.0f, std::abs(ys.back()));
        const float ez = coordEps(zs, zs.size()) + 1e-6f * std::max(1.0f, std::abs(zs.back()));
        const size_t vN = mesh.vertices.size() / 3;
        auto contains = [](float v, const std::vector<float>& arr, float eps) {
            for (float a : arr) if (std::abs(v - a) <= eps) return true;
            return false;
        };
        for (size_t n = 0; n < vN; ++n) {
            if (!contains(mesh.vertices[n*3+0], xs, ex)) return false;
            if (!contains(mesh.vertices[n*3+1], ys, ey)) return false;
            if (!contains(mesh.vertices[n*3+2], zs, ez)) return false;
        }
    }

    out.dX = dX; out.dY = dY; out.dZ = dZ;
    out.x.swap(xs); out.y.swap(ys); out.z.swap(zs);
    return true;
}

bool extractPointVectors(const RenderMesh& mesh, const CartesianGrid& grid,
                         const std::string& name, PointVectorField& out) {
    out = PointVectorField{};
    if (name.empty()) return false;
    size_t count = 0;
    const glm::vec3* d = mesh.vectorFieldData(name, count);
    if (!d || count != grid.nodeCount()) return false;
    out.data = d;
    out.count = count;
    return true;
}

bool extractPointVectorsUnstructured(const RenderMesh& mesh, const std::string& name, PointVectorField& out){
    out = PointVectorField{};
    if (name.empty()) return false;
    size_t count=0;
    const glm::vec3* d = mesh.vectorFieldData(name, count);
    if (d && count>0){
        size_t nPt = mesh.vertices.size()/3;
        if (count==nPt || (mesh.sourcePointCount>0 && count==(size_t)mesh.sourcePointCount)) { out.data=d; out.count=count; return true; }
        if (count>0){ out.data=d; out.count=count; return true; }
    }
    // Fallback to cell vectors (treat cell centers as points for LSQ — Q/vorticity still defined)
    const glm::vec3* cd = mesh.cellVectorFieldData(name, count);
    if (cd && count>0){ out.data=cd; out.count=count; return true; }
    return false;
}

bool extractPointScalars(const RenderMesh& mesh, const CartesianGrid& grid,
                         const std::string& name, PointScalarField& out) {
    out = PointScalarField{};
    if (name.empty() || !mesh.attributes) return false;
    auto it = mesh.attributes->pointScalars.find(name);
    if (it == mesh.attributes->pointScalars.end()) return false;
    if (it->second.size() == grid.nodeCount()) {
        out.data = it->second.data();
        out.count = it->second.size();
        return true;
    }
    // Post-split mesh: pointScalars was expanded to post-split vertex count
    // (177 vs 125) via vertexSourceIndex. Recover per-node values by gathering
    // through the source index (first occurrence, exact for point data).
    // Use a stable cache keyed by mesh identity + field name + geometry hash
    // so the returned pointer remains valid for the immediate compute and
    // survives address-reuse across frames.
    if (it->second.size() == mesh.vertices.size() / 3
        && mesh.vertexSourceIndex.size() == it->second.size()
        && mesh.sourcePointCount == (int)grid.nodeCount()) {
        struct CacheEntry {
            uint64_t geomHash = 0;
            std::string fieldName;
            const RenderMesh* meshPtr = nullptr;
            std::vector<float> data;
        };
        thread_local std::unordered_map<std::string, CacheEntry> cache;
        const std::string key = std::to_string(reinterpret_cast<uintptr_t>(&mesh)) + "#" + name + "#" + std::to_string(mesh.geometryHash);
        // Also invalidate when geometryHash changes (address reuse) or mesh ptr changes
        auto itc = cache.find(key);
        if (itc == cache.end() || itc->second.geomHash != mesh.geometryHash || itc->second.meshPtr != &mesh) {
            CacheEntry e;
            e.geomHash = mesh.geometryHash;
            e.fieldName = name;
            e.meshPtr = &mesh;
            e.data.assign(grid.nodeCount(), 0.0f);
            std::vector<char> seen(grid.nodeCount(), 0);
            for (size_t v = 0; v < mesh.vertexSourceIndex.size(); ++v) {
                const uint32_t src = mesh.vertexSourceIndex[v];
                if (src < e.data.size() && !seen[src]) {
                    e.data[src] = it->second[v];
                    seen[src] = 1;
                }
            }
            cache[key] = std::move(e);
            itc = cache.find(key);
        }
        // Prune stale entries to bound memory (keep only entries for current mesh)
        if (cache.size() > 8) {
            for (auto cit = cache.begin(); cit != cache.end(); ) {
                if (cit->second.meshPtr != &mesh) cit = cache.erase(cit);
                else ++cit;
            }
        }
        out.data = itc->second.data.data();
        out.count = itc->second.data.size();
        return true;
    }
    return false;
}

bool extractPointScalarsUnstructured(const RenderMesh& mesh, const std::string& name, PointScalarField& out){
    out = PointScalarField{};
    if (name.empty() || !mesh.attributes) return false;
    auto it = mesh.attributes->pointScalars.find(name);
    if (it!=mesh.attributes->pointScalars.end()){
        size_t nPt = mesh.vertices.size()/3;
        if (it->second.size()==nPt || (mesh.sourcePointCount>0 && it->second.size()==(size_t)mesh.sourcePointCount) || !it->second.empty()){
            out.data=it->second.data(); out.count=it->second.size(); return true;
        }
    }
    // Fallback to cell scalars (extrapolated point copy may exist, else use cell centers)
    auto cit = mesh.attributes->cellScalars.find(name);
    if (cit!=mesh.attributes->cellScalars.end() && !cit->second.empty()){
        out.data=cit->second.data(); out.count=cit->second.size(); return true;
    }
    return false;
}

namespace {

// First derivative of a node-ordered component along one axis.
// Interior: weighted parabolic fit for non-uniform spacing (h1 = x_i - x_{i-1}, h2 = x_{i+1} - x_i, h = h1+h2):
//   f' ≈ -h2/(h1*h) f_{i-1} + (h2-h1)/(h1*h2) f_i + h1/(h2*h) f_{i+1}
// Boundary: 2nd-order one-sided for publication-grade wall accuracy (degrades to 1st-order if n==2).
// Forward (pos==0): f'_0 ≈ -(2*h1+h2)/(h1*(h1+h2)) f0 + (h1+h2)/(h1*h2) f1 - h1/(h2*(h1+h2)) f2
// Backward (pos==n-1): symmetric. Uniform reduces to (-3 f0 +4 f1 - f2)/(2h).
inline float axisDerivative(const float* f, const std::vector<float>& c,
                            size_t idx, int pos, int n, size_t stride) {
    if (n <= 1) return 0.0f;
    if (pos <= 0) {
        if (n >= 3) {
            const float h1 = c[1] - c[0];
            const float h2 = c[2] - c[1];
            const float hsum = h1 + h2;
            if (std::abs(h1) < 1e-12f || std::abs(h2) < 1e-12f || std::abs(hsum) < 1e-12f) return 0.0f;
            return -(2*h1 + h2) / (h1 * hsum) * f[idx]
                 +  hsum / (h1 * h2) * f[idx + stride]
                 -  h1 / (h2 * hsum) * f[idx + 2*stride];
        }
        const float h = c[1] - c[0];
        if (std::abs(h) < 1e-12f) return 0.0f;
        return (f[idx + stride] - f[idx]) / h;
    }
    if (pos >= n - 1) {
        if (n >= 3) {
            const float h1 = c[(size_t)n - 2] - c[(size_t)n - 3];
            const float h2 = c[(size_t)n - 1] - c[(size_t)n - 2];
            const float hsum = h1 + h2;
            if (std::abs(h1) < 1e-12f || std::abs(h2) < 1e-12f || std::abs(hsum) < 1e-12f) return 0.0f;
            return  h2 / (h1 * hsum) * f[idx - 2*stride]
                 -  hsum / (h1 * h2) * f[idx - stride]
                 + (2*h2 + h1) / (h2 * hsum) * f[idx];
        }
        const float h = c[(size_t)n - 1] - c[(size_t)n - 2];
        if (std::abs(h) < 1e-12f) return 0.0f;
        return (f[idx] - f[idx - stride]) / h;
    }
    const float h1 = c[(size_t)pos] - c[(size_t)pos - 1];
    const float h2 = c[(size_t)pos + 1] - c[(size_t)pos];
    const float h = h1 + h2;
    if (std::abs(h1) < 1e-12f || std::abs(h2) < 1e-12f || std::abs(h) < 1e-12f) return 0.0f;
    return -h2 / (h1 * h) * f[idx - stride]
         + (h2 - h1) / (h1 * h2) * f[idx]
         + h1 / (h2 * h) * f[idx + stride];
}

} // anonymous

PostFieldsResult computePostFields(const CartesianGrid& grid,
                                   const PostFieldsRequest& req) {
    PostFieldsResult r;
    const bool needVel = req.wantVorticity || req.wantHelicity || req.wantQ;
    const bool needP = req.wantGradP;
    if (needVel && !req.velocity) { r.error = "velocity field required"; return r; }
    if (needP && !req.pressure) { r.error = "pressure field required"; return r; }
    if (!needVel && !needP) { r.error = "nothing requested"; return r; }

    const size_t N = grid.nodeCount();
    const size_t sx = 1, sy = (size_t)grid.dX, sz = (size_t)grid.dX * (size_t)grid.dY;

    // Split velocity into components once (SoA) for cache-friendly stencils.
    std::vector<float> U, V, W;
    if (needVel) {
        U.resize(N); V.resize(N); W.resize(N);
        for (size_t n = 0; n < N; ++n) {
            U[n] = req.velocity[n].x;
            V[n] = req.velocity[n].y;
            W[n] = req.velocity[n].z;
        }
    }
    const float* P = req.pressure;

    if (req.wantVorticity) { r.vorticity.resize(N); r.hasVorticity = true; }
    if (req.wantHelicity) { r.helicity.resize(N); r.hasHelicity = true; }
    if (req.wantQ) { r.qCriterion.resize(N); r.hasQ = true; }
    if (req.wantGradP) { r.gradP.resize(N); r.hasGradP = true; }

    for (int k = 0; k < grid.dZ; ++k)
        for (int j = 0; j < grid.dY; ++j)
            for (int i = 0; i < grid.dX; ++i) {
                const size_t n = grid.index(i, j, k);
                float dudx = 0, dudy = 0, dudz = 0;
                float dvdx = 0, dvdy = 0, dvdz = 0;
                float dwdx = 0, dwdy = 0, dwdz = 0;
                if (needVel) {
                    dudx = axisDerivative(U.data(), grid.x, n, i, grid.dX, sx);
                    dudy = axisDerivative(U.data(), grid.y, n, j, grid.dY, sy);
                    dudz = axisDerivative(U.data(), grid.z, n, k, grid.dZ, sz);
                    dvdx = axisDerivative(V.data(), grid.x, n, i, grid.dX, sx);
                    dvdy = axisDerivative(V.data(), grid.y, n, j, grid.dY, sy);
                    dvdz = axisDerivative(V.data(), grid.z, n, k, grid.dZ, sz);
                    dwdx = axisDerivative(W.data(), grid.x, n, i, grid.dX, sx);
                    dwdy = axisDerivative(W.data(), grid.y, n, j, grid.dY, sy);
                    dwdz = axisDerivative(W.data(), grid.z, n, k, grid.dZ, sz);
                }

                if (req.wantVorticity || req.wantHelicity) {
                    const glm::vec3 om(dwdy - dvdz, dudz - dwdx, dvdx - dudy);
                    if (req.wantVorticity) r.vorticity[n] = om;
                    if (req.wantHelicity) {
                        r.helicity[n] = req.velocity[n].x * om.x
                                      + req.velocity[n].y * om.y
                                      + req.velocity[n].z * om.z;
                    }
                }
                if (req.wantQ) {
                    // S = ½(G+Gᵀ), Ω = ½(G−Gᵀ); Q = ½(‖Ω‖²−‖S‖²).
                    // Diagonal cancels in Ω; off-diagonals pair up.
                    const float s12 = 0.5f * (dudy + dvdx), o12 = 0.5f * (dudy - dvdx);
                    const float s13 = 0.5f * (dudz + dwdx), o13 = 0.5f * (dudz - dwdx);
                    const float s23 = 0.5f * (dvdz + dwdy), o23 = 0.5f * (dvdz - dwdy);
                    const float sNorm2 = dudx*dudx + dvdy*dvdy + dwdz*dwdz
                        + 2.0f * (s12*s12 + s13*s13 + s23*s23);
                    const float oNorm2 = 2.0f * (o12*o12 + o13*o13 + o23*o23);
                    r.qCriterion[n] = 0.5f * (oNorm2 - sNorm2);
                }
                if (req.wantGradP) {
                    r.gradP[n] = glm::vec3(
                        axisDerivative(P, grid.x, n, i, grid.dX, sx),
                        axisDerivative(P, grid.y, n, j, grid.dY, sy),
                        axisDerivative(P, grid.z, n, k, grid.dZ, sz));
                }
            }

    r.ok = true;
    return r;
}

namespace {
bool isCellVectorPointer(const RenderMesh& mesh, const glm::vec3* p){
    if (!p || mesh.cellVectorsData.empty()) return false;
    const glm::vec3* s = mesh.cellVectorsData.data();
    const glm::vec3* e = s + mesh.cellVectorsData.size();
    return p >= s && p < e;
}
bool isCellScalarPointer(const RenderMesh& mesh, const float* p){
    if (!p || !mesh.attributes) return false;
    for (auto &kv: mesh.attributes->cellScalars){
        const float* s = kv.second.data();
        const float* e = s + kv.second.size();
        if (p >= s && p < e) return true;
    }
    return false;
}
std::vector<glm::vec3> extrapolateCellVectorsToPoints(const RenderMesh& mesh, const glm::vec3* cellData, size_t cellCount, size_t pointCount){
    std::vector<glm::vec3> out(pointCount, glm::vec3(0.0f));
    std::vector<int> cnt(pointCount, 0);
    if (mesh.cellConnectivity.empty() || mesh.cellOffsets.empty()){
        // No topology: fallback to nearest cell center via brute (for polydata without cells, just copy first)
        if (cellCount>0){
            for (size_t i=0;i<pointCount;++i) out[i]=cellData[ i % cellCount ];
        }
        return out;
    }
    const auto& conn = mesh.cellConnectivity;
    const auto& off = mesh.cellOffsets;
    uint32_t start=0;
    for(size_t c=0;c<off.size() && c<cellCount; ++c){
        uint32_t end = off[c];
        if (end>conn.size()) { start=end; continue; }
        for(uint32_t a=start;a<end;++a){
            uint32_t pid = conn[a];
            if (pid < pointCount){
                out[pid] += cellData[c];
                cnt[pid]++;
            }
        }
        start=end;
    }
    for(size_t i=0;i<pointCount;++i) if(cnt[i]>0) out[i] /= float(cnt[i]);
    // Isolated points (no incident cell) — keep 0, will be handled as zero gradient
    return out;
}
std::vector<float> extrapolateCellScalarsToPoints(const RenderMesh& mesh, const float* cellData, size_t cellCount, size_t pointCount){
    std::vector<float> out(pointCount, 0.0f);
    std::vector<int> cnt(pointCount, 0);
    if (mesh.cellConnectivity.empty() || mesh.cellOffsets.empty()){
        if (cellCount>0){
            for (size_t i=0;i<pointCount;++i) out[i]=cellData[ i % cellCount ];
        }
        return out;
    }
    const auto& conn = mesh.cellConnectivity;
    const auto& off = mesh.cellOffsets;
    uint32_t start=0;
    for(size_t c=0;c<off.size() && c<cellCount; ++c){
        uint32_t end = off[c];
        if (end>conn.size()) { start=end; continue; }
        for(uint32_t a=start;a<end;++a){
            uint32_t pid = conn[a];
            if (pid < pointCount){
                out[pid] += cellData[c];
                cnt[pid]++;
            }
        }
        start=end;
    }
    for(size_t i=0;i<pointCount;++i) if(cnt[i]>0) out[i] /= float(cnt[i]);
    return out;
}
} // anon

PostFieldsResult computePostFieldsUnstructured(const RenderMesh& mesh,
                                               const PostFieldsRequest& req){
    PostFieldsResult r;
    const bool needVel = req.wantVorticity || req.wantHelicity || req.wantQ;
    const bool needP = req.wantGradP;
    if (needVel && !req.velocity) { r.error="velocity field required"; return r; }
    if (needP && !req.pressure) { r.error="pressure field required"; return r; }
    if (!needVel && !needP) { r.error="nothing requested"; return r; }
    // Logical point count: for split meshes (vertexSourceIndex) the field arrays
    // are sized to sourcePointCount (e.g. 125) not vertices.size()/3 (e.g. 177).
    size_t vertN = mesh.vertices.size()/3;
    if (vertN==0) { r.error="empty mesh"; return r; }
    size_t logicalN = vertN;
    const bool isSplit = !mesh.vertexSourceIndex.empty()
                      && mesh.vertexSourceIndex.size()==vertN
                      && mesh.sourcePointCount>0
                      && (size_t)mesh.sourcePointCount <= vertN;
    if (isSplit) logicalN = (size_t)mesh.sourcePointCount;
    if (req.velocity && mesh.pointVectorCount>0) logicalN = std::min(logicalN, mesh.pointVectorCount);
    const size_t N = logicalN;
    if (N==0) { r.error="empty logical points"; return r; }

    // Handle cell-data fallback: extrapolate cell vectors/scalars to points for unified LSQ
    const glm::vec3* velPtr = req.velocity;
    const float* presPtr = req.pressure;
    std::vector<glm::vec3> velTmp;
    std::vector<float> presTmp;
    bool velIsCell = needVel && isCellVectorPointer(mesh, req.velocity);
    bool presIsCell = needP && isCellScalarPointer(mesh, req.pressure);
    if (velIsCell){
        size_t cellCount = mesh.cellVectorCount ? mesh.cellVectorCount : mesh.cellVectorsData.size();
        velTmp = extrapolateCellVectorsToPoints(mesh, req.velocity, cellCount, N);
        velPtr = velTmp.data();
        // Debug for cell uniform test
        if (N==27){
            for(size_t dbg=0; dbg<std::min<size_t>(N,5); ++dbg){
                // Use std::cerr if needed
            }
        }
    }
    if (presIsCell){
        size_t cellCount = 0;
        for(auto &kv: mesh.attributes->cellScalars){ if(kv.second.data()==req.pressure) { cellCount = kv.second.size(); break; } }
        if (cellCount==0) cellCount = N; // fallback
        presTmp = extrapolateCellScalarsToPoints(mesh, req.pressure, cellCount, N);
        presPtr = presTmp.data();
    }

    gradient::LeastSquaresEstimator lsq(mesh);
    if (lsq.pointCount() != N) {
        if (lsq.pointCount() < N) {
            r.error="LSQ point count mismatch (split mesh dedup failed)";
            return r;
        }
    }
    if (needVel) {
        if (req.wantVorticity) { r.vorticity.assign(N, glm::vec3(0)); r.hasVorticity=true; }
        if (req.wantHelicity) { r.helicity.assign(N, 0.0f); r.hasHelicity=true; }
        if (req.wantQ) { r.qCriterion.assign(N, 0.0f); r.hasQ=true; }
    }
    if (needP) { r.gradP.assign(N, glm::vec3(0)); r.hasGradP=true; }
    for(size_t i=0;i<N;++i){
        gradient::GradTensors gt;
        bool ok=false;
        if (needVel){
            ok = lsq.estimate(i, velPtr, gt, needP?presPtr:nullptr);
        } else {
            glm::vec3 gp; ok = lsq.estimate(i, presPtr, gp); gt.gradP = gp;
        }

        if (!ok) continue;
        if (req.wantVorticity || req.wantHelicity){
            glm::vec3 om(gt.dwdy - gt.dvdz, gt.dudz - gt.dwdx, gt.dvdx - gt.dudy);
            if (req.wantVorticity) r.vorticity[i]=om;
            if (req.wantHelicity){
                const glm::vec3 &v = velPtr[i];
                r.helicity[i]= v.x*om.x + v.y*om.y + v.z*om.z;
            }
        }
        if (req.wantQ){
            float s12=0.5f*(gt.dudy+gt.dvdx), o12=0.5f*(gt.dudy-gt.dvdx);
            float s13=0.5f*(gt.dudz+gt.dwdx), o13=0.5f*(gt.dudz-gt.dwdx);
            float s23=0.5f*(gt.dvdz+gt.dwdy), o23=0.5f*(gt.dvdz-gt.dwdy);
            float sNorm2=gt.dudx*gt.dudx + gt.dvdy*gt.dvdy + gt.dwdz*gt.dwdz + 2*(s12*s12+s13*s13+s23*s23);
            float oNorm2=2*(o12*o12+o13*o13+o23*o23);
            r.qCriterion[i]=0.5f*(oNorm2 - sNorm2);
        }
        if (req.wantGradP){
            r.gradP[i]=gt.gradP;
        }
    }
    r.ok=true;
    return r;
}

PostFieldsResult computePostFields(const RenderMesh& mesh, const PostFieldsRequest& req){
    CartesianGrid grid;
    if (extractCartesianGrid(mesh, grid)){
        const size_t nCount = grid.nodeCount();
        // Validate that the supplied field pointers are sized for the structured grid.
        // For split meshes pointVectorCount == nCount and extractor returns nCount-sized
        // pointer (via thread_local dedup), so structured path is valid. If caller
        // accidentally passed a split-expanded 177-sized buffer (unstructured extraction)
        // then nCount != logical point count and structured compute would read wrong
        // stride — fallback to LSQ.
        bool sizeOk = true;
        // Cell-data fields are size=cellCount, not nCount — must use LSQ path (extrapolated)
        if (req.velocity && isCellVectorPointer(mesh, req.velocity)) sizeOk = false;
        if (req.pressure && isCellScalarPointer(mesh, req.pressure)) sizeOk = false;
        if (sizeOk && req.velocity) {
            if (mesh.pointVectorCount>0 && mesh.pointVectorCount != nCount) {
                const bool isSplit = !mesh.vertexSourceIndex.empty() && mesh.sourcePointCount==(int)nCount;
                if (!isSplit) sizeOk = false;
            }
        }
        if (sizeOk) {
            auto res = computePostFields(grid, req);
            if (res.ok) return res;
        }
    }
    // Fallback LSQ (handles unstructured, polydata, and size-mismatched cartesian)
    return computePostFieldsUnstructured(mesh, req);
}

std::string vorticityName(const std::string& v) { return v + "_vorticity"; }
std::string helicityName(const std::string& v) { return v + "_helicity"; }
std::string qCriterionName(const std::string& v) { return v + "_Q"; }
std::string gradPName(const std::string& p) { return p + "_grad"; }

namespace {

// Range scan kept local: FieldStore::computeRange sits above this layer, and
// post_fields must stay dependency-free so it remains headlessly testable.
void scanRange(const std::vector<float>& v, float& mn, float& mx) {
    mn = 0.0f; mx = 1.0f;
    if (v.empty()) return;
    float lo = std::numeric_limits<float>::max();
    float hi = std::numeric_limits<float>::lowest();
    bool have = false;
    for (float f : v) {
        if (!std::isfinite(f)) continue;
        lo = std::min(lo, f);
        hi = std::max(hi, f);
        have = true;
    }
    if (!have) return;
    mn = lo;
    mx = (std::abs(hi - lo) < 1e-6f) ? lo + 1.0f : hi;
}

} // anonymous

SnapshotResult extractSnapshot(const RenderMesh& mesh, const SnapshotRequest& request) {
    SnapshotResult out;
    if (!request.autoReextract) {
        out.ok = true;
        out.keepExisting = true;
        return out;
    }
    const bool needVel = request.needsVelocity();
    const bool needP = request.needsPressure();

    CartesianGrid grid;
    const bool isCartesian = extractCartesianGrid(mesh, grid);
    PointVectorField vf{};
    PointScalarField pf{};
    bool haveVf = false, havePf = false;
    if (isCartesian) {
        if (needVel) haveVf = extractPointVectors(mesh, grid, request.velocitySource, vf);
        if (needP) havePf = extractPointScalars(mesh, grid, request.pressureSource, pf);
        if (needVel && !haveVf) haveVf = extractPointVectorsUnstructured(mesh, request.velocitySource, vf);
        if (needP && !havePf) havePf = extractPointScalarsUnstructured(mesh, request.pressureSource, pf);
    } else {
        if (needVel) haveVf = extractPointVectorsUnstructured(mesh, request.velocitySource, vf);
        if (needP) havePf = extractPointScalarsUnstructured(mesh, request.pressureSource, pf);
    }

    if ((needVel && !haveVf) || (needP && !havePf)) {
        out.status = "Auto re-extract failed: field not found on frame";
        return out;
    }

    PostFieldsRequest req;
    if (haveVf) req.velocity = vf.data;
    if (havePf) req.pressure = pf.data;
    req.wantVorticity = request.wantVorticity;
    req.wantHelicity = request.wantHelicity;
    req.wantQ = request.wantQ;
    req.wantGradP = request.wantGradP;

    auto res = isCartesian ? computePostFields(grid, req) : computePostFields(mesh, req);
    if (!res.ok && isCartesian) res = computePostFields(mesh, req);
    if (!res.ok) {
        out.status = res.error + " (frame)";
        return out;
    }

    auto hasGarbage = [](const std::vector<float>& v) {
        for (float f : v) if (!std::isfinite(f)) return true;
        return false;
    };
    auto hasGarbageVec = [](const std::vector<glm::vec3>& v) {
        for (const auto& t : v)
            if (!std::isfinite(t.x) || !std::isfinite(t.y) || !std::isfinite(t.z)) return true;
        return false;
    };
    const bool garbage = (res.hasHelicity && hasGarbage(res.helicity))
                      || (res.hasQ && hasGarbage(res.qCriterion))
                      || (res.hasVorticity && hasGarbageVec(res.vorticity))
                      || (res.hasGradP && hasGarbageVec(res.gradP));
    if (garbage) {
        out.status = "Garbage (NaN/Inf) on frame — blanking";
        return out;
    }

    auto snap = std::make_shared<ComputedSnapshot>();
    if (res.hasVorticity) {
        auto v = std::make_shared<const std::vector<glm::vec3>>(std::move(res.vorticity));
        snap->vectors[vorticityName(request.velocitySource)] = v;
    }
    if (res.hasHelicity) {
        auto v = std::make_shared<const std::vector<float>>(std::move(res.helicity));
        float mn = 0.0f, mx = 1.0f;
        scanRange(*v, mn, mx);
        const std::string name = helicityName(request.velocitySource);
        snap->scalars[name] = v;
        snap->scalarRanges[name] = {mn, mx};
    }
    if (res.hasQ) {
        auto v = std::make_shared<const std::vector<float>>(std::move(res.qCriterion));
        float mn = 0.0f, mx = 1.0f;
        scanRange(*v, mn, mx);
        const std::string name = qCriterionName(request.velocitySource);
        snap->scalars[name] = v;
        snap->scalarRanges[name] = {mn, mx};
    }
    if (res.hasGradP) {
        auto v = std::make_shared<const std::vector<glm::vec3>>(std::move(res.gradP));
        snap->vectors[gradPName(request.pressureSource)] = v;
    }
    out.snapshot = snap;
    out.ok = true;
    return out;
}

} // namespace postfields
