#pragma once

#include "render/foundation/gl_raii.h"
#include "core/mesh_loader.h"
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>

class VolumeTextureCache {
public:
    VolumeTextureCache() = default;

    // Returns a 3D texture handle for the named scalar field.
    // Builds and caches on first access. Returns 0 if unavailable.
    GLuint textureForField(const std::string& name, const RenderMesh* mesh,
                           const glm::vec3& boxMin, const glm::vec3& boxMax);

    void invalidate(const std::string& name);
    void invalidateAll();
    void shutdown();

private:
    struct Entry {
        GlTexture tex;
        int dimX = 0, dimY = 0, dimZ = 0;
    };

    GLuint buildTexture(const std::string& name, const RenderMesh* mesh,
                        const glm::vec3& boxMin, const glm::vec3& boxMax);
    const std::vector<float>* resolveField(const RenderMesh& mesh, const std::string& name) const;

    std::unordered_map<std::string, Entry> m_entries;
    GlBuffer m_pbo[2];
    int m_pboIndex = 0;
    bool m_pboInitialized = false;
};
