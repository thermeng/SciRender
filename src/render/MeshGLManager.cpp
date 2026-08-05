#include "render/MeshGLManager.h"
#include "render/render_config.h"
#include "core/mesh_loader.h"

#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <algorithm>
#include <string>

namespace {
// Format gate for LOD (decimation). We now allow the regular volumetric grid
// dataset types AND surface datasets. Surface meshes (STL) and triangulated VTK
// surface datasets (UNSTRUCTURED_GRID, POLYDATA) are eligible for LOD, but they
// are additionally screened by surfaceDecimationSafe() before a decimated mesh
// is actually built, so disconnected/overlapping shells are never clustered.
bool datasetSupportsDecimation(const RenderMesh& in) {
    const std::string& t = in.datasetType;
    return t == "STRUCTURED_GRID" ||
           t == "STRUCTURED_POINTS" ||
           t == "RECTILINEAR_GRID" ||
           t == "STL" ||
           t == "UNSTRUCTURED_GRID" ||
           t == "POLYDATA";
}

// Vertex-clustering decimation averages every vertex inside a coarse spatial
// cell into a single point. For a mesh with multiple disconnected shells that
// fall into the same cell, this transiently merges unrelated parts into
// "spurious new surfaces" while the camera moves. This cheap one-time
// topological test decides whether clustering is safe for a given surface mesh:
//  - a single connected component is always safe;
//  - multiple components are safe only if none are within one cell of each other
//    (so clustering cannot merge them);
//  - meshes with more than kMaxComponents components are treated as unsafe to
//    bound the O(components^2) overlap test (these are exactly the risky ones).
bool surfaceDecimationSafe(const RenderMesh& in) {
    const size_t nv = in.vertices.size() / 3;
    const size_t nt = in.indices.size() / 3;
    if (nv < 3 || nt < 1) return false;

    // Union-find over vertex indices, with per-component tight bounding boxes.
    struct UF {
        std::vector<int> parent;
        int find(int x) {
            while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
            return x;
        }
        void unite(int a, int b) {
            int ra = find(a), rb = find(b);
            if (ra != rb) parent[ra] = rb;
        }
    };
    UF uf;
    uf.parent.resize(nv);
    for (size_t i = 0; i < nv; ++i) uf.parent[i] = static_cast<int>(i);

    // vertex -> triangle CSR so we can flood triangles that share a vertex.
    std::vector<int> triHead(nv, -1), triNext(nt * 3);
    for (size_t t = 0; t < nt; ++t) {
        for (int k = 0; k < 3; ++k) {
            unsigned int v = in.indices[t * 3 + k];
            if (v >= nv) return false; // malformed; be conservative
            triNext[t * 3 + k] = triHead[v];
            triHead[v] = static_cast<int>(t * 3 + k);
        }
    }

    // Flood-fill connected triangles (sharing a vertex) into components, and
    // accumulate a per-component bounding box over the triangle vertices.
    const int kMaxComponents = 64;
    struct Comp {
        float minX, minY, minZ, maxX, maxY, maxZ;
    };
    std::vector<Comp> comps;
    std::vector<int> triComp(nt, -1);
    std::vector<int> stack;

    for (size_t s = 0; s < nt; ++s) {
        if (triComp[s] != -1) continue;
        const int root = uf.find(in.indices[s * 3]);
        comps.push_back({1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f});
        Comp& c = comps.back();
        stack.clear();
        stack.push_back(static_cast<int>(s));
        triComp[s] = static_cast<int>(comps.size() - 1);
        while (!stack.empty()) {
            const int t = stack.back(); stack.pop_back();
            for (int k = 0; k < 3; ++k) {
                unsigned int v = in.indices[t * 3 + k];
                const float x = in.vertices[3 * v + 0];
                const float y = in.vertices[3 * v + 1];
                const float z = in.vertices[3 * v + 2];
                if (x < c.minX) c.minX = x; if (x > c.maxX) c.maxX = x;
                if (y < c.minY) c.minY = y; if (y > c.maxY) c.maxY = y;
                if (z < c.minZ) c.minZ = z; if (z > c.maxZ) c.maxZ = z;
                // push unvisited triangles adjacent through this vertex
                for (int e = triHead[v]; e != -1; e = triNext[e]) {
                    const int nt2 = e / 3;
                    if (triComp[nt2] == -1) {
                        triComp[nt2] = static_cast<int>(comps.size() - 1);
                        stack.push_back(nt2);
                    }
                }
            }
        }
        // Ensure the whole vertex-connected set is one union-find group so the
        // component we just flooded is internally consistent.
        (void)root;
    }

    if (comps.size() <= 1) return true;
    if (static_cast<int>(comps.size()) > kMaxComponents) return false;

    // Compute the clustering cell size the same way decimate() will.
    const double dx = in.bounds.maxX - in.bounds.minX;
    const double dy = in.bounds.maxY - in.bounds.minY;
    const double dz = in.bounds.maxZ - in.bounds.minZ;
    const double diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (diag < 1e-9) return false;
    int cellsPerAxis = static_cast<int>(std::round(std::pow((double)nv, 1.0 / 3.0) * RenderConfig::defaults().lodDecimateRatio));
    cellsPerAxis = std::max(2, std::min(cellsPerAxis, 512));
    const double cell = diag / cellsPerAxis;

    // Any pair of components whose boxes come within `cell` on all three axes
    // could share a cluster cell => clustering could merge unrelated shells.
    for (size_t a = 0; a + 1 < comps.size(); ++a) {
        for (size_t b = a + 1; b < comps.size(); ++b) {
            const Comp& ca = comps[a];
            const Comp& cb = comps[b];
            const bool closeX = (ca.maxX - cb.minX) < cell && (cb.maxX - ca.minX) < cell;
            const bool closeY = (ca.maxY - cb.minY) < cell && (cb.maxY - ca.minY) < cell;
            const bool closeZ = (ca.maxZ - cb.minZ) < cell && (cb.maxZ - ca.minZ) < cell;
            if (closeX && closeY && closeZ) return false;
        }
    }
    return true;
}
} // namespace

