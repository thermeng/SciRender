#pragma once

#include "render/foundation/gl_raii.h"
#include "core/mesh_loader.h"
#include "core/post_fields.h"
#include "core/unstructured_volume.h"
#include <glm/glm.hpp>
#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Per-axis slice ranges over the *occupied* voxels of a derived grid, captured
// when the field is scattered. The slice overlay needs a range that ignores the
// empty space around an unstructured mesh; the structured path scans its field
// directly instead.
struct VolumePlaneRanges {
    std::array<std::vector<float>, 3> minValue;
    std::array<std::vector<float>, 3> maxValue;

    // Range of the slice nearest `frac` (0..1 along the axis). False when the
    // slice is empty, i.e. the plane misses the mesh.
    bool range(int axis, float frac, float& outMin, float& outMax) const;
};

// One volume texture set. Structured grids have no occupancy mask (occupancy
// stays 0 and the shaders take the unmasked path).
struct VolumeTexture {
    GLuint scalar = 0;    // R32F, dimX*dimY*dimZ
    GLuint occupancy = 0; // R8 mask, 0 when there is none
    int dimX = 0, dimY = 0, dimZ = 0;
    std::shared_ptr<const VolumePlaneRanges> planes; // derived grids only

    bool valid() const { return scalar != 0; }
};

class VolumeTextureCache {
public:
    VolumeTextureCache() = default;

    // Structured grids: samples the mesh's IJK field.
    // Unstructured (derived != nullptr): scatters the field over the derived
    // voxel grid and uploads the occupancy mask with it.
    // placement: 0=Vertex (point), 1=Cell Center (cell) — mirrors scalarPlacement.
    // snap: computed post-proc snapshot (null = mesh fields only).
    // mesh is shared so a cache entry can pin the exact field array it was
    // built from: RenderMesh is immutable once published, so (mesh identity,
    // data pointer, size) is a sound content token and a stale entry cannot
    // survive a missed invalidation.
    VolumeTexture textureFor(const std::string& name,
                             const std::shared_ptr<const RenderMesh>& mesh,
                             const std::shared_ptr<const volume::DerivedGrid>& derived,
                             const glm::vec3& boxMin, const glm::vec3& boxMax,
                             int placement = 0,
                             const postfields::ComputedSnapshot* snap = nullptr);

    // Returns a 3D texture handle for the named scalar field (structured path).
    // Builds and caches on first access. Returns 0 if unavailable.
    GLuint textureForField(const std::string& name, const RenderMesh* mesh,
                           const glm::vec3& boxMin, const glm::vec3& boxMax, int placement = 0,
                           const postfields::ComputedSnapshot* snap = nullptr);

    void invalidate(const std::string& name);
    void invalidateAll();
    void shutdown();

private:
    struct Entry {
        GlTexture tex;
        GlTexture occupancy;
        int dimX = 0, dimY = 0, dimZ = 0;
        int placement = 0;
        bool derived = false;
        // Identifies the derived grid the entry was built from. A new grid
        // (topology change) expires the entry even when the dims match.
        std::weak_ptr<const volume::DerivedGrid> derivedOwner;
        // Content token for the scattered field: the mesh keeps the array
        // alive (immutable), so pointer + size equality means same contents.
        // snapToken covers snapshot-backed fields (snapshot swaps are
        // invalidated by the renderer; the pointer guards the rest).
        std::shared_ptr<const RenderMesh> meshOwner;
        const postfields::ComputedSnapshot* snapToken = nullptr;
        const void* fieldData = nullptr;
        std::size_t fieldCount = 0;
        std::shared_ptr<const VolumePlaneRanges> planes;
    };

    GLuint buildTexture(const std::string& name, const RenderMesh* mesh,
                        const glm::vec3& boxMin, const glm::vec3& boxMax, int placement,
                        const postfields::ComputedSnapshot* snap);
    const std::vector<float>* resolveField(const RenderMesh& mesh, const std::string& name, int placement,
                                           const postfields::ComputedSnapshot* snap) const;
    // Single upload path for 3D textures (scalar R32F and occupancy R8):
    // double-buffered PBO with a direct-upload fallback.
    void upload3D(GLuint tex, int dimX, int dimY, int dimZ, GLenum format, GLenum type,
                  const void* data, std::size_t bytes);

    std::unordered_map<std::string, Entry> m_entries;
    GlBuffer m_pbo[2];
    int m_pboIndex = 0;
    bool m_pboInitialized = false;
};
