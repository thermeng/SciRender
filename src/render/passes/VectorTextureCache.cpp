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
    if (!vf.data || vf.count == 0) {
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
    int dimX = mesh.gridDimX, dimY = mesh.gridDimY, dimZ = mesh.gridDimZ;
    int numVerts = static_cast<int>(mesh.vertices.size() / 3);
    int limit = std::min(numVerts, static_cast<int>(count));

    if (dimX > 0 && dimY > 0 && dimZ > 0 && dimX * dimY * dimZ == limit && !isCell) {
        outGrid.resize(static_cast<size_t>(dimX) * dimY * dimZ);
        memcpy(outGrid.data(), data, outGrid.size() * sizeof(glm::vec3));
        outDimX = dimX; outDimY = dimY; outDimZ = dimZ;
        return true;
    }

    if (dimX > 0 && dimY > 0 && dimZ > 0 && isCell) {
        int cDimX = (dimX > 1) ? dimX - 1 : 1;
        int cDimY = (dimY > 1) ? dimY - 1 : 1;
        int cDimZ = (dimZ > 1) ? dimZ - 1 : 1;
        if (cDimX * cDimY * cDimZ == static_cast<int>(count)) {
            outGrid.resize(static_cast<size_t>(cDimX) * cDimY * cDimZ);
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
    outGrid.resize(total, glm::vec3(0.0f));

    float minX = static_cast<float>(mesh.bounds.minX), maxX = static_cast<float>(mesh.bounds.maxX);
    float minY = static_cast<float>(mesh.bounds.minY), maxY = static_cast<float>(mesh.bounds.maxY);
    float minZ = static_cast<float>(mesh.bounds.minZ), maxZ = static_cast<float>(mesh.bounds.maxZ);
    float rangeX = maxX - minX, rangeY = maxY - minY, rangeZ = maxZ - minZ;
    if (rangeX < 1e-9f) rangeX = 1.0f;
    if (rangeY < 1e-9f) rangeY = 1.0f;
    if (rangeZ < 1e-9f) rangeZ = 1.0f;

    const float* verts = mesh.vertices.data();
    const glm::vec3* cellPos = nullptr;
    bool useCellCenters = false;
    if (isCell && !mesh.cellCenters.empty() && static_cast<int>(mesh.cellCenters.size()) == static_cast<int>(count)) {
        cellPos = mesh.cellCenters.data();
        useCellCenters = true;
    }
    int stride = 1;
    if (limit > 8000) stride = (limit + 7999) / 8000;
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

        float bestDist = std::numeric_limits<float>::max();
        glm::vec3 bestVec(0.0f);
        for (int v = 0; v < limit; v += stride) {
            float vx, vy, vz;
            if (useCellCenters) {
                vx = cellPos[v].x;
                vy = cellPos[v].y;
                vz = cellPos[v].z;
            } else {
                vx = verts[v * 3 + 0];
                vy = verts[v * 3 + 1];
                vz = verts[v * 3 + 2];
            }
            float dx = vx - px, dy = vy - py, dz = vz - pz;
            float d = dx * dx + dy * dy + dz * dz;
            if (d < bestDist) {
                bestDist = d;
                bestVec = data[v];
            }
        }
        outGrid[i] = bestVec;
    }
    return true;
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
    (void)licBoundaryMode;
    if (!mesh || name.empty()) return 0;

    const glm::vec3* data = nullptr;
    size_t count = 0;
    bool isCell = false;
    if (!resolveField(*mesh, name, placement, data, count, isCell)) return 0;

    int dimX = mesh->gridDimX, dimY = mesh->gridDimY, dimZ = mesh->gridDimZ;
    int numVerts = static_cast<int>(mesh->vertices.size() / 3);
    int limit = std::min(numVerts, static_cast<int>(count));
    int expectedDimX, expectedDimY, expectedDimZ;
    if (!isCell && dimX > 0 && dimY > 0 && dimZ > 0 && dimX * dimY * dimZ == limit) {
        expectedDimX = dimX; expectedDimY = dimY; expectedDimZ = dimZ;
    } else if (isCell && dimX > 0 && dimY > 0 && dimZ > 0) {
        int cDimX = (dimX > 1) ? dimX - 1 : 1;
        int cDimY = (dimY > 1) ? dimY - 1 : 1;
        int cDimZ = (dimZ > 1) ? dimZ - 1 : 1;
        if (cDimX * cDimY * cDimZ == static_cast<int>(count)) {
            expectedDimX = cDimX; expectedDimY = cDimY; expectedDimZ = cDimZ;
        } else {

            float density = std::cbrt(static_cast<float>(std::max(numVerts,1)));
            int r = std::clamp(static_cast<int>(std::ceil(density)), 16, 32);
            if (numVerts > 50000) r = 32;
            if (numVerts > 200000) r = 24;
            expectedDimX = expectedDimY = expectedDimZ = r;
        }
    } else {
        float density = std::cbrt(static_cast<float>(std::max(numVerts,1)));
        int r = std::clamp(static_cast<int>(std::ceil(density)), 16, 32);
        if (numVerts > 50000) r = 32;
        if (numVerts > 200000) r = 24;
        expectedDimX = expectedDimY = expectedDimZ = r;
    }
    std::string key = name + (isCell ? "#C" : "#P") + "#K";
    auto it = m_entries.find(key);
    if (it != m_entries.end() && it->second.tex.has()
        && it->second.dimX == expectedDimX && it->second.dimY == expectedDimY && it->second.dimZ == expectedDimZ) {
        touch(it->second, key);
        return it->second.tex.get();
    }

    return buildTexture(name, mesh, boxMin, boxMax, placement, licBoundaryMode);
}

void VectorTextureCache::mirrorPadGrid(const std::vector<glm::vec3>& src, int sx, int sy, int sz,
                                     std::vector<glm::vec3>& dst, int& dx, int& dy, int& dz) {
    int px = (sx > 1) ? 1 : 0;
    int py = (sy > 1) ? 1 : 0;
    int pz = (sz > 1) ? 1 : 0;
    dx = sx + 2 * px; dy = sy + 2 * py; dz = sz + 2 * pz;
    dst.resize(static_cast<size_t>(dx) * dy * dz);
    auto srcAt = [&](int x, int y, int z) -> glm::vec3 {
        x = std::clamp(x, 0, sx - 1); y = std::clamp(y, 0, sy - 1); z = std::clamp(z, 0, sz - 1);
        return src[static_cast<size_t>(x + y * sx + z * sx * sy)];
    };
    for (int z = 0; z < dz; ++z)
        for (int y = 0; y < dy; ++y)
            for (int x = 0; x < dx; ++x) {
                int sx0 = x - px; int sy0 = y - py; int sz0 = z - pz;


                if (sx0 < 0) sx0 = -sx0;
                else if (sx0 >= sx) sx0 = 2 * sx - sx0 - 2;

                if (sy0 < 0) sy0 = -sy0;
                else if (sy0 >= sy) sy0 = 2 * sy - sy0 - 2;

                if (sz0 < 0) sz0 = -sz0;
                else if (sz0 >= sz) sz0 = 2 * sz - sz0 - 2;

                sx0 = std::clamp(sx0, 0, sx - 1); sy0 = std::clamp(sy0, 0, sy - 1); sz0 = std::clamp(sz0, 0, sz - 1);
                dst[static_cast<size_t>(x + y * dx + z * dx * dy)] = srcAt(sx0, sy0, sz0);
            }
}

GLuint VectorTextureCache::buildTexture(const std::string& name, const RenderMesh* mesh,
                                         const glm::vec3& boxMin, const glm::vec3& boxMax,
                                         int placement, int licBoundaryMode) {
    (void)boxMin; (void)boxMax; (void)licBoundaryMode;
    const glm::vec3* data = nullptr;
    size_t count = 0;
    bool isCell = false;
    if (!resolveField(*mesh, name, placement, data, count, isCell)) return 0;

    std::vector<glm::vec3> grid;
    int dimX = 0, dimY = 0, dimZ = 0;
    if (!buildGrid(*mesh, data, count, isCell, grid, dimX, dimY, dimZ)) return 0;




    int texDimX = dimX, texDimY = dimY, texDimZ = dimZ;
    const glm::vec3* texData = grid.data();

    GLuint raw = 0;
    glCreateTextures(GL_TEXTURE_3D, 1, &raw);
    glTextureStorage3D(raw, 1, GL_RGB32F, texDimX, texDimY, texDimZ);
    glTextureParameteri(raw, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTextureParameteri(raw, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTextureParameteri(raw, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_BORDER);
    float border[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glTextureParameterfv(raw, GL_TEXTURE_BORDER_COLOR, border);
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
            GLuint prevPbo = 0;
            glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, (GLint*)&prevPbo);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo.get());
            glTextureSubImage3D(raw, 0, 0, 0, 0, texDimX, texDimY, texDimZ, GL_RGB, GL_FLOAT, nullptr);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, prevPbo);
        } else {
            glTextureSubImage3D(raw, 0, 0, 0, 0, texDimX, texDimY, texDimZ, GL_RGB, GL_FLOAT, texData);
        }
    } else {
        glTextureSubImage3D(raw, 0, 0, 0, 0, texDimX, texDimY, texDimZ, GL_RGB, GL_FLOAT, texData);
    }

    std::string key = name + (isCell ? "#C" : "#P") + "#K";
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
    for (const auto& kv : m_entries) {
        const std::string& k = kv.first;
        if (k.size() > name.size() && k.compare(0, name.size(), name) == 0 && k[name.size()] == '#') {
            dimX = kv.second.dimX;
            dimY = kv.second.dimY;
            dimZ = kv.second.dimZ;
            return true;
        }
    }
    return false;
}