void MeshGLManager::buildMeshGL(const RenderMesh& renderMesh, std::vector<Mesh>& out) {
    Mesh mesh;
    mesh.indexCount = static_cast<int>(renderMesh.indices.size());
    mesh.vertexCount = static_cast<int>(renderMesh.vertices.size() / 3);

    glCreateVertexArrays(1, mesh.vao.ptr());
    glCreateBuffers(1, mesh.vbo.ptr());
    glCreateBuffers(1, mesh.nbo.ptr());
    glCreateBuffers(1, mesh.ebo.ptr());

    glEnableVertexArrayAttrib(mesh.vao, 0);
    glVertexArrayAttribFormat(mesh.vao, 0, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(mesh.vao, 0, 0);
    glNamedBufferData(mesh.vbo, renderMesh.vertices.size() * sizeof(float), renderMesh.vertices.data(), GL_STATIC_DRAW);
    glVertexArrayVertexBuffer(mesh.vao, 0, mesh.vbo, 0, 3 * sizeof(float));

    glEnableVertexArrayAttrib(mesh.vao, 1);
    glVertexArrayAttribFormat(mesh.vao, 1, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(mesh.vao, 1, 1);

    const size_t vertCount = renderMesh.vertices.size() / 3;
    if (renderMesh.normals.size() == renderMesh.vertices.size() && !renderMesh.normals.empty()) {
        glNamedBufferData(mesh.nbo, renderMesh.normals.size() * sizeof(float), renderMesh.normals.data(), GL_STATIC_DRAW);
    } else {
        std::vector<float> fallback(vertCount * 3, 0.0f);
        for (size_t i = 0; i < vertCount; ++i) fallback[i * 3 + 2] = 1.0f;
        glNamedBufferData(mesh.nbo, fallback.size() * sizeof(float), fallback.data(), GL_STATIC_DRAW);
    }
    glVertexArrayVertexBuffer(mesh.vao, 1, mesh.nbo, 0, 3 * sizeof(float));

    if (!renderMesh.scalars.empty()) {
        glCreateBuffers(1, mesh.sbo.ptr());
        glEnableVertexArrayAttrib(mesh.vao, 2);
        glVertexArrayAttribFormat(mesh.vao, 2, 1, GL_FLOAT, GL_FALSE, 0);
        glVertexArrayAttribBinding(mesh.vao, 2, 2);
        glVertexArrayVertexBuffer(mesh.vao, 2, mesh.sbo, 0, sizeof(float));
        glNamedBufferData(mesh.sbo, renderMesh.scalars.size() * sizeof(float), renderMesh.scalars.data(), GL_STATIC_DRAW);
    } else {
        mesh.sbo.reset();
    }

    glNamedBufferData(mesh.ebo, renderMesh.indices.size() * sizeof(unsigned int), renderMesh.indices.data(), GL_STATIC_DRAW);
    glVertexArrayElementBuffer(mesh.vao, mesh.ebo);

    // ponytail: per-cell boundary edges (cellEdges) -> dedicated line VBO
    if (!renderMesh.cellEdges.empty()) {
        glCreateVertexArrays(1, mesh.lineVao.ptr());
        glCreateBuffers(1, mesh.lineVbo.ptr());
        glEnableVertexArrayAttrib(mesh.lineVao, 0);
        glVertexArrayAttribFormat(mesh.lineVao, 0, 3, GL_FLOAT, GL_FALSE, 0);
        glVertexArrayAttribBinding(mesh.lineVao, 0, 0);
        glNamedBufferData(mesh.lineVbo, renderMesh.cellEdges.size() * sizeof(float), renderMesh.cellEdges.data(), GL_STATIC_DRAW);
        glVertexArrayVertexBuffer(mesh.lineVao, 0, mesh.lineVbo, 0, 0);
        mesh.lineCount = static_cast<int>(renderMesh.cellEdges.size() / 3);
    }

    out.push_back(std::move(mesh));
}

RenderMesh MeshGLManager::decimate(const RenderMesh& in) {
    RenderMesh out;
    const size_t nv = in.vertices.size() / 3;
    // Small meshes gain nothing from LOD and risk degeneracy — skip them.
    if (nv < RenderConfig::defaults().lodMinVertices || in.indices.size() < 3) return out;

    const double minX = in.bounds.minX, minY = in.bounds.minY, minZ = in.bounds.minZ;
    const double dx = in.bounds.maxX - minX, dy = in.bounds.maxY - minY, dz = in.bounds.maxZ - minZ;
    const double diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (diag < 1e-9) return out;

    // Cells per axis chosen so the cluster count is a coarse fraction of the
    // vertices (~half the "one cell per vertex" resolution => ~1/8th vertices).
    int cellsPerAxis = static_cast<int>(std::round(std::pow((double)nv, 1.0 / 3.0) * RenderConfig::defaults().lodDecimateRatio));
    cellsPerAxis = std::max(2, std::min(cellsPerAxis, 512));
    const double cell = diag / cellsPerAxis;

    auto clampCell = [&](double v, int n) {
        int i = static_cast<int>(std::floor(v / cell));
        if (i < 0) i = 0; else if (i >= n) i = n - 1;
        return i;
    };
    auto keyFor = [&](size_t i) -> uint64_t {
        const int ci = clampCell(in.vertices[3 * i + 0] - minX, cellsPerAxis);
        const int cj = clampCell(in.vertices[3 * i + 1] - minY, cellsPerAxis);
        const int ck = clampCell(in.vertices[3 * i + 2] - minZ, cellsPerAxis);
        return static_cast<uint64_t>(ci)
             | (static_cast<uint64_t>(cj) << 20)
             | (static_cast<uint64_t>(ck) << 40);
    };

    std::unordered_map<uint64_t, int> cellToNew;
    cellToNew.reserve(static_cast<size_t>(nv) + 1); // one cluster per vertex worst-case; avoid rehashing
    std::vector<int> remap(nv, -1);
    std::vector<double> sx, sy, sz, nx, ny, nz, sc, cnt;
    const bool hasS = !in.scalars.empty();
    const bool hasN = !in.normals.empty();

    for (size_t i = 0; i < nv; ++i) {
        const uint64_t k = keyFor(i);
        auto it = cellToNew.find(k);
        int newIdx;
        if (it == cellToNew.end()) {
            newIdx = static_cast<int>(cellToNew.size());
            cellToNew[k] = newIdx;
            sx.push_back(0.0); sy.push_back(0.0); sz.push_back(0.0);
            nx.push_back(0.0); ny.push_back(0.0); nz.push_back(0.0);
            sc.push_back(0.0); cnt.push_back(0.0);
        } else {
            newIdx = it->second;
        }
        remap[i] = newIdx;
        sx[newIdx] += in.vertices[3 * i + 0];
        sy[newIdx] += in.vertices[3 * i + 1];
        sz[newIdx] += in.vertices[3 * i + 2];
        if (hasN) {
            nx[newIdx] += in.normals[3 * i + 0];
            ny[newIdx] += in.normals[3 * i + 1];
            nz[newIdx] += in.normals[3 * i + 2];
        }
        if (hasS) sc[newIdx] += in.scalars[i];
        cnt[newIdx] += 1.0;
    }

    const int newCount = static_cast<int>(cellToNew.size());
    out.vertices.resize(static_cast<size_t>(newCount) * 3);
    out.normals.resize(static_cast<size_t>(newCount) * 3);
    if (hasS) out.scalars.resize(newCount);

    for (int i = 0; i < newCount; ++i) {
        const double inv = 1.0 / cnt[i];
        out.vertices[3 * i + 0] = static_cast<float>(sx[i] * inv);
        out.vertices[3 * i + 1] = static_cast<float>(sy[i] * inv);
        out.vertices[3 * i + 2] = static_cast<float>(sz[i] * inv);
        double nl = std::sqrt(nx[i] * nx[i] + ny[i] * ny[i] + nz[i] * nz[i]);
        if (nl > 1e-12) { nl = 1.0 / nl; } else { nl = 0.0; }
        out.normals[3 * i + 0] = static_cast<float>(nx[i] * nl);
        out.normals[3 * i + 1] = static_cast<float>(ny[i] * nl);
        out.normals[3 * i + 2] = static_cast<float>(nz[i] * nl);
        if (hasS) out.scalars[i] = static_cast<float>(sc[i] * inv);
    }

    // Remap triangles, dropping any that collapsed into a single cell.
    out.indices.reserve(in.indices.size());
    for (size_t t = 0; t + 2 < in.indices.size(); t += 3) {
        const int a = remap[in.indices[t]];
        const int b = remap[in.indices[t + 1]];
        const int c = remap[in.indices[t + 2]];
        if (a == b || b == c || a == c) continue;
        out.indices.push_back(a);
        out.indices.push_back(b);
        out.indices.push_back(c);
    }

    out.bounds = in.bounds; // vertices stay within the same box
    return out;
}

void MeshGLManager::upload(std::shared_ptr<const RenderMesh> renderMesh) {
    std::vector<Mesh> newFull, newDec;
    if (!renderMesh) return;

    // Cache the full-resolution source so a later scalar-only field switch can
    // re-derive the decimated LOD scalars without re-uploading geometry.
    // Stored as the shared_ptr (NO copy) — the single heavy CPU payload.
    fullSource_ = std::move(renderMesh);
    hasFullSource_ = true;

    // Full-resolution mesh.
    buildMeshGL(*fullSource_, newFull);

    // LOD: a coarsely decimated mesh, used only while the camera is moving.
    // Eligible surface/STL/VTK meshes are additionally screened by
    // surfaceDecimationSafe() so disconnected/overlapping shells are never
    // clustered into spurious surfaces during camera motion.
    if (datasetSupportsDecimation(*fullSource_) && surfaceDecimationSafe(*fullSource_)) {
        RenderMesh decimated = decimate(*fullSource_);
        bool lodWorthwhile = !decimated.indices.empty() &&
                             decimated.indices.size() < fullSource_->indices.size() / 2;
        if (lodWorthwhile) buildMeshGL(decimated, newDec);
    }

    // WIPE OUT OLD OPENGL HANDLES BEFORE GENERATING NEW ONES
    // Guarded by the mutex so it cannot race with clear() on the UI thread.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        meshes_.clear();
        decimatedMeshes_.clear();

        meshes_ = std::move(newFull);
        decimatedMeshes_ = std::move(newDec);
        hasDecimated_ = !decimatedMeshes_.empty();
    }

    meshChanged = true;
}

void MeshGLManager::clear() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        meshes_.clear();
        decimatedMeshes_.clear();
        hasDecimated_ = false;
        hasFullSource_ = false;
        fullSource_.reset();
    }
}

