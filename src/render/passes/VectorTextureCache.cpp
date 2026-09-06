#include "render/passes/VectorTextureCache.h"
#include "core/FieldResolver.h"
#include <glad/gl.h>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <limits>

bool VectorTextureCache::resolveField(const RenderMesh& mesh, const std::string& name, int placement,
                                       const glm::vec3*& outData, size_t& outCount, bool& outIsCell) const {
    int effPlacement = placement;
    if (effPlacement < 0) {
        effPlacement = (mesh.meshHasCellVectors() && !mesh.meshHasVectors()) ? 1 : 0;
    }
    FieldResolver::VectorField vf = FieldResolver::resolveVector(mesh, name, effPlacement);
    // Strict placement: when the UI explicitly asks for Cell placement (1),
    // do NOT silently fall back to point vectors. That would hide a missing
    // cellCenters / cell data error behind a dim/mode mismatch.
    if ((!vf.data || vf.count == 0) && effPlacement != 1) {
        vf = FieldResolver::resolveVector(mesh, name, 0);
    }
    if (!vf.data || vf.count == 0) return false;
    outData = vf.data;
    outCount = vf.count;
    outIsCell = vf.isCell;
    return true;
}

bool VectorTextureCache::buildGrid(const RenderMesh& mesh, const glm::vec3* data, size_t count, bool isCell,
                                    std::vector<glm::vec3>& outGrid, int& outDimX, int& outDimY, int& outDimZ) const {
    constexpr size_t kMaxGridTexels = 8u * 1024u * 1024u; // 8M vec3 = 96MB cap
    int dimX = mesh.gridDimX, dimY = mesh.gridDimY, dimZ = mesh.gridDimZ;
    const size_t numVerts = mesh.vertices.size() / 3;
    const size_t limit = std::min(numVerts, count);

    if (dimX > 0 && dimY > 0 && dimZ > 0 && !isCell) {
        const size_t need = static_cast<size_t>(dimX) * static_cast<size_t>(dimY) * static_cast<size_t>(dimZ);
        if (need == limit && need <= kMaxGridTexels) {
            outGrid.resize(need);
            memcpy(outGrid.data(), data, outGrid.size() * sizeof(glm::vec3));
            outDimX = dimX; outDimY = dimY; outDimZ = dimZ;
            return true;
        }
        // else: fall through to resampled grid; oversized/mismatched structured
        // dims must never allocate blindly (OOM / int-overflow guard)
    }

    // General-purpose cell-grid handling: structured cell count is dim-1; for slab where dim==1 there are no cells along that axis,
    // but we keep 1 to maintain a valid 3D texture (depth 1) until rank-aware 2D path is implemented.
    if (dimX > 0 && dimY > 0 && dimZ > 0 && isCell) {
        int cDimX = (dimX > 1) ? dimX - 1 : 1;
        int cDimY = (dimY > 1) ? dimY - 1 : 1;
        int cDimZ = (dimZ > 1) ? dimZ - 1 : 1;
        const size_t need = static_cast<size_t>(cDimX) * static_cast<size_t>(cDimY) * static_cast<size_t>(cDimZ);
        if (need == count && need <= kMaxGridTexels) {
            outGrid.resize(need);
            memcpy(outGrid.data(), data, outGrid.size() * sizeof(glm::vec3));
            outDimX = cDimX; outDimY = cDimY; outDimZ = cDimZ;
            return true;
        }
    }

    int res = 32;
    if (numVerts > 0) {
        float density = std::cbrt(static_cast<float>(numVerts));
        int ideal = static_cast<int>(std::ceil(density));
        res = std::clamp(ideal, 16, 32);
        if (numVerts > 50000) res = 32;
        if (numVerts > 200000) res = 24;
    }
    outDimX = outDimY = outDimZ = res;
    size_t total = static_cast<size_t>(res) * res * res;
    if (total == 0 || total > kMaxGridTexels) return false;
    if (limit == 0) {
        outGrid.assign(total, glm::vec3(0.0f));
        return true;
    }
    outGrid.resize(total, glm::vec3(0.0f));

    float minX = static_cast<float>(mesh.bounds.minX), maxX = static_cast<float>(mesh.bounds.maxX);
    float minY = static_cast<float>(mesh.bounds.minY), maxY = static_cast<float>(mesh.bounds.maxY);
    float minZ = static_cast<float>(mesh.bounds.minZ), maxZ = static_cast<float>(mesh.bounds.maxZ);
    float rangeX = maxX - minX, rangeY = maxY - minY, rangeZ = maxZ - minZ;

    const float* verts = mesh.vertices.data();
    const glm::vec3* cellPos = nullptr;
    bool useCellCenters = false;
    if (isCell) {
        if (!mesh.cellCenters.empty() && mesh.cellCenters.size() == count) {
            cellPos = mesh.cellCenters.data();
            useCellCenters = true;
        } else {
            return false;
        }
    }

    struct SpatialEntry {
        float x, y, z;
        const glm::vec3* vec;
    };
    // Cap resample source points: full nearest-neighbour over 200k+ points is
    // ~345M distance evals on the render thread. Strided subsampling keeps
    // LIC interactive while preserving coverage.
    constexpr size_t kMaxResamplePoints = 65536;
    size_t stride = 1;
    if (limit > kMaxResamplePoints) stride = (limit + kMaxResamplePoints - 1) / kMaxResamplePoints;
    std::vector<SpatialEntry> spatial;
    spatial.reserve((limit + stride - 1) / stride);
    for (size_t v = 0; v < limit; v += stride) {
        SpatialEntry e;
        if (useCellCenters) {
            e.x = cellPos[v].x;
            e.y = cellPos[v].y;
            e.z = cellPos[v].z;
        } else {
            e.x = verts[v * 3 + 0];
            e.y = verts[v * 3 + 1];
            e.z = verts[v * 3 + 2];
        }
        e.vec = &data[v];
        spatial.push_back(e);
    }

    const int gridRes = 8;
    // General-purpose thin-axis handling: use relative epsilon based on scene diag, not absolute 1e-8 world units.
    float diagRange = std::sqrt(rangeX*rangeX + rangeY*rangeY + rangeZ*rangeZ);
    float epsRange = std::max(diagRange * 1e-7f, 1e-7f);
    std::vector<std::vector<size_t>> grid(static_cast<size_t>(gridRes) * gridRes * gridRes);
    for (size_t v = 0; v < spatial.size(); ++v) {
        int gx = static_cast<int>(std::clamp((spatial[v].x - minX) / (rangeX > epsRange ? rangeX : epsRange), 0.0f, 0.999f) * gridRes);
        int gy = static_cast<int>(std::clamp((spatial[v].y - minY) / (rangeY > epsRange ? rangeY : epsRange), 0.0f, 0.999f) * gridRes);
        int gz = static_cast<int>(std::clamp((spatial[v].z - minZ) / (rangeZ > epsRange ? rangeZ : epsRange), 0.0f, 0.999f) * gridRes);
        grid[static_cast<size_t>(gx + gy * gridRes + gz * gridRes * gridRes)].push_back(v);
    }

    // Linear resampling: inverse-distance weighted (IDW) average over 27-cell neighborhood
    // replaces pure nearest-neighbor Voronoi. Matches Streamline's trilinear philosophy
    // and gives smooth GL_LINEAR interpolation. eps avoids singularity at texel center.
    const float idwEps = 1e-6f * (rangeX*rangeX + rangeY*rangeY + rangeZ*rangeZ + 1.0f);
    for (size_t i = 0; i < total; ++i) {
        int ix = static_cast<int>(i) % res;
        int iy = (static_cast<int>(i) / res) % res;
        int iz = static_cast<int>(i) / (res * res);
        float wx = (ix + 0.5f) / static_cast<float>(res);
        float wy = (iy + 0.5f) / static_cast<float>(res);
        float wz = (iz + 0.5f) / static_cast<float>(res);
        float px = minX + wx * rangeX;
        float py = minY + wy * rangeY;
        float pz = minZ + wz * rangeZ;

        glm::vec3 acc(0.0f);
        float wSum = 0.0f;
        int gx = static_cast<int>(std::clamp((px - minX) / (rangeX > epsRange ? rangeX : epsRange), 0.0f, 0.999f) * gridRes);
        int gy = static_cast<int>(std::clamp((py - minY) / (rangeY > epsRange ? rangeY : epsRange), 0.0f, 0.999f) * gridRes);
        int gz = static_cast<int>(std::clamp((pz - minZ) / (rangeZ > epsRange ? rangeZ : epsRange), 0.0f, 0.999f) * gridRes);
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = gx + dx, ny = gy + dy, nz = gz + dz;
                    if (nx < 0 || nx >= gridRes || ny < 0 || ny >= gridRes || nz < 0 || nz >= gridRes) continue;
                    const auto& cell = grid[static_cast<size_t>(nx + ny * gridRes + nz * gridRes * gridRes)];
                    for (size_t v : cell) {
                        const auto& e = spatial[v];
                        float ddx = e.x - px, ddy = e.y - py, ddz = e.z - pz;
                        float d2 = ddx * ddx + ddy * ddy + ddz * ddz;
                        // IDW p=2: w = 1/(d2+eps) ; close samples dominate but neighbors blend
                        float w = 1.0f / (d2 + idwEps);
                        acc += (*e.vec) * w;
                        wSum += w;
                    }
                }
            }
        }
        outGrid[i] = (wSum > 0.0f) ? (acc / wSum) : glm::vec3(0.0f);
    }
    return true;
}

