#include "core/gradient_estimator.h"
#include <cmath>
#include <algorithm>
#include <limits>
#include <unordered_set>

namespace gradient {

namespace {

inline bool solve3x3(const float ATA[3][3], const float ATb[3], glm::vec3& out) {
    // Invert 3x3 via adjugate; scale-relative singularity check for high-performance CFD
    // (extent may be 1e-6..1e6, so absolute 1e-12 is meaningless)
    const float a00=ATA[0][0], a01=ATA[0][1], a02=ATA[0][2];
    const float a11=ATA[1][1], a12=ATA[1][2];
    const float a22=ATA[2][2];
    const float det = a00*(a11*a22 - a12*a12) - a01*(a01*a22 - a12*a02) + a02*(a01*a12 - a11*a02);
    const float scale = std::max({std::abs(a00), std::abs(a11), std::abs(a22), 1e-12f});
    if (std::abs(det) < 1e-12f * scale*scale*scale) return false;
    const float invDet = 1.0f/det;
    const float b00 =  (a11*a22 - a12*a12);
    const float b01 = -(a01*a22 - a12*a02);
    const float b02 =  (a01*a12 - a11*a02);
    const float b11 =  (a00*a22 - a02*a02);
    const float b12 = -(a00*a12 - a01*a02);
    const float b22 =  (a00*a11 - a01*a01);
    out.x = invDet * (b00*ATb[0] + b01*ATb[1] + b02*ATb[2]);
    out.y = invDet * (b01*ATb[0] + b11*ATb[1] + b12*ATb[2]);
    out.z = invDet * (b02*ATb[0] + b12*ATb[1] + b22*ATb[2]);
    return true;
}

inline bool solve2x2(const float ATA2[2][2], const float ATb2[2], float& gx, float& gy) {
    const float det = ATA2[0][0]*ATA2[1][1] - ATA2[0][1]*ATA2[0][1];
    const float scale = std::max({std::abs(ATA2[0][0]), std::abs(ATA2[1][1]), 1e-12f});
    if (std::abs(det) < 1e-12f * scale*scale) return false;
    const float inv = 1.0f/det;
    gx = inv * ( ATA2[1][1]*ATb2[0] - ATA2[0][1]*ATb2[1]);
    gy = inv * (-ATA2[0][1]*ATb2[0] + ATA2[0][0]*ATb2[1]);
    return true;
}

bool solveLeastSquares(const std::vector<glm::vec3>& dX,
                       const std::vector<float>& dF,
                       glm::vec3& grad)
{
    if (dX.size() < 3 || dX.size()!=dF.size()) return false;
    float ATA[3][3] = {};
    float ATb[3] = {};
    for (size_t k=0;k<dX.size();++k){
        const auto &d = dX[k];
        const float b = dF[k];
        const float dist2 = d.x*d.x + d.y*d.y + d.z*d.z + 1e-12f;
        const float w = 1.0f / dist2; // inverse-distance weighting for stretched CFD meshes
        ATA[0][0] += w*d.x*d.x; ATA[0][1] += w*d.x*d.y; ATA[0][2] += w*d.x*d.z;
        ATA[1][1] += w*d.y*d.y; ATA[1][2] += w*d.y*d.z;
        ATA[2][2] += w*d.z*d.z;
        ATb[0] += w*d.x*b; ATb[1] += w*d.y*b; ATb[2] += w*d.z*b;
    }
    ATA[1][0]=ATA[0][1]; ATA[2][0]=ATA[0][2]; ATA[2][1]=ATA[1][2];
    if (solve3x3(ATA, ATb, grad)) return true;
    // Planar fallback for 2D VTI / thin plates: try 2D LSQ on dominant plane
    // Build 2x2 systems for XY, XZ, YZ and pick best conditioned
    {
        const float ATAxy[2][2] = {{ATA[0][0], ATA[0][1]}, {ATA[0][1], ATA[1][1]}};
        const float ATbxy[2] = {ATb[0], ATb[1]};
        float gx, gy;
        if (solve2x2(ATAxy, ATbxy, gx, gy)) { grad = glm::vec3(gx, gy, 0.0f); return true; }
    }
    {
        const float ATAxz[2][2] = {{ATA[0][0], ATA[0][2]}, {ATA[0][2], ATA[2][2]}};
        const float ATbxz[2] = {ATb[0], ATb[2]};
        float gx, gz;
        if (solve2x2(ATAxz, ATbxz, gx, gz)) { grad = glm::vec3(gx, 0.0f, gz); return true; }
    }
    {
        const float ATAyz[2][2] = {{ATA[1][1], ATA[1][2]}, {ATA[1][2], ATA[2][2]}};
        const float ATbyz[2] = {ATb[1], ATb[2]};
        float gy, gz;
        if (solve2x2(ATAyz, ATbyz, gy, gz)) { grad = glm::vec3(0.0f, gy, gz); return true; }
    }
    // Ridge regularization as last resort (thin sliver)
    {
        float scale = std::max({std::abs(ATA[0][0]), std::abs(ATA[1][1]), std::abs(ATA[2][2]), 1e-12f});
        float eps = 1e-6f * scale;
        ATA[0][0] += eps; ATA[1][1] += eps; ATA[2][2] += eps;
        if (solve3x3(ATA, ATb, grad)) return true;
    }
    return false;
}

} // anonymous (internal helper duplicated as static for class)

bool LeastSquaresEstimator::solveLeastSquares(const std::vector<glm::vec3>& dX,
                                              const std::vector<float>& dF,
                                              glm::vec3& grad)
{
    return ::gradient::solveLeastSquares(dX,dF,grad);
}

LeastSquaresEstimator::LeastSquaresEstimator(const RenderMesh& mesh): m_mesh(&mesh) {
    // For split meshes (post-normal split, 177 verts vs 125 logical points) the
    // LSQ field arrays are sized to sourcePointCount. Use logical count to avoid
    // zero-distance duplicate neighbors that make ATA singular and cause seam artifacts.
    const size_t vertN = mesh.vertices.size()/3;
    const bool isSplit = !mesh.vertexSourceIndex.empty()
                      && mesh.vertexSourceIndex.size()==vertN
                      && mesh.sourcePointCount>0
                      && (size_t)mesh.sourcePointCount <= vertN;
    if (isSplit) {
        m_pointCount = (size_t)mesh.sourcePointCount;
        // Build deduplicated logical positions for LSQ (first occurrence per source id)
        m_logicalVerts.resize(m_pointCount*3);
        std::vector<char> filled(m_pointCount, 0);
        for (size_t v=0; v<vertN; ++v) {
            uint32_t src = mesh.vertexSourceIndex[v];
            if (src < m_pointCount && !filled[src]) {
                m_logicalVerts[src*3+0] = mesh.vertices[v*3+0];
                m_logicalVerts[src*3+1] = mesh.vertices[v*3+1];
                m_logicalVerts[src*3+2] = mesh.vertices[v*3+2];
                filled[src]=1;
            }
        }
        // Fallback: if some ids never filled (should not happen), copy from front
        for (size_t i=0;i<m_pointCount;++i) if(!filled[i]) {
            m_logicalVerts[i*3+0]=mesh.vertices[0];
            m_logicalVerts[i*3+1]=mesh.vertices[1];
            m_logicalVerts[i*3+2]=mesh.vertices[2];
        }
    } else {
        m_pointCount = vertN;
    }
    if (m_pointCount==0) return;
    // Structured grids (ImageData/RectilinearGrid/STRUCTURED_GRID incl. curvilinear)
    // Use i,j,k 6-connectivity when dims match logical count — works for curvilinear via actual xyz.
    if (buildNeighborsStructured()) return;
    if (mesh.hasUnstructuredTopology()) {
        if (buildNeighborsFromTopology()) return;
    }
    if (!mesh.indices.empty()) {
        if (buildNeighborsFromIndices()) return;
    }
    buildNeighborsFromRadius();
}

bool LeastSquaresEstimator::buildNeighborsStructured(){
    int dX=m_mesh->gridDimX, dY=m_mesh->gridDimY, dZ=m_mesh->gridDimZ;
    // Need at least 2D structured (two dims >1). For 3D, all three.
    int dimsGT1 = (dX>1)+(dY>1)+(dZ>1);
    if (dimsGT1 < 2) return false;
    size_t expected = (size_t)dX*dY*dZ;
    if (expected != m_pointCount && (size_t)m_mesh->sourcePointCount != m_pointCount) {
        // For split meshes vertices may be duplicated; use sourcePointCount as true N
        if ((size_t)dX*dY*dZ != (size_t)m_mesh->sourcePointCount) return false;
        // For split case we cannot map i,j,k to vertices index directly; fall back to other methods
        return false;
    }
    if (expected != m_pointCount) return false;
    m_neighbors.assign(m_pointCount, {});
    auto idx=[&](int x,int y,int z){ return x + y*dX + z*dX*dY; };
    for(int z=0;z<dZ;++z) for(int y=0;y<dY;++y) for(int x=0;x<dX;++x){
        int n=idx(x,y,z);
        if(n<0|| (size_t)n>=m_pointCount) continue;
        if(x>0) m_neighbors[n].push_back(idx(x-1,y,z));
        if(x+1<dX) m_neighbors[n].push_back(idx(x+1,y,z));
        if(y>0) m_neighbors[n].push_back(idx(x,y-1,z));
        if(y+1<dY) m_neighbors[n].push_back(idx(x,y+1,z));
        if(z>0) m_neighbors[n].push_back(idx(x,y,z-1));
        if(z+1<dZ) m_neighbors[n].push_back(idx(x,y,z+1));
    }
    return true;
}

bool LeastSquaresEstimator::buildNeighborsFromTopology(){
    const auto& conn = m_mesh->cellConnectivity;
    const auto& off  = m_mesh->cellOffsets;
    if (conn.empty()||off.empty()) return false;
    m_neighbors.assign(m_pointCount, {});
    // For each cell, connect its points fully (clique)
    uint32_t start=0;
    for (size_t c=0;c<off.size();++c){
        uint32_t end = off[c];
        if (end>conn.size()) return false;
        for (uint32_t a=start;a<end;++a){
            uint32_t pa = conn[a];
            if (pa>=m_pointCount) continue;
            for (uint32_t b=start;b<end;++b){
                uint32_t pb = conn[b];
                if (pb>=m_pointCount||pb==pa) continue;
                m_neighbors[pa].push_back(pb);
            }
        }
        start=end;
    }
    // dedup + cap
    for (auto &nb: m_neighbors){
        std::sort(nb.begin(), nb.end());
        nb.erase(std::unique(nb.begin(), nb.end()), nb.end());
        if (nb.size()>32) nb.resize(32);
    }
    return true;
}

bool LeastSquaresEstimator::buildNeighborsFromIndices(){
    const auto& idx = m_mesh->indices;
    if (idx.size()<3) return false;
    m_neighbors.assign(m_pointCount, {});
    // Split meshes: indices are in split-vertex space (0..vertN-1) while the
    // LSQ field arrays are in logical point space (0..sourcePointCount-1).
    // Fold each index through vertexSourceIndex so the neighbor ring is the
    // TRUE mesh one-ring; without this the ring is incomplete (skipped tri
    // entries) and the estimator silently degrades to the radius-ball
    // heuristic, which is badly inaccurate on stretched boundary-layer meshes.
    const uint32_t* vmap = nullptr;
    if (!m_mesh->vertexSourceIndex.empty()
        && m_mesh->vertexSourceIndex.size() == m_mesh->vertices.size()/3
        && m_pointCount <= m_mesh->vertexSourceIndex.size()) {
        vmap = m_mesh->vertexSourceIndex.data();
    }
    for (size_t t=0; t+2<idx.size(); t+=3){
        uint32_t a=idx[t], b=idx[t+1], c=idx[t+2];
        if (vmap) {
            if (a>=m_mesh->vertexSourceIndex.size()||b>=m_mesh->vertexSourceIndex.size()||c>=m_mesh->vertexSourceIndex.size()) continue;
            a=vmap[a]; b=vmap[b]; c=vmap[c];
        }
        if (a>=m_pointCount||b>=m_pointCount||c>=m_pointCount) continue;
        m_neighbors[a].push_back(b); m_neighbors[a].push_back(c);
        m_neighbors[b].push_back(a); m_neighbors[b].push_back(c);
        m_neighbors[c].push_back(a); m_neighbors[c].push_back(b);
    }
    // expand to 2-ring for stability (one-ring may be only 6 for regular tri mesh)
    // simple: for each point, add neighbors-of-neighbors (capped)
    std::vector<std::vector<uint32_t>> expanded = m_neighbors;
    for (size_t i=0;i<m_pointCount;++i){
        std::unordered_set<uint32_t> s(expanded[i].begin(), expanded[i].end());
        for (uint32_t nb: m_neighbors[i]){
            if (nb>=m_neighbors.size()) continue;
            for (uint32_t nn: m_neighbors[nb]){
                if (nn!=i) s.insert(nn);
            }
        }
        expanded[i].assign(s.begin(), s.end());
        if (expanded[i].size()>24) expanded[i].resize(24);
    }
    m_neighbors.swap(expanded);
    return true;
}

bool LeastSquaresEstimator::buildNeighborsFromRadius(){
    // Adaptive radius for high-performance CFD: 3.5× avg spacing, clamped.
    // Works for 2D VTI (planar) and dense 12M unstructured via bucket.
    m_neighbors.assign(m_pointCount, {});
    const float* verts = vertData();
    const float extent = std::abs((float)m_mesh->bounds.extent);
    float avgSpacing = 1.0f;
    if (extent > 1e-6f && m_pointCount > 8) {
        // Detect planar 2D (depth/height/width collapse) → use sqrt, else cbrt
        const double dx = m_mesh->bounds.width(), dy = m_mesh->bounds.height(), dz = m_mesh->bounds.depth();
        const double minDim = std::min({dx, dy, dz});
        const bool planar = minDim < 1e-6 * extent;
        if (planar) avgSpacing = (float)(extent / std::sqrt((double)m_pointCount));
        else avgSpacing = (float)(extent / std::cbrt((double)m_pointCount));
    } else if (extent > 1e-6f) {
        avgSpacing = extent * 0.08f;
    }
    float radius = avgSpacing * 3.5f;
    if (extent > 1e-6f) radius = std::clamp(radius, extent*0.01f, extent*0.15f);
    else radius = 1.0f;
    const float r2 = radius*radius;
    // Build quantized grid to avoid O(N²) for large meshes: map posKey -> list
    std::unordered_map<uint64_t, std::vector<uint32_t>> buckets;
    buckets.reserve(m_pointCount*2);
    auto qKey = [&](float x,float y,float z)->uint64_t{
        int64_t qx = (int64_t)std::llround(x*4.0f);
        int64_t qy = (int64_t)std::llround(y*4.0f);
        int64_t qz = (int64_t)std::llround(z*4.0f);
        return ((uint64_t)(qx+ 1LL<<33)<<42) ^ ((uint64_t)(qy+ 1LL<<33)<<21) ^ (uint64_t)(qz+ 1LL<<33);
    };
    for (size_t i=0;i<m_pointCount;++i){
        buckets[qKey(verts[i*3], verts[i*3+1], verts[i*3+2])].push_back((uint32_t)i);
    }
    // For each point, collect neighbors within radius from own + adjacent buckets (expanded brute)
    for (size_t i=0;i<m_pointCount;++i){
        float xi=verts[i*3], yi=verts[i*3+1], zi=verts[i*3+2];
        // brute scan limited buckets: just check all points within radius² by scanning points with nearby bucket key not efficient,
        // fallback to brute over all points when N < 8000
        if (m_pointCount < 8000){
            for (size_t j=0;j<m_pointCount;++j) if(j!=i){
                float dx=verts[j*3]-xi, dy=verts[j*3+1]-yi, dz=verts[j*3+2]-zi;
                if (dx*dx+dy*dy+dz*dz < r2) m_neighbors[i].push_back((uint32_t)j);
                if (m_neighbors[i].size()>=24) break;
            }
        } else {
            // bucket check 27 neighboring quantized cells
            int64_t qx=(int64_t)std::llround(xi*4.0f), qy=(int64_t)std::llround(yi*4.0f), qz=(int64_t)std::llround(zi*4.0f);
            for(int dz=-1;dz<=1;++dz) for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx){
                auto it=buckets.find(qKey((qx+dx)/4.0f,(qy+dy)/4.0f,(qz+dz)/4.0f));
                if(it==buckets.end()) continue;
                for(uint32_t j: it->second){ if(j==i) continue; float ddx=verts[j*3]-xi,ddy=verts[j*3+1]-yi,ddz=verts[j*3+2]-zi; if(ddx*ddx+ddy*ddy+ddz*ddz < r2) m_neighbors[i].push_back(j); if(m_neighbors[i].size()>=24) break; } if(m_neighbors[i].size()>=24) break;
            }
        }
        if (m_neighbors[i].empty()){
            // ensure at least 4 nearest brute
            float best[4]={1e30f,1e30f,1e30f,1e30f}; int bestIdx[4]={-1,-1,-1,-1};
            for(size_t j=0;j<m_pointCount;++j) if(j!=i){
                float dx=verts[j*3]-xi, dy=verts[j*3+1]-yi, dz=verts[j*3+2]-zi; float d2=dx*dx+dy*dy+dz*dz;
                for(int k=0;k<4;++k) if(d2<best[k]){ for(int t=3;t>k;--t){best[t]=best[t-1]; bestIdx[t]=bestIdx[t-1];} best[k]=d2; bestIdx[k]=(int)j; break; }
            }
            for(int k=0;k<4;++k) if(bestIdx[k]>=0) m_neighbors[i].push_back((uint32_t)bestIdx[k]);
        }
    }
    return true;
}

bool LeastSquaresEstimator::estimate(size_t pid, const float* scalar, glm::vec3& gradOut) const {
    if (pid>=m_pointCount||!scalar||pid>=m_neighbors.size()) return false;
    const auto& nb = m_neighbors[pid];
    if (nb.size()<3) return false;
    const float* verts = vertData();
    glm::vec3 xi(verts[pid*3], verts[pid*3+1], verts[pid*3+2]);
    float pi = scalar[pid];
    std::vector<glm::vec3> dX; dX.reserve(nb.size());
    std::vector<float> dF; dF.reserve(nb.size());
    for(uint32_t j: nb){
        if (j>=m_pointCount) continue;
        glm::vec3 xj(verts[j*3], verts[j*3+1], verts[j*3+2]);
        dX.push_back(xj - xi);
        dF.push_back(scalar[j]-pi);
    }
    return solveLeastSquares(dX,dF,gradOut);
}

bool LeastSquaresEstimator::estimate(size_t pid, const glm::vec3* vec, GradTensors& g, const float* pressure) const {
    if (pid>=m_pointCount||!vec) return false;
    const auto& nb = m_neighbors[pid];
    if (nb.size()<3) return false;
    const float* verts = vertData();
    glm::vec3 xi(verts[pid*3], verts[pid*3+1], verts[pid*3+2]);
    std::vector<glm::vec3> dX; dX.reserve(nb.size());
    for(uint32_t j: nb){
        glm::vec3 xj(verts[j*3], verts[j*3+1], verts[j*3+2]);
        dX.push_back(xj - xi);
    }
    // Solve per component
    std::vector<float> dU,dV,dW;
    dU.reserve(nb.size()); dV.reserve(nb.size()); dW.reserve(nb.size());
    for(uint32_t j: nb){ dU.push_back(vec[j].x - vec[pid].x); dV.push_back(vec[j].y - vec[pid].y); dW.push_back(vec[j].z - vec[pid].z); }
    glm::vec3 gu,gv,gw;
    if (!solveLeastSquares(dX,dU,gu)) return false;
    if (!solveLeastSquares(dX,dV,gv)) return false;
    if (!solveLeastSquares(dX,dW,gw)) return false;
    g.dudx=gu.x; g.dudy=gu.y; g.dudz=gu.z;
    g.dvdx=gv.x; g.dvdy=gv.y; g.dvdz=gv.z;
    g.dwdx=gw.x; g.dwdy=gw.y; g.dwdz=gw.z;
    if (pressure){
        std::vector<float> dP; dP.reserve(nb.size());
        for(uint32_t j: nb) dP.push_back(pressure[j]-pressure[pid]);
        glm::vec3 gp;
        if (solveLeastSquares(dX,dP,gp)) g.gradP=gp;
    }
    return true;
}

void LeastSquaresEstimator::estimateAll(const glm::vec3* velocity, const float* pressure,
                                        std::vector<GradTensors>& out) const {
    out.assign(m_pointCount, GradTensors{});
    for(size_t i=0;i<m_pointCount;++i){
        GradTensors gt;
        if (velocity) estimate(i, velocity, gt, pressure);
        else if (pressure) { glm::vec3 gp; if(estimate(i, pressure, gp)) gt.gradP=gp; }
        out[i]=gt;
    }
}

} // namespace gradient