std::vector<float> MeshGLManager::decimateScalars(
    const std::vector<float>& fullScalars) const {
    if (!hasFullSource_ || !hasDecimated_ || decimatedMeshes_.empty())
        return {};
    const RenderMesh& in = *fullSource_;
    const size_t nv = in.vertices.size() / 3;
    // The LOD mesh exists only when the geometry was decimated; the decimated
    // vertex count is carried by the (single) decimated Mesh's index setup.
    // Reuse the exact clustering from decimate() to average the full scalars
    // into the same coarse vertices.
    const double minX = in.bounds.minX, minY = in.bounds.minY, minZ = in.bounds.minZ;
    const double dx = in.bounds.maxX - minX, dy = in.bounds.maxY - minY, dz = in.bounds.maxZ - minZ;
    const double diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (diag < 1e-9) return {};
    int cellsPerAxis = static_cast<int>(std::round(std::pow((double)nv, 1.0 / 3.0) * RenderConfig::defaults().lodDecimateRatio));
    cellsPerAxis = std::max(2, std::min(cellsPerAxis, 512));
    const double cell = diag / cellsPerAxis;

    auto clampCell = [&](double v, int n) {
        int i = static_cast<int>(std::floor(v / cell));
        if (i < 0) i = 0; else if (i >= n) i = n - 1;
        return i;
    };
    auto keyFor = [&](size_t i) -> uint64_t {
        const int ci = clampCell(in.vertices[3 * i + 0] - minX, cellsPerAxis);
        const int cj = clampCell(in.vertices[3 * i + 1] - minY, cellsPerAxis);
        const int ck = clampCell(in.vertices[3 * i + 2] - minZ, cellsPerAxis);
        return static_cast<uint64_t>(ci)
             | (static_cast<uint64_t>(cj) << 20)
             | (static_cast<uint64_t>(ck) << 40);
    };

    std::unordered_map<uint64_t, int> cellToNew;
    cellToNew.reserve(nv + 1);
    std::vector<int> remap(nv, -1);
    std::vector<double> sc, cnt;

    for (size_t i = 0; i < nv; ++i) {
        const uint64_t k = keyFor(i);
        auto it = cellToNew.find(k);
        int newIdx;
        if (it == cellToNew.end()) {
            newIdx = static_cast<int>(cellToNew.size());
            cellToNew[k] = newIdx;
            sc.push_back(0.0); cnt.push_back(0.0);
        } else {
            newIdx = it->second;
        }
        remap[i] = newIdx;
        if (i < fullScalars.size()) sc[newIdx] += static_cast<double>(fullScalars[i]);
        cnt[newIdx] += 1.0;
    }

    const int newCount = static_cast<int>(cellToNew.size());
    std::vector<float> out(static_cast<size_t>(newCount));
    for (int i = 0; i < newCount; ++i) {
        const double inv = cnt[i] > 0.0 ? 1.0 / cnt[i] : 0.0;
        out[i] = static_cast<float>(sc[i] * inv);
    }
    return out;
}

