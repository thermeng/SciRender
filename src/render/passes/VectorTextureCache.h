#pragma once

#include "render/foundation/gl_raii.h"
#include "core/mesh_loader.h"
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include <list>

class VectorTextureCache {
public:
    VectorTextureCache() = default;

    GLuint textureForField(const std::string& name, const RenderMesh* mesh,
                           const glm::vec3& boxMin, const glm::vec3& boxMax,
                           int placement = -1, int licBoundaryMode = 0);

    bool getTextureDims(const std::string& name, int& dimX, int& dimY, int& dimZ) const;
    bool getTextureDims(const std::string& name, int licBoundaryMode, int& dimX, int& dimY, int& dimZ) const;
    bool getTextureDims(const std::string& name, int placement, int licBoundaryMode, const RenderMesh* mesh, int& dimX, int& dimY, int& dimZ) const;

    void invalidate(const std::string& name);
    void invalidateAll();
    void shutdown();

private:
    static constexpr size_t kMaxEntries = 4;

    struct Entry {
        GlTexture tex;
        int dimX = 0, dimY = 0, dimZ = 0;
        std::list<std::string>::iterator lruIt;
    };

    void touch(Entry& e, const std::string& key);
    void evictIfNeeded();

    GLuint buildTexture(const std::string& name, const RenderMesh* mesh,
                        const glm::vec3& boxMin, const glm::vec3& boxMax,
                        int placement, int licBoundaryMode);
    bool resolveField(const RenderMesh& mesh, const std::string& name, int placement,
                      const glm::vec3*& outData, size_t& outCount, bool& outIsCell) const;
    bool buildGrid(const RenderMesh& mesh, const glm::vec3* data, size_t count, bool isCell,
                   std::vector<glm::vec3>& outGrid, int& outDimX, int& outDimY, int& outDimZ) const;

    static void mirrorPadGrid(const std::vector<glm::vec3>& src, int sx, int sy, int sz,
                              std::vector<glm::vec3>& dst, int& dx, int& dy, int& dz);

    std::unordered_map<std::string, Entry> m_entries;
    std::list<std::string> m_lru;
    GlBuffer m_pbo[2];
    int m_pboIndex = 0;
    bool m_pboInitialized = false;
};