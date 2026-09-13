#include "render/passes/VolumeTextureCache.h"
#include "core/FieldStore.h"
#include <glad/gl.h>
#include <cstring>
#include <algorithm>
#include <limits>

const std::vector<float>* VolumeTextureCache::resolveField(const RenderMesh& mesh, const std::string& name, int placement,
                                                           const postfields::ComputedSnapshot* snap) const {
    float mn, mx;
    auto p = FieldStore::toPlacement(placement);
    // Volume path must not return extrapolated point copy for CellCenter; use volume-aware resolver.
    // Snapshot fields are point-sized (nodeCount == dimX*dimY*dimZ) so either path returns correct size.
    if (p == FieldStore::Placement::CellCenter)
        return FieldStore::volumeScalarData(mesh, name, mn, mx, p, snap);
    return FieldStore::scalarData(mesh, name, mn, mx, p, snap);
}

GLuint VolumeTextureCache::textureForField(const std::string& name, const RenderMesh* mesh,
                                             const glm::vec3& boxMin, const glm::vec3& boxMax, int placement,
                                             const postfields::ComputedSnapshot* snap) {
    if (!mesh || name.empty()) return 0;

    int dimX = mesh->gridDimX, dimY = mesh->gridDimY, dimZ = mesh->gridDimZ;
    if (dimX <= 0 || dimY <= 0 || dimZ <= 0) return 0;

    // Cache key includes placement so Vertex vs Cell textures are distinct.
    std::string key = name + (placement == 1 ? "#cell" : "#vert");
    auto it = m_entries.find(key);
    if (it != m_entries.end() && it->second.tex.has()
        && it->second.dimX == dimX && it->second.dimY == dimY && it->second.dimZ == dimZ
        && it->second.placement == placement) {
        return it->second.tex.get();
    }

    return buildTexture(name, mesh, boxMin, boxMax, placement, snap);
}

GLuint VolumeTextureCache::buildTexture(const std::string& name, const RenderMesh* mesh,
                                         const glm::vec3& boxMin, const glm::vec3& boxMax, int placement,
                                         const postfields::ComputedSnapshot* snap) {
    const std::vector<float>* data = resolveField(*mesh, name, placement, snap);
    if (!data || data->empty()) return 0;

    int dimX = mesh->gridDimX, dimY = mesh->gridDimY, dimZ = mesh->gridDimZ;
    if (dimX <= 0 || dimY <= 0 || dimZ <= 0) return 0;

    GLuint raw = 0;
    glCreateTextures(GL_TEXTURE_3D, 1, &raw);
    glTextureStorage3D(raw, 1, GL_R32F, dimX, dimY, dimZ);
    glTextureParameteri(raw, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(raw, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(raw, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTextureParameteri(raw, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(raw, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    size_t bytes = data->size() * sizeof(float);
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
            std::memcpy(ptr, data->data(), bytes);
            glUnmapNamedBuffer(pbo);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo.get());
            glTextureSubImage3D(raw, 0, 0, 0, 0, dimX, dimY, dimZ, GL_RED, GL_FLOAT, nullptr);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        } else {
            glTextureSubImage3D(raw, 0, 0, 0, 0, dimX, dimY, dimZ, GL_RED, GL_FLOAT, data->data());
        }
    } else {
        glTextureSubImage3D(raw, 0, 0, 0, 0, dimX, dimY, dimZ, GL_RED, GL_FLOAT, data->data());
    }

    std::string key = name + (placement == 1 ? "#cell" : "#vert");
    Entry& e = m_entries[key];
    e.tex.reset(raw);
    e.dimX = dimX; e.dimY = dimY; e.dimZ = dimZ; e.placement = placement;
    return raw;
}

void VolumeTextureCache::invalidate(const std::string& name) {
    m_entries.erase(name);
    m_entries.erase(name + "#cell");
    m_entries.erase(name + "#vert");
}

void VolumeTextureCache::invalidateAll() {
    m_entries.clear();
    for (int i = 0; i < 2; ++i) if (m_pbo[i].has()) m_pbo[i].reset();
    m_pboInitialized = false;
}

void VolumeTextureCache::shutdown() {
    invalidateAll();
    for (int i = 0; i < 2; ++i) if (m_pbo[i].has()) m_pbo[i].reset();
    m_pboInitialized = false;
}