void MeshGLManager::updateScalars(std::shared_ptr<const std::vector<float>> scalars) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::vector<float>* src = scalars.get();
    const bool hasData = src && !src->empty();

    // Decimated LOD scalars are derived by clustering, not a 1:1 copy. This is
    // the only unavoidable intermediate allocation (pre-existing behavior);
    // the GUI/render thread no longer copies the full-resolution payload.
    std::vector<float> decScalars = hasData ? decimateScalars(*src) : std::vector<float>{};

    auto reupload = [&](std::vector<Mesh>& meshes, const std::vector<float>& data) {
        for (auto& m : meshes) {
            if (!data.empty()) {
                if (!m.sbo.has()) {
                    glCreateBuffers(1, m.sbo.ptr());
                    glEnableVertexArrayAttrib(m.vao, 2);
                    glVertexArrayAttribFormat(m.vao, 2, 1, GL_FLOAT, GL_FALSE, 0);
                    glVertexArrayAttribBinding(m.vao, 2, 2);
                    glVertexArrayVertexBuffer(m.vao, 2, m.sbo, 0, sizeof(float));
                }
                const size_t bytes = data.size() * sizeof(float);
                glNamedBufferData(m.sbo, bytes, nullptr, GL_STATIC_DRAW);
                glNamedBufferSubData(m.sbo, 0, bytes, data.data());
            } else if (m.sbo.has()) {
                m.sbo.reset();
            }
        }
    };
    reupload(meshes_, hasData ? *src : std::vector<float>{});
    reupload(decimatedMeshes_, decScalars);
}

