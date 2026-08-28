#include "render/passes/VolumeTextureCache.h"
#include "core/FieldResolver.h"
#include <glad/gl.h>
#include <cstring>
#include <algorithm>
#include <limits>

const std::vector<float>* VolumeTextureCache::resolveField(const RenderMesh& mesh, const std::string& name) const {
    float mn, mx;
    return FieldResolver::scalarData(mesh, name, mn, mx);
}

GLuint VolumeTextureCache::textureForField(const std::string& name, const RenderMesh* mesh,
                                            const glm::vec3& boxMin, const glm::vec3& boxMax) {
    if (!mesh || name.empty()) return 0;

    int dimX = mesh->gridDimX, dimY = mesh->gridDimY, dimZ = mesh->gridDimZ;
    if (dimX <= 0 || dimY <= 0 || dimZ <= 0) return 0;

    auto it = m_entries.find(name);
    if (it != m_entries.end() && it->second.tex.has()
        && it->second.dimX == dimX && it->second.dimY == dimY && it->second.dimZ == dimZ) {
        return it->second.tex.get();
    }

    return buildTexture(name, mesh, boxMin, boxMax);
}

GLuint VolumeTextureCache::buildTexture(const std::string& name, const RenderMesh* mesh,
                                        const glm::vec3& boxMin, const glm::vec3& boxMax) {
    const std::vector<float>* data = resolveField(*mesh, name);
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

    Entry& e = m_entries[name];
    e.tex.reset(raw);
    e.dimX = dimX; e.dimY = dimY; e.dimZ = dimZ;
    return raw;
}

void VolumeTextureCache::invalidate(const std::string& name) {
    m_entries.erase(name);
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
