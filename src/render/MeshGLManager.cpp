#include "render/MeshGLManager.h"
#include "core/mesh_loader.h"

#include <cmath>
#include <unordered_map>
#include <algorithm>
#include <string>

namespace {
// LOD vertex-clustering decimation averages every vertex inside a coarse spatial
// cell into a single point. That is only safe for a densely-sampled volumetric
// lattice, where neighbouring grid points genuinely belong to the same surface.
// For surface meshes (STL) and non-volumetric VTK datasets (POLYDATA, isolated
// unstructured surfaces) the same clustering merges topologically unrelated
// vertices across disconnected shells / thin features, producing the reported
// "overlapping vertices / spurious new surfaces" artifact while the camera moves.
//
// So we only allow decimation for the regular volumetric grid dataset types.
bool datasetSupportsDecimation(const RenderMesh& in) {
    const std::string& t = in.datasetType;
    return t == "STRUCTURED_GRID" ||
           t == "STRUCTURED_POINTS" ||
           t == "RECTILINEAR_GRID";
}
} // namespace

void MeshGLManager::destroyMesh(Mesh& mesh) {
    glDeleteVertexArrays(1, &mesh.vao);
    glDeleteBuffers(1, &mesh.vbo);
    glDeleteBuffers(1, &mesh.nbo);
    glDeleteBuffers(1, &mesh.ebo);
    if (mesh.sbo) glDeleteBuffers(1, &mesh.sbo);
}

void MeshGLManager::buildMeshGL(const RenderMesh& renderMesh, std::vector<Mesh>& out) {
    Mesh mesh;
    mesh.indexCount = static_cast<int>(renderMesh.indices.size());

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

void MeshGLManager::upload(const RenderMesh& renderMesh) {
    std::vector<Mesh> newFull, newDec;

    // Full-resolution mesh.
    buildMeshGL(renderMesh, newFull);

    // LOD: a coarsely decimated mesh, used only while the camera is moving.
    // Only volumetric grid datasets are eligible — see datasetSupportsDecimation()
    // for why surface/STL and non-volumetric VTK meshes must NOT be clustered.
    if (datasetSupportsDecimation(renderMesh)) {
        RenderMesh decimated = decimate(renderMesh);
        bool lodWorthwhile = !decimated.indices.empty() &&
                             decimated.indices.size() < renderMesh.indices.size() / 2;
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

void MeshGLManager::updateScalars(const std::vector<float>& scalars) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto reupload = [&](std::vector<Mesh>& meshes) {
        for (auto& m : meshes) {
            if (!scalars.empty()) {
                if (m.sbo == 0) {
                    glGenBuffers(1, &m.sbo);
                    glBindVertexArray(m.vao);
                    glBindBuffer(GL_ARRAY_BUFFER, m.sbo);
                    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(float), (void*)0);
                    glEnableVertexAttribArray(2);
                    glBindVertexArray(0);
                }
                glBindBuffer(GL_ARRAY_BUFFER, m.sbo);
                glBufferData(GL_ARRAY_BUFFER, scalars.size() * sizeof(float),
                             scalars.data(), GL_STATIC_DRAW);
            } else if (m.sbo != 0) {
                glDeleteBuffers(1, &m.sbo);
                m.sbo = 0;
            }
        }
    };
    reupload(meshes_);
    reupload(decimatedMeshes_);
}

void MeshGLManager::clear() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& m : meshes_) destroyMesh(m);
        meshes_.clear();
        for (auto& m : decimatedMeshes_) destroyMesh(m);
        decimatedMeshes_.clear();
        hasDecimated_ = false;
    }
}

void MeshGLManager::snapshotDrawList(std::vector<std::pair<GLuint, int>>& out,
                                      bool useLod, bool cameraMoving) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::vector<Mesh>& src =
        (useLod && cameraMoving && hasDecimated_) ? decimatedMeshes_ : meshes_;
    out.reserve(src.size());
    for (const auto& m : src) out.push_back({m.vao, m.indexCount});
}