void MeshGLManager::snapshotDrawList(std::vector<std::pair<GLuint, int>>& out,
                                      bool useLod, bool cameraMoving,
                                      std::vector<int>& outMode,
                                      std::vector<int>& outVerts) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::vector<Mesh>& src =
        (useLod && cameraMoving && hasDecimated_) ? decimatedMeshes_ : meshes_;
    out.reserve(src.size());
    outMode.reserve(src.size());
    outVerts.reserve(src.size());
    for (const auto& m : src) {
        out.push_back({m.vao.get(), m.indexCount});
        outMode.push_back(0);          // triangles handled by indexCount path
        outVerts.push_back(m.vertexCount);
    }
}

namespace {
GLuint compileCompute(const char* src, const char* label, std::string& errOut) {
    GLuint s = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, 512, nullptr, log);
        errOut += std::string("[LOD] ") + label + " compile error: " + log + "\n";
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint linkCompute(GLuint s, const char* label, std::string& errOut) {
    GLuint p = glCreateProgram();
    glAttachShader(p, s);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, 512, nullptr, log);
        errOut += std::string("[LOD] ") + label + " link error: " + log + "\n";
        glDeleteProgram(p);
        return 0;
    }
    glDeleteShader(s);
    return p;
}
}

void MeshGLManager::initLodCompute(const std::string& accumSrc, const std::string& outputSrc, const std::string& trisSrc) {
    if (lodProgramAccum.has()) return;

    lastLodError_.clear();

    GLuint sAccum = compileCompute(accumSrc.c_str(), "accum", lastLodError_);
    GLuint sOutput = compileCompute(outputSrc.c_str(), "output", lastLodError_);
    GLuint sTris = compileCompute(trisSrc.c_str(), "tris", lastLodError_);

    lodProgramAccum.reset(sAccum ? linkCompute(sAccum, "accum", lastLodError_) : 0);
    lodProgramOutput.reset(sOutput ? linkCompute(sOutput, "output", lastLodError_) : 0);
    lodProgramTris.reset(sTris ? linkCompute(sTris, "tris", lastLodError_) : 0);

    if (!lodProgramAccum.has() || !lodProgramOutput.has() || !lodProgramTris.has()) {
        cleanupLodCompute();
        return;
    }

    glCreateBuffers(1, lodCellSsbo.ptr());
    glCreateBuffers(1, lodRemapSsbo.ptr());
    glCreateBuffers(1, lodCounterSsbo.ptr());
    glCreateBuffers(1, lodParamsUbo.ptr());
    lodGpuDecimationReady = true;
}

