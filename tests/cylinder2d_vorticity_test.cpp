// tests/cylinder2d_vorticity_test.cpp
// Empirical probe: real parser + real extraction on the cylinder-shedding 2D
// unstructured QUAD dataset. Prints diagnostics rather than one-shot asserts,
// because the point of this harness is to SEE what the pipeline produces.
#include "core/mesh_loader.h"
#include "core/post_fields.h"
#include <cstdio>
#include <cmath>
#include <algorithm>

static void dumpMeshShape(const RenderMesh& m) {
    printf("datasetType=%s\n", m.datasetType.c_str());
    printf("vertices=%zu indices=%zu normals=%zu\n", m.vertices.size()/3, m.indices.size()/3, m.normals.size()/3);
    printf("gridDim=%dx%dx%d sourcePointCount=%d\n", m.gridDimX, m.gridDimY, m.gridDimZ, m.sourcePointCount);
    printf("pointVectorCount=%zu cellVectorCount=%zu cellCenters=%zu\n",
           m.pointVectorCount, m.cellVectorCount, m.cellCenters.size());
    printf("vertexSourceIndex=%zu cellConnectivity=%zu cellOffsets=%zu\n",
           m.vertexSourceIndex.size(), m.cellConnectivity.size(), m.cellOffsets.size());
    printf("available scalars: "); for (auto& s : m.availableScalarNames) printf("%s; ", s.c_str()); printf("\n");
    printf("available point vectors: "); for (auto& s : m.availableVectorNames) printf("%s; ", s.c_str()); printf("\n");
    printf("available cell vectors: "); for (auto& s : m.availableCellVectorNames) printf("%s; ", s.c_str()); printf("\n");
    float zmin = 1e30f, zmax = -1e30f;
    for (size_t i = 2; i < m.vertices.size(); i += 3) {
        zmin = std::min(zmin, m.vertices[i]); zmax = std::max(zmax, m.vertices[i]);
    }
    printf("z range: %.6g .. %.6g\n", zmin, zmax);
    postfields::CartesianGrid g;
    printf("extractCartesianGrid -> %s\n", postfields::extractCartesianGrid(m, g) ? "true (structured!)" : "false (unstructured LSQ)");
}

static void statLine(const char* tag, std::vector<float>& v) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    printf("%s: max=%.4g p1=%.4g p10=%.4g med=%.4g p90=%.4g p99=%.4g\n",
           tag, v[n-1], v[n/100], v[n/10], v[n/2], v[(n*9)/10], v[(n*99)/100]);
}

// Area-weighted piecewise-linear (Green-Gauss) node gradient over surface
// triangles, in logical-point space.
struct GGImpl {
    std::vector<float> dx, dy, ar;
    GGImpl(size_t N) : dx(N, 0), dy(N, 0), ar(N, 0) {}
    template<typename PosFn, typename SF>
    void accumulate(const RenderMesh& m, const std::vector<int>& logicalOf,
                    PosFn pos, SF sf) {
        for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
            const int a = logicalOf[m.indices[t]], b = logicalOf[m.indices[t+1]], c = logicalOf[m.indices[t+2]];
            if (a < 0 || b < 0 || c < 0) continue;
            const glm::vec2 pa = pos(a), pb = pos(b), pc = pos(c);
            const glm::vec2 d1 = pb - pa, d2 = pc - pa;
            const float twiceA = d1.x*d2.y - d1.y*d2.x;
            if (std::abs(twiceA) < 1e-14f) continue;
            const float A = 0.5f * std::abs(twiceA);
            const float fa = sf(a), fb = sf(b), fc = sf(c);
            const glm::vec2 g = ( (fb - fa) *  glm::vec2(d2.y, -d2.x)
                                + (fc - fa) * -glm::vec2(d1.y, -d1.x) ) / twiceA;
            dx[a] += g.x*A; dy[a] += g.y*A;
            dx[b] += g.x*A; dy[b] += g.y*A;
            dx[c] += g.x*A; dy[c] += g.y*A;
            ar[a] += A; ar[b] += A; ar[c] += A;
        }
    }
};
using GG = GGImpl;

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* path = argc > 1 ? argv[1] : "../samples/cylinder_vortex_shedding_1000.vtu";
    RenderMesh m;
    try { m = loadMeshFile(path); }
    catch (const std::exception& e) { printf("PARSE FAIL: %s\n", e.what()); return 1; }
    dumpMeshShape(m);

    if (m.availableVectorNames.empty() || m.availableScalarNames.empty()) {
        printf("no vector or no scalar data; aborting probe\n");
        return 1;
    }
    const std::string vname = m.availableVectorNames[0];
    const std::string pname = m.availableScalarNames[0];
    size_t vcount = 0;
    const glm::vec3* vel = m.vectorFieldData(vname, vcount);
    if (!vel) { printf("vector data null\n"); return 1; }
    const float* pres = nullptr;
    if (m.attributes) {
        auto it = m.attributes->pointScalars.find(pname);
        if (it != m.attributes->pointScalars.end()) pres = it->second.data();
    }

    // ---- Shared GG machinery: fold split indices to logical, area-weighted PL gradient ----
    const size_t N = vel ? m.pointVectorCount : 0;
    const size_t splitN = m.vertexSourceIndex.size();
    std::vector<int> logicalOf(splitN, -1), repVertexOfLogical(N, -1);
    for (size_t v = 0; v < splitN; ++v) {
        const uint32_t s = v < splitN && m.vertexSourceIndex[v] < N ? m.vertexSourceIndex[v] : UINT32_MAX;
        if (s >= N) continue;
        if (repVertexOfLogical[s] < 0) repVertexOfLogical[s] = (int)v;
        logicalOf[v] = (int)s;
    }
    auto pos = [&](int logical) -> glm::vec2 {
        const int v = repVertexOfLogical[logical];
        return glm::vec2(m.vertices[v*3], m.vertices[v*3+1]);
    };

    postfields::PostFieldsRequest req;
    req.velocity = vel; req.pressure = pres;
    auto r = postfields::computePostFields(m, req);
    printf("computePostFields -> ok=%d err=%s\n", (int)r.ok, r.error.c_str());
    if (!r.ok) return 1;

    // Reference: cell-vector velocity input (what the user reaches if the app
    // resolves the CELL Velocity for vorticity). With dead cellConnectivity the
    // extrapolation degenerates to cellData[i % cellCount] -> striped garbage.
    {
        size_t ccount = 0;
        const glm::vec3* cvel = m.cellVectorFieldData(vname, ccount);
        printf("\ncell-vector branch: count=%zu ptr=%s\n", ccount, cvel ? "ok" : "null");
        if (cvel && pres) {
            postfields::PostFieldsRequest creq;
            creq.velocity = cvel; creq.pressure = pres;
            auto cr = postfields::computePostFields(m, creq);
            printf("cell-vector computePostFields -> ok=%d err=%s N=%zu\n",
                   (int)cr.ok, cr.error.c_str(), cr.vorticity.size());
            if (cr.ok) {
                std::vector<float> cw;
                for (auto& w : cr.vorticity) cw.push_back(w.z);
                statLine("cell-vector wz", cw);
            }
        }
    }

    // ---- GG vorticity vs LSQ vorticity on the real field ----
    {
        GG ggU(N), ggV(N);
        ggU.accumulate(m, logicalOf, pos, [&](int i){ return vel[i].x; });
        ggV.accumulate(m, logicalOf, pos, [&](int i){ return vel[i].y; });
        std::vector<float> lsInt, refInt, diffInt;
        float maxArea = 0;
        for (size_t i = 0; i < N; ++i) maxArea = std::max(maxArea, ggU.ar[i]);
        for (size_t i = 0; i < N; ++i) {
            if (ggU.ar[i] < 0.04f * maxArea) continue; // skip partial-ring boundary nodes
            const float ls = r.vorticity[i].z;
            const float ref = ggV.dx[i]/ggV.ar[i] - ggU.dy[i]/ggU.ar[i];
            lsInt.push_back(ls); refInt.push_back(ref); diffInt.push_back(ls - ref);
        }
        printf("\nLSQ vs GG (interior %zu nodes)\n", diffInt.size());
        statLine("LSQ   wz(int)", lsInt);
        statLine("GG    wz(int)", refInt);
        statLine("diff  wz(int)", diffInt);
        size_t big = 0; float reldiff = 0; size_t abstol = 0;
        float peak = 0;
        for (auto& f : lsInt) peak = std::max(peak, std::abs(f));
        for (auto& f : refInt) peak = std::max(peak, std::abs(f));
        const float tol = 0.02f * peak;
        for (size_t k = 0; k < diffInt.size(); ++k) {
            const float scale = std::max(1e-6f, std::abs(refInt[k]));
            const float rd = std::abs(diffInt[k]) / scale;
            reldiff = std::max(reldiff, rd);
            if (rd > 0.05f) ++big;
            if (std::abs(diffInt[k]) > tol) ++abstol;
        }
        printf("interior: |wz diff|>5%% of |GG|: %zu/%zu; |>2%% peak| (tol=%.3g): %zu\n", big, diffInt.size(), tol, abstol);
        // Location of the worst absolute residuals, via direct recompute per node
        struct Rec { float diff, ls, gg; glm::vec2 p; int id; };
        std::vector<Rec> recs;
        for (size_t i = 0; i < N; ++i) {
            if (ggU.ar[i] < 0.04f * maxArea) continue;
            const float ls = r.vorticity[i].z;
            const float gg = ggV.dx[i]/ggV.ar[i] - ggU.dy[i]/ggU.ar[i];
            recs.push_back({std::abs(ls - gg), ls, gg, pos((int)i), (int)i});
        }
        std::sort(recs.begin(), recs.end(), [](const Rec& a, const Rec& b){ return a.diff > b.diff; });
        for (size_t k = 0; k < 5 && k < recs.size(); ++k) {
            const Rec& rr = recs[k];
            printf("worst#%d node(%d): pos=(%.4f,%.4f) |diff|=%.3g lsq=%.3g gg=%.3g ar=%.4g\n",
                   (int)k+1, rr.id, rr.p.x, rr.p.y, rr.diff, rr.ls, rr.gg, ggU.ar[rr.id]);
        }
        // residual k-t histogram vs peak scale
        size_t d01 = 0, d02 = 0, d05 = 0;
        for (auto& rr : recs) {
            const float a = rr.diff;
            if (a > 0.2f * peak) ++d02;
            if (a > 0.05f * peak) ++d05;
            if (a > 0.01f * peak) ++d01;
        }
        printf("resid: >1%% peak: %zu | >5%% peak: %zu | >20%% peak: %zu (peak=%.3g)\n", d01, d05, d02, peak);
    }

    // ---- Discriminator: LINEAR velocity (u=x, v=y) on the SAME mesh ----
    // Both LSQ and GG must reproduce grad u=(1,0), grad v=(0,1), omega=0.
    {
        std::vector<glm::vec3> lin(N);
        for (size_t i = 0; i < N; ++i) {
            const glm::vec2 p = pos((int)i);
            lin[i] = glm::vec3(p.x, p.y, 0.0f);
        }
        postfields::PostFieldsRequest lreq;
        lreq.velocity = lin.data(); lreq.pressure = pres;
        auto lr = postfields::computePostFields(m, lreq);
        if (lr.ok) {
            float e1 = 0;
            for (auto& w : lr.vorticity) e1 = std::max(e1, std::abs(w.z));
            printf("\nlinear discriminator: LSQ omega_z err=%.3g\n", e1);
            GG ggA(N), ggB(N);
            ggA.accumulate(m, logicalOf, pos, [&](int i){ return lin[i].x; });
            ggB.accumulate(m, logicalOf, pos, [&](int i){ return lin[i].y; });
            float maxa = 0;
            for (size_t i = 0; i < N; ++i) maxa = std::max(maxa, ggA.ar[i]);
            float egx = 0, egy = 0; size_t gbad = 0;
            for (size_t i = 0; i < N; ++i) if (ggA.ar[i] > 0.04f * maxa) {
                const float x1 = std::abs(ggA.dx[i]/ggA.ar[i] - 1.0f);
                const float y1 = std::abs(ggB.dy[i]/ggB.ar[i] - 1.0f);
                if (x1 > 1e-3f || y1 > 1e-3f) ++gbad;
                egx = std::max(egx, x1); egy = std::max(egy, y1);
            }
            printf("linear discriminator: GG  duDx err=%.3g dvDy err=%.3g (bad interior=%zu)\n", egx, egy, gbad);
        }
    }

    return 0;
}
