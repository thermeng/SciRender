#include "render/MeshGLManager.h"
#include "core/mesh_loader.h"

#include <cmath>
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
    int cellsPerAxis = static_cast<int>(std::round(std::pow((double)nv, 1.0 / 3.0) * 0.5));
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

void MeshGLManager::destroyMesh(Mesh& mesh) {
    glDeleteVertexArrays(1, &mesh.vao);
    glDeleteBuffers(1, &mesh.vbo);
    glDeleteBuffers(1, &mesh.nbo);
    glDeleteBuffers(1, &mesh.ebo);
    if (mesh.sbo) glDeleteBuffers(1, &mesh.sbo);
    if (mesh.lineVao) { glDeleteVertexArrays(1, &mesh.lineVao); glDeleteBuffers(1, &mesh.lineVbo); }
}

void MeshGLManager::buildMeshGL(const RenderMesh& renderMesh, std::vector<Mesh>& out) {
    Mesh mesh;
    mesh.indexCount = static_cast<int>(renderMesh.indices.size());
    mesh.vertexCount = static_cast<int>(renderMesh.vertices.size() / 3);

    glGenVertexArrays(1, &mesh.vao);
    glGenBuffers(1, &mesh.vbo);
    glGenBuffers(1, &mesh.nbo);
    glGenBuffers(1, &mesh.ebo);

    glBindVertexArray(mesh.vao);

    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, renderMesh.vertices.size() * sizeof(float), renderMesh.vertices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindBuffer(GL_ARRAY_BUFFER, mesh.nbo);
    // B4: never upload an empty normal buffer. If normals are missing (should not
    // happen once computeNormals runs, but keep this robust) attribute 1 would
    // otherwise point at a zero-sized/garbage buffer => undefined shading. Fall
    // back to a per-vertex (0,0,1) so lighting is at least deterministic.
    const size_t vertCount = renderMesh.vertices.size() / 3;
    if (renderMesh.normals.size() == renderMesh.vertices.size() && !renderMesh.normals.empty()) {
        glBufferData(GL_ARRAY_BUFFER, renderMesh.normals.size() * sizeof(float), renderMesh.normals.data(), GL_STATIC_DRAW);
    } else {
        std::vector<float> fallback(vertCount * 3, 0.0f);
        for (size_t i = 0; i < vertCount; ++i) fallback[i * 3 + 2] = 1.0f;
        glBufferData(GL_ARRAY_BUFFER, fallback.size() * sizeof(float), fallback.data(), GL_STATIC_DRAW);
    }
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);

    if (!renderMesh.scalars.empty()) {
        glGenBuffers(1, &mesh.sbo);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.sbo);
        glBufferData(GL_ARRAY_BUFFER, renderMesh.scalars.size() * sizeof(float), renderMesh.scalars.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(float), (void*)0);
        glEnableVertexAttribArray(2);
    } else {
        mesh.sbo = 0;
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, renderMesh.indices.size() * sizeof(unsigned int), renderMesh.indices.data(), GL_STATIC_DRAW);

    // ponytail: per-cell boundary edges (cellEdges) -> dedicated line VBO
    if (!renderMesh.cellEdges.empty()) {
        glGenVertexArrays(1, &mesh.lineVao);
        glGenBuffers(1, &mesh.lineVbo);
        glBindVertexArray(mesh.lineVao);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.lineVbo);
        glBufferData(GL_ARRAY_BUFFER, renderMesh.cellEdges.size() * sizeof(float), renderMesh.cellEdges.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);
        mesh.lineCount = static_cast<int>(renderMesh.cellEdges.size() / 3);
    }

    glBindVertexArray(0);
    out.push_back(mesh);
}

RenderMesh MeshGLManager::decimate(const RenderMesh& in) {
    RenderMesh out;
    const size_t nv = in.vertices.size() / 3;
    // Small meshes gain nothing from LOD and risk degeneracy — skip them.
    if (nv < 4000 || in.indices.size() < 3) return out;

    const double minX = in.bounds.minX, minY = in.bounds.minY, minZ = in.bounds.minZ;
    const double dx = in.bounds.maxX - minX, dy = in.bounds.maxY - minY, dz = in.bounds.maxZ - minZ;
    const double diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (diag < 1e-9) return out;

    // Cells per axis chosen so the cluster count is a coarse fraction of the
    // vertices (~half the "one cell per vertex" resolution => ~1/8th vertices).
    int cellsPerAxis = static_cast<int>(std::round(std::pow((double)nv, 1.0 / 3.0) * 0.5));
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
        for (auto& m : meshes_) destroyMesh(m);
        meshes_.clear();
        for (auto& m : decimatedMeshes_) destroyMesh(m);
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
        for (auto& m : meshes_) destroyMesh(m);
        meshes_.clear();
        for (auto& m : decimatedMeshes_) destroyMesh(m);
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
    int cellsPerAxis = static_cast<int>(std::round(std::pow((double)nv, 1.0 / 3.0) * 0.5));
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
                if (m.sbo == 0) {
                    glGenBuffers(1, &m.sbo);
                    glBindVertexArray(m.vao);
                    glBindBuffer(GL_ARRAY_BUFFER, m.sbo);
                    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(float), (void*)0);
                    glEnableVertexAttribArray(2);
                    glBindVertexArray(0);
                }
                glBindBuffer(GL_ARRAY_BUFFER, m.sbo);
                const size_t bytes = data.size() * sizeof(float);
                // Orphan: discard the old backing store so the driver can
                // pipeline a fresh allocation and avoid stalling on a live
                // buffer. Then stream the new scalars in via glBufferSubData.
                glBufferData(GL_ARRAY_BUFFER, bytes, nullptr, GL_STATIC_DRAW);
                glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, data.data());
            } else if (m.sbo != 0) {
                glDeleteBuffers(1, &m.sbo);
                m.sbo = 0;
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
        out.push_back({m.vao, m.indexCount});
        outMode.push_back(0);          // triangles handled by indexCount path
        outVerts.push_back(m.vertexCount);
    }
}