void MeshGLManager::cleanupLodCompute() {
    lodProgramAccum.reset();
    lodProgramOutput.reset();
    lodProgramTris.reset();
    lodCellSsbo.reset();
    lodRemapSsbo.reset();
    lodCounterSsbo.reset();
    lodParamsUbo.reset();
    lodGpuDecimationReady = false;
    lodCellsPerAxis = 0;
}

void MeshGLManager::setComputeShaderSources(const std::string& accumSrc, const std::string& outputSrc, const std::string& trisSrc) {
    lodAccumSrc_ = accumSrc;
    lodOutputSrc_ = outputSrc;
    lodTrisSrc_ = trisSrc;
}

bool MeshGLManager::dispatchLodCompute(const RenderMesh& mesh, Mesh& outDecimated) {
    if (!lodGpuDecimationReady) initLodCompute(lodAccumSrc_, lodOutputSrc_, lodTrisSrc_);
    if (!lodProgramAccum.has() || !lodProgramOutput.has() || !lodProgramTris.has()) {
        if (lastLodError_.empty()) lastLodError_ = "[LOD] compute programs not ready";
        return false;
    }

    // The persistent LOD buffers stay bound to SSBO slots 8-11 from the
    // previous dispatch. Resizing them with glNamedBufferData while bound to an
    // indexed binding point generates GL_INVALID_OPERATION, so unbind first.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 11, 0);

    const size_t nv = mesh.vertices.size() / 3;
    if (nv < 3 || mesh.indices.empty()) {
        lastLodError_ = "[LOD] source mesh too small or has no triangles (nv="
            + std::to_string(nv) + ", indices=" + std::to_string(mesh.indices.size()) + ")";
        return false;
    }

    const double dx = mesh.bounds.maxX - mesh.bounds.minX;
    const double dy = mesh.bounds.maxY - mesh.bounds.minY;
    const double dz = mesh.bounds.maxZ - mesh.bounds.minZ;
    const double diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (diag < 1e-9) {
        lastLodError_ = "[LOD] degenerate bounds (diag=" + std::to_string(diag) + ")";
        return false;
    }

    int cellsPerAxis = static_cast<int>(std::round(std::pow((double)nv, 1.0 / 3.0) * 0.5));
    cellsPerAxis = std::max(2, std::min(cellsPerAxis, 128));
    const int totalCells = cellsPerAxis * cellsPerAxis * cellsPerAxis;
    lodCellsPerAxis = cellsPerAxis;

    struct Params {
        glm::ivec4 cellsPerAxis;
        glm::vec4 boundsMin;
        glm::vec4 boundsMax;
        int vertexCount;
        int triangleCount;
        int hasNormals;
        int hasScalars;
    };
    Params params;
    //params.cellsPerAxis = glm::ivec4(cellsPerAxis, 0, 0, 0);
    params.cellsPerAxis = glm::ivec4(cellsPerAxis, cellsPerAxis, cellsPerAxis, 0);
    params.boundsMin = glm::vec4(static_cast<float>(mesh.bounds.minX), static_cast<float>(mesh.bounds.minY), static_cast<float>(mesh.bounds.minZ), 0.0f);
    params.boundsMax = glm::vec4(static_cast<float>(mesh.bounds.maxX), static_cast<float>(mesh.bounds.maxY), static_cast<float>(mesh.bounds.maxZ), 0.0f);
    params.vertexCount = static_cast<int>(nv);
    params.triangleCount = static_cast<int>(mesh.indices.size() / 3);
    params.hasNormals = mesh.normals.size() == mesh.vertices.size() && !mesh.normals.empty() ? 1 : 0;
    params.hasScalars = !mesh.scalars.empty() ? 1 : 0;

    glNamedBufferData(lodParamsUbo, sizeof(Params), &params, GL_STATIC_DRAW);
    glNamedBufferData(lodCellSsbo, totalCells * sizeof(unsigned int) * 10, nullptr, GL_DYNAMIC_DRAW);
    glNamedBufferData(lodRemapSsbo, nv * sizeof(unsigned int), nullptr, GL_DYNAMIC_DRAW);
    glNamedBufferData(lodCounterSsbo, 2 * sizeof(unsigned int), nullptr, GL_DYNAMIC_DRAW);
    {
        // Zero the cell data buffer so atomicAdd starts from 0.
        std::vector<GLuint> zeros(totalCells * 10, 0);
        glNamedBufferSubData(lodCellSsbo, 0, zeros.size() * sizeof(GLuint), zeros.data());
    }
    GLuint zero = 0;
    glNamedBufferSubData(lodCounterSsbo, 0, sizeof(unsigned int), &zero);
    glNamedBufferSubData(lodCounterSsbo, sizeof(unsigned int), sizeof(unsigned int), &zero);

    GlBuffer inPosSsbo, inNormSsbo, inScalSsbo, inIdxSsbo;
    glCreateBuffers(1, inPosSsbo.ptr());
    glCreateBuffers(1, inNormSsbo.ptr());
    glCreateBuffers(1, inScalSsbo.ptr());
    glCreateBuffers(1, inIdxSsbo.ptr());
    glNamedBufferData(inPosSsbo, mesh.vertices.size() * sizeof(float), mesh.vertices.data(), GL_STATIC_DRAW);
    glNamedBufferData(inNormSsbo, std::max<size_t>(1, mesh.normals.size()) * sizeof(float), mesh.normals.empty() ? nullptr : mesh.normals.data(), GL_STATIC_DRAW);
    glNamedBufferData(inScalSsbo, std::max<size_t>(1, mesh.scalars.size()) * sizeof(float), mesh.scalars.empty() ? nullptr : mesh.scalars.data(), GL_STATIC_DRAW);
    glNamedBufferData(inIdxSsbo, mesh.indices.size() * sizeof(unsigned int), mesh.indices.data(), GL_STATIC_DRAW);

    GlBuffer outPosSsbo, outNormSsbo, outScalSsbo, outIdxSsbo;
    glCreateBuffers(1, outPosSsbo.ptr());
    glCreateBuffers(1, outNormSsbo.ptr());
    glCreateBuffers(1, outScalSsbo.ptr());
    glCreateBuffers(1, outIdxSsbo.ptr());
    glNamedBufferData(outPosSsbo, mesh.vertices.size() * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glNamedBufferData(outNormSsbo, std::max<size_t>(1, mesh.normals.size()) * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glNamedBufferData(outScalSsbo, std::max<size_t>(1, mesh.scalars.size()) * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glNamedBufferData(outIdxSsbo, mesh.indices.size() * sizeof(unsigned int), nullptr, GL_DYNAMIC_DRAW);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, inPosSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, inNormSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, inScalSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, inIdxSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, outPosSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, outNormSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, outScalSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, outIdxSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, lodCellSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, lodParamsUbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, lodCounterSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 11, lodRemapSsbo);

    GLuint groups = (static_cast<GLuint>(nv) + 255u) / 256u;

    glUseProgram(lodProgramAccum);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    glDispatchCompute(groups, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    GLenum errAccum = glGetError();

    glUseProgram(lodProgramOutput);
    glDispatchCompute(totalCells, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    GLenum errOutput = glGetError();

    glUseProgram(lodProgramTris);
    GLuint triGroups = (static_cast<GLuint>(mesh.indices.size() / 3) + 255u) / 256u;
    glDispatchCompute(triGroups, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    glMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
    GLenum errTris = glGetError();

    // The compute writes must be visible to the host before the counter/data
    // readbacks below. GL_SHADER_STORAGE_BARRIER_BIT only orders shader-to-shader
    // accesses; host reads of buffer data additionally need
    // GL_BUFFER_UPDATE_BARRIER_BIT (plus glFinish for a hard sync point). Without
    // it, drivers may return stale zeros from the SSBOs.
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    glFinish();

    GLuint outVertCount = 0, outTriCount = 0;
    glGetNamedBufferSubData(lodCounterSsbo, 0, sizeof(unsigned int), &outVertCount);
    glGetNamedBufferSubData(lodCounterSsbo, sizeof(unsigned int), sizeof(unsigned int), &outTriCount);

    if (outVertCount == 0 || outTriCount == 0) {
        GLuint cellSample[8] = {0,0,0,0,0,0,0,0};
        glGetNamedBufferSubData(lodCellSsbo, 0, sizeof(cellSample), cellSample);
        lastLodError_ = "[LOD] compute produced empty result (verts="
            + std::to_string(outVertCount) + ", tris=" + std::to_string(outTriCount)
            + ", nv=" + std::to_string(nv) + ", cellsPerAxis=" + std::to_string(cellsPerAxis)
            + ", hasNormals=" + std::to_string(params.hasNormals)
            + ", hasScalars=" + std::to_string(params.hasScalars)
            + ", accumCells[0..7]="
            + std::to_string(cellSample[0]) + "," + std::to_string(cellSample[1]) + ","
            + std::to_string(cellSample[2]) + "," + std::to_string(cellSample[3]) + ","
            + std::to_string(cellSample[4]) + "," + std::to_string(cellSample[5]) + ","
            + std::to_string(cellSample[6]) + "," + std::to_string(cellSample[7])
            + ", glErr=0x" + [&]() { char buf[8]; std::snprintf(buf, sizeof(buf), "%X", errAccum | errOutput | errTris); return std::string(buf); }() + ")";
        return false;
    }

    std::vector<float> outVerts(outVertCount * 3);
    std::vector<float> outNorms(outVertCount * 3);
    std::vector<float> outScalars(outVertCount);
    std::vector<unsigned int> outIndices(outTriCount);

    glGetNamedBufferSubData(outPosSsbo, 0, outVerts.size() * sizeof(float), outVerts.data());
    glGetNamedBufferSubData(outNormSsbo, 0, outNorms.size() * sizeof(float), outNorms.data());
    glGetNamedBufferSubData(outScalSsbo, 0, outScalars.size() * sizeof(float), outScalars.data());
    glGetNamedBufferSubData(outIdxSsbo, 0, outIndices.size() * sizeof(unsigned int), outIndices.data());

    outDecimated = Mesh{};
    outDecimated.vertexCount = static_cast<int>(outVertCount);
    outDecimated.indexCount = static_cast<int>(outIndices.size());

    glCreateBuffers(1, outDecimated.vbo.ptr());
    glCreateBuffers(1, outDecimated.nbo.ptr());
    glCreateBuffers(1, outDecimated.ebo.ptr());
    if (!outScalars.empty()) glCreateBuffers(1, outDecimated.sbo.ptr());

    glNamedBufferData(outDecimated.vbo, outVerts.size() * sizeof(float), outVerts.data(), GL_STATIC_DRAW);
    glNamedBufferData(outDecimated.nbo, outNorms.size() * sizeof(float), outNorms.data(), GL_STATIC_DRAW);
    glNamedBufferData(outDecimated.ebo, outIndices.size() * sizeof(unsigned int), outIndices.data(), GL_STATIC_DRAW);
    if (!outScalars.empty()) {
        glNamedBufferData(outDecimated.sbo, outScalars.size() * sizeof(float), outScalars.data(), GL_STATIC_DRAW);
    }

    glCreateVertexArrays(1, outDecimated.vao.ptr());
    glEnableVertexArrayAttrib(outDecimated.vao, 0);
    glVertexArrayAttribFormat(outDecimated.vao, 0, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(outDecimated.vao, 0, 0);
    glVertexArrayVertexBuffer(outDecimated.vao, 0, outDecimated.vbo, 0, 3 * sizeof(float));

    glEnableVertexArrayAttrib(outDecimated.vao, 1);
    glVertexArrayAttribFormat(outDecimated.vao, 1, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(outDecimated.vao, 1, 1);
    glVertexArrayVertexBuffer(outDecimated.vao, 1, outDecimated.nbo, 0, 3 * sizeof(float));

    if (!outScalars.empty()) {
        glEnableVertexArrayAttrib(outDecimated.vao, 2);
        glVertexArrayAttribFormat(outDecimated.vao, 2, 1, GL_FLOAT, GL_FALSE, 0);
        glVertexArrayAttribBinding(outDecimated.vao, 2, 2);
        glVertexArrayVertexBuffer(outDecimated.vao, 2, outDecimated.sbo, 0, sizeof(float));
    }

    glVertexArrayElementBuffer(outDecimated.vao, outDecimated.ebo);

    return true;
}