bool VectorTextureCache::getTextureDims(const std::string& name, int licBoundaryMode, int& dimX, int& dimY, int& dimZ) const {
    (void)licBoundaryMode;
    std::string suffix = "#K";
    for (const auto& kv : m_entries) {
        const std::string& k = kv.first;
        if (k.size() >= name.size() + 3 && k.compare(0, name.size(), name) == 0) {
            if (k.size() >= suffix.size() && k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0) {
                dimX = kv.second.dimX;
                dimY = kv.second.dimY;
                dimZ = kv.second.dimZ;
                return true;
            }
        }
    }
    return getTextureDims(name, dimX, dimY, dimZ);
}

bool VectorTextureCache::getTextureDims(const std::string& name, int placement, int licBoundaryMode, const RenderMesh* mesh, int& dimX, int& dimY, int& dimZ) const {
    (void)licBoundaryMode;
    if (mesh) {
        const glm::vec3* d = nullptr;
        size_t cnt = 0;
        bool isCell = false;
        if (resolveField(*mesh, name, placement, d, cnt, isCell)) {
            std::string key = name + (isCell ? "#C" : "#P") + "#K";
            auto it = m_entries.find(key);
            if (it != m_entries.end()) {
                dimX = it->second.dimX;
                dimY = it->second.dimY;
                dimZ = it->second.dimZ;
                return true;
            }
        }
    }
    return getTextureDims(name, licBoundaryMode, dimX, dimY, dimZ);
}

void VectorTextureCache::invalidate(const std::string& name) {
    auto eraseKey = [&](const std::string& k) {
        auto it = m_entries.find(k);
        if (it != m_entries.end()) {
            if (it->second.lruIt != m_lru.end())
                m_lru.erase(it->second.lruIt);
            m_entries.erase(it);
        }
    };
    eraseKey(name);
    eraseKey(name + "#C");
    eraseKey(name + "#P");
    eraseKey(name + "#C#K");
    eraseKey(name + "#P#K");
    eraseKey(name + "#C#M");
    eraseKey(name + "#P#M");
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