std::string VectorTextureCache::makeKey(const std::string& name, bool isCell) {
    // Canonical key: no boundary mode (always Repeat). Legacy "#B*" suffix dropped.
    return name + (isCell ? "#C" : "#P");
}
std::string VectorTextureCache::makeKey(const std::string& name, bool isCell, int /*licBoundaryMode*/) {
    return makeKey(name, isCell);
}

void VectorTextureCache::touch(Entry& e, const std::string& key) {
    if (e.lruIt != m_lru.end()) {
        m_lru.erase(e.lruIt);
    }
    m_lru.push_front(key);
    e.lruIt = m_lru.begin();
}

void VectorTextureCache::evictIfNeeded() {
    while (m_entries.size() > kMaxEntries) {

        const std::string& oldest = m_lru.back();
        m_entries.erase(oldest);
        m_lru.pop_back();
    }
}

GLuint VectorTextureCache::textureForField(const std::string& name, const RenderMesh* mesh,
                                             const glm::vec3& boxMin, const glm::vec3& boxMax,
                                             int placement, int licBoundaryMode) {
    if (!mesh || name.empty()) return 0;

    const glm::vec3* data = nullptr;
    size_t count = 0;
    bool isCell = false;
    if (!resolveField(*mesh, name, placement, data, count, isCell)) return 0;

    int dimX = mesh->gridDimX, dimY = mesh->gridDimY, dimZ = mesh->gridDimZ;
    const size_t numVerts = mesh->vertices.size() / 3;
    const size_t limit = std::min(numVerts, count);
    int baseDimX = 0, baseDimY = 0, baseDimZ = 0;
    if (!isCell && dimX > 0 && dimY > 0 && dimZ > 0 &&
        static_cast<size_t>(dimX) * static_cast<size_t>(dimY) * static_cast<size_t>(dimZ) == limit) {
        baseDimX = dimX; baseDimY = dimY; baseDimZ = dimZ;
    } else if (isCell && dimX > 0 && dimY > 0 && dimZ > 0) {
        int cDimX = (dimX > 1) ? dimX - 1 : 1;
        int cDimY = (dimY > 1) ? dimY - 1 : 1;
        int cDimZ = (dimZ > 1) ? dimZ - 1 : 1;
        if (static_cast<size_t>(cDimX) * static_cast<size_t>(cDimY) * static_cast<size_t>(cDimZ) == count) {
            baseDimX = cDimX; baseDimY = cDimY; baseDimZ = cDimZ;
        } else {

            float density = std::cbrt(static_cast<float>(std::max<size_t>(numVerts, 1)));
            int r = std::clamp(static_cast<int>(std::ceil(density)), 16, 32);
            if (numVerts > 50000) r = 32;
            if (numVerts > 200000) r = 24;
            baseDimX = baseDimY = baseDimZ = r;
        }
    } else {
        float density = std::cbrt(static_cast<float>(std::max<size_t>(numVerts, 1)));
        int r = std::clamp(static_cast<int>(std::ceil(density)), 16, 32);
        if (numVerts > 50000) r = 32;
        if (numVerts > 200000) r = 24;
        baseDimX = baseDimY = baseDimZ = r;
    }
    // Wrap mode is fixed to Repeat (GL_REPEAT); no mirror padding.
    int expectedDimX = baseDimX, expectedDimY = baseDimY, expectedDimZ = baseDimZ;
    const std::string key = makeKey(name, isCell);
    auto it = m_entries.find(key);
    if (it != m_entries.end() && it->second.tex.has()
        && it->second.dimX == expectedDimX && it->second.dimY == expectedDimY && it->second.dimZ == expectedDimZ) {
        touch(it->second, key);
        return it->second.tex.get();
    }

    return buildTexture(name, mesh, boxMin, boxMax, placement, licBoundaryMode);
}

