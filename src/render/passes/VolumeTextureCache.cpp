#include "render/passes/VolumeTextureCache.h"
#include "core/FieldStore.h"
#include <glad/gl.h>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

void configureTex3D(GLuint tex, GLenum minFilter) {
    glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, minFilter);
    glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, minFilter);
}

} // namespace

bool VolumePlaneRanges::range(int axis, float frac, float& outMin, float& outMax) const {
    if (axis < 0 || axis > 2) return false;
    const auto& mn = minValue[static_cast<std::size_t>(axis)];
    const auto& mx = maxValue[static_cast<std::size_t>(axis)];
    if (mn.empty() || mn.size() != mx.size()) return false;

    // Voxel i is centred at uvw = (i + 0.5) / n, so the plane at `frac` maps to
    // the nearest voxel centre.
    const int n = static_cast<int>(mn.size());
    int slice = static_cast<int>(std::lround(static_cast<double>(frac) * n - 0.5));
    slice = std::clamp(slice, 0, n - 1);
    if (!(mx[static_cast<std::size_t>(slice)] >= mn[static_cast<std::size_t>(slice)])) return false;
    outMin = mn[static_cast<std::size_t>(slice)];
    outMax = mx[static_cast<std::size_t>(slice)];
    return true;
}

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

VolumeTexture VolumeTextureCache::textureFor(const std::string& name, const RenderMesh* mesh,
                                             const std::shared_ptr<const volume::DerivedGrid>& derived,
                                             const glm::vec3& boxMin, const glm::vec3& boxMax,
                                             int placement,
                                             const postfields::ComputedSnapshot* snap) {
    VolumeTexture out;
    if (!mesh || name.empty()) return out;

    if (!derived || derived->empty()) {
        // Structured grids keep the existing unmasked path.
        out.scalar = textureForField(name, mesh, boxMin, boxMax, placement, snap);
        out.dimX = mesh->gridDimX;
        out.dimY = mesh->gridDimY;
        out.dimZ = mesh->gridDimZ;
        return out;
    }

    const std::string key = name + (placement == 1 ? "#cell" : "#vert") + "#derived";
    auto it = m_entries.find(key);
    if (it != m_entries.end() && it->second.derived && it->second.tex.has()
        && it->second.derivedOwner.lock().get() == derived.get()
        && it->second.dimX == derived->dimX && it->second.dimY == derived->dimY
        && it->second.dimZ == derived->dimZ
        && it->second.placement == placement) {
        out.scalar = it->second.tex.get();
        out.occupancy = it->second.occupancy.get();
        out.dimX = it->second.dimX;
        out.dimY = it->second.dimY;
        out.dimZ = it->second.dimZ;
        out.planes = it->second.planes;
        return out;
    }

    const std::size_t nCells = mesh->cellOffsets.size();
    const std::vector<float>* values = nullptr;
    bool cellPlacement = false;
    if (placement == 1 && nCells > 0) {
        // The exact per-cell array is what makes a cell field exact; the
        // point-sized extrapolated copy is the fallback.
        float mn = 0.0f, mx = 1.0f;
        const std::vector<float>* cells = FieldStore::volumeScalarData(
            *mesh, name, mn, mx, FieldStore::Placement::CellCenter, snap);
        if (cells && cells->size() == nCells) {
            values = cells;
            cellPlacement = true;
        }
    }
    if (!values) {
        values = resolveField(*mesh, name, 0, snap);
        if (!values || values->empty()) return out;
    }

    volume::ScalarGrid scattered;
    volume::scatter(*mesh, *derived, values, cellPlacement, scattered);
    if (scattered.empty()) return out;

    const std::size_t expected = static_cast<std::size_t>(derived->dimX)
                               * static_cast<std::size_t>(derived->dimY)
                               * static_cast<std::size_t>(derived->dimZ);
    if (scattered.value.size() != expected || scattered.occupancy.size() != expected) return out;

    GLuint scalarRaw = 0;
    glCreateTextures(GL_TEXTURE_3D, 1, &scalarRaw);
    glTextureStorage3D(scalarRaw, 1, GL_R32F, derived->dimX, derived->dimY, derived->dimZ);
    configureTex3D(scalarRaw, GL_LINEAR);

    const std::size_t bytes = expected * sizeof(float);
    if (!m_pboInitialized) {
        for (int i = 0; i < 2; ++i)
            if (!m_pbo[i].has()) glCreateBuffers(1, m_pbo[i].ptr());
        m_pboInitialized = true;
    }
    bool uploaded = false;
    if (m_pbo[0].has() && bytes > 0) {
        GlBuffer& pbo = m_pbo[m_pboIndex];
        m_pboIndex = (m_pboIndex + 1) % 2;
        glNamedBufferData(pbo, bytes, nullptr, GL_STREAM_DRAW);
        void* ptr = glMapNamedBufferRange(pbo, 0, bytes, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
        if (ptr) {
            std::memcpy(ptr, scattered.value.data(), bytes);
            glUnmapNamedBuffer(pbo);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo.get());
            glTextureSubImage3D(scalarRaw, 0, 0, 0, 0, derived->dimX, derived->dimY, derived->dimZ,
                                GL_RED, GL_FLOAT, nullptr);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            uploaded = true;
        }
    }
    if (!uploaded) {
        glTextureSubImage3D(scalarRaw, 0, 0, 0, 0, derived->dimX, derived->dimY, derived->dimZ,
                            GL_RED, GL_FLOAT, scattered.value.data());
    }

    // Occupancy mask: one byte per voxel, nearest-filtered so the volume/slice
    // shaders can discard empty space exactly instead of blurring the boundary.
    GLuint occRaw = 0;
    glCreateTextures(GL_TEXTURE_3D, 1, &occRaw);
    glTextureStorage3D(occRaw, 1, GL_R8, derived->dimX, derived->dimY, derived->dimZ);
    configureTex3D(occRaw, GL_NEAREST);
    glTextureSubImage3D(occRaw, 0, 0, 0, 0, derived->dimX, derived->dimY, derived->dimZ,
                        GL_RED, GL_UNSIGNED_BYTE, scattered.occupancy.data());

    auto planes = std::make_shared<VolumePlaneRanges>();
    planes->minValue = std::move(scattered.planeMin);
    planes->maxValue = std::move(scattered.planeMax);

    Entry& e = m_entries[key];
    e.tex.reset(scalarRaw);
    e.occupancy.reset(occRaw);
    e.dimX = derived->dimX;
    e.dimY = derived->dimY;
    e.dimZ = derived->dimZ;
    e.placement = placement;
    e.derived = true;
    e.derivedOwner = derived;
    e.planes = planes;

    out.scalar = scalarRaw;
    out.occupancy = occRaw;
    out.dimX = e.dimX;
    out.dimY = e.dimY;
    out.dimZ = e.dimZ;
    out.planes = std::move(planes);
    return out;
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
    configureTex3D(raw, GL_LINEAR);

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
    e.occupancy.reset();
    e.dimX = dimX; e.dimY = dimY; e.dimZ = dimZ; e.placement = placement;
    e.derived = false;
    e.derivedOwner.reset();
    e.planes.reset();
    return raw;
}

void VolumeTextureCache::invalidate(const std::string& name) {
    m_entries.erase(name);
    m_entries.erase(name + "#cell");
    m_entries.erase(name + "#vert");
    m_entries.erase(name + "#cell#derived");
    m_entries.erase(name + "#vert#derived");
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
