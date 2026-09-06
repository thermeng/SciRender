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

    // licBoundaryMode is deprecated (always Repeat/GL_REPEAT); kept in signature for API compat but ignored.
    GLuint textureForField(const std::string& name, const RenderMesh* mesh,
                           const glm::vec3& boxMin, const glm::vec3& boxMax,
                           int placement = -1, int licBoundaryMode = 1);

    bool getTextureDims(const std::string& name, int& dimX, int& dimY, int& dimZ) const;
    // Deprecated licBoundaryMode variants — forward to canonical; mode ignored.
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

    static std::string makeKey(const std::string& name, bool isCell);
    // Deprecated compat — ignores mode, forwards to canonical makeKey(name,isCell)
    static std::string makeKey(const std::string& name, bool isCell, int licBoundaryMode);

    std::unordered_map<std::string, Entry> m_entries;
    std::list<std::string> m_lru;
    GlBuffer m_pbo[2];
    int m_pboIndex = 0;
    bool m_pboInitialized = false;
};