GLuint VectorTextureCache::buildTexture(const std::string& name, const RenderMesh* mesh,
                                         const glm::vec3& boxMin, const glm::vec3& boxMax,
                                         int placement, int licBoundaryMode) {
    (void)boxMin; (void)boxMax; (void)placement;
    const glm::vec3* data = nullptr;
    size_t count = 0;
    bool isCell = false;
    if (!resolveField(*mesh, name, placement, data, count, isCell)) return 0;

    std::vector<glm::vec3> grid;
    int dimX = 0, dimY = 0, dimZ = 0;
    if (!buildGrid(*mesh, data, count, isCell, grid, dimX, dimY, dimZ)) return 0;




    int texDimX = dimX, texDimY = dimY, texDimZ = dimZ;
    const glm::vec3* texData = grid.data();
    int wrapMode = GL_REPEAT; // fixed: Repeat (GL_REPEAT)
    (void)licBoundaryMode; // deprecated — ignored, kept for API compat

    if (texDimX <= 0 || texDimY <= 0 || texDimZ <= 0) return 0;
    {
        constexpr size_t kMaxTexTexels = 8u * 1024u * 1024u;
        const size_t need =
            static_cast<size_t>(texDimX) * static_cast<size_t>(texDimY) * static_cast<size_t>(texDimZ);
        if (need == 0 || need > kMaxTexTexels) return 0;
    }

    GLuint raw = 0;
    glCreateTextures(GL_TEXTURE_3D, 1, &raw);
    if (raw == 0) return 0;
    glTextureStorage3D(raw, 1, GL_RGB32F, texDimX, texDimY, texDimZ);
    glTextureParameteri(raw, GL_TEXTURE_WRAP_S, wrapMode);
    glTextureParameteri(raw, GL_TEXTURE_WRAP_T, wrapMode);
    glTextureParameteri(raw, GL_TEXTURE_WRAP_R, wrapMode);
    glTextureParameteri(raw, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(raw, GL_TEXTURE_MAG_FILTER, GL_LINEAR);


    size_t bytes = static_cast<size_t>(texDimX) * texDimY * texDimZ * 3 * sizeof(float);
    if (!m_pboInitialized) {
        for (int i = 0; i < 2; ++i)
            if (!m_pbo[i].has()) glCreateBuffers(1, m_pbo[i].ptr());
        m_pboInitialized = true;
    }
    if (m_pbo[0].has() && bytes > 0) {
        GlBuffer& pbo = m_pbo[m_pboIndex];
        m_pboIndex = (m_pboIndex + 1) % 2;
        glNamedBufferData(pbo, bytes, nullptr, GL_STREAM_DRAW);
        void* ptr = glMapNamedBufferRange(pbo, 0, bytes, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
        if (ptr) {
            std::memcpy(ptr, texData, bytes);
            glUnmapNamedBuffer(pbo);
            GLint prevPbo = 0;
            glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prevPbo);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo.get());
            glTextureSubImage3D(raw, 0, 0, 0, 0, texDimX, texDimY, texDimZ, GL_RGB, GL_FLOAT, nullptr);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(prevPbo));
        } else {
            glTextureSubImage3D(raw, 0, 0, 0, 0, texDimX, texDimY, texDimZ, GL_RGB, GL_FLOAT, texData);
        }
    } else {
        glTextureSubImage3D(raw, 0, 0, 0, 0, texDimX, texDimY, texDimZ, GL_RGB, GL_FLOAT, texData);
    }

    const std::string key = makeKey(name, isCell);
    auto existing = m_entries.find(key);
    if (existing != m_entries.end() && existing->second.lruIt != m_lru.end()) {
        m_lru.erase(existing->second.lruIt);
    }
    Entry& e = m_entries[key];
    e.tex.reset(raw);
    e.dimX = texDimX; e.dimY = texDimY; e.dimZ = texDimZ;
    m_lru.push_front(key);
    e.lruIt = m_lru.begin();
    evictIfNeeded();
    return raw;
}

bool VectorTextureCache::getTextureDims(const std::string& name, int& dimX, int& dimY, int& dimZ) const {
    auto it = m_entries.find(name);
    if (it != m_entries.end()) {
        dimX = it->second.dimX;
        dimY = it->second.dimY;
        dimZ = it->second.dimZ;
        return true;
    }
    // Scan over isCell variants (no boundary mode). Keep prefix-safe exact match.
    for (const auto& key : m_lru) {
        for (int cell = 0; cell <= 1; ++cell) {
            if (key == makeKey(name, cell != 0)) {
                auto jt = m_entries.find(key);
                if (jt != m_entries.end()) {
                    dimX = jt->second.dimX;
                    dimY = jt->second.dimY;
                    dimZ = jt->second.dimZ;
                    return true;
                }
            }
        }
    }
    return false;
}

bool VectorTextureCache::getTextureDims(const std::string& name, int /*licBoundaryMode*/, int& dimX, int& dimY, int& dimZ) const {
    // Deprecated mode param ignored — delegate to canonical.
    return getTextureDims(name, dimX, dimY, dimZ);
}

bool VectorTextureCache::getTextureDims(const std::string& name, int placement, int /*licBoundaryMode*/, const RenderMesh* mesh, int& dimX, int& dimY, int& dimZ) const {
    if (mesh) {
        const glm::vec3* d = nullptr;
        size_t cnt = 0;
        bool isCell = false;
        if (resolveField(*mesh, name, placement, d, cnt, isCell)) {
            const std::string key = makeKey(name, isCell);
            auto it = m_entries.find(key);
            if (it != m_entries.end()) {
                dimX = it->second.dimX;
                dimY = it->second.dimY;
                dimZ = it->second.dimZ;
                return true;
            }
        }
    }
    return getTextureDims(name, dimX, dimY, dimZ);
}

void VectorTextureCache::invalidate(const std::string& name) {
    for (int cell = 0; cell <= 1; ++cell) {
        const std::string k = makeKey(name, cell != 0);
        auto it = m_entries.find(k);
        if (it != m_entries.end()) {
            if (it->second.lruIt != m_lru.end())
                m_lru.erase(it->second.lruIt);
            m_entries.erase(it);
        }
        // Drop legacy "#B*" keys if present (old cache entries)
        for (int mode = 0; mode <= 2; ++mode) {
            const std::string legacy = name + (cell ? "#C" : "#P") + "#K" + "#B" + std::to_string(mode);
            auto jt = m_entries.find(legacy);
            if (jt != m_entries.end()) {
                if (jt->second.lruIt != m_lru.end())
                    m_lru.erase(jt->second.lruIt);
                m_entries.erase(jt);
            }
        }
    }
    // Also drop a legacy bare-name entry if present.
    auto bare = m_entries.find(name);
    if (bare != m_entries.end()) {
        if (bare->second.lruIt != m_lru.end())
            m_lru.erase(bare->second.lruIt);
        m_entries.erase(bare);
    }
}

void VectorTextureCache::invalidateAll() {
    m_entries.clear();
    m_lru.clear();
    for (int i = 0; i < 2; ++i) if (m_pbo[i].has()) m_pbo[i].reset();
    m_pboInitialized = false;
}

void VectorTextureCache::shutdown() {
    invalidateAll();
    for (int i = 0; i < 2; ++i) if (m_pbo[i].has()) m_pbo[i].reset();
    m_pboInitialized = false;
}