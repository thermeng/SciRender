#pragma once

#include "render/foundation/gl_raii.h"
#include "core/mesh_loader.h"
#include "core/post_fields.h"
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include <list>

class VectorTextureCache {
public:
    VectorTextureCache() = default;

    // licBoundaryMode is deprecated (always Clamp/GL_CLAMP_TO_EDGE); kept in signature for API compat but ignored.
    // snap: computed post-proc snapshot (null = mesh fields only).
    GLuint textureForField(const std::string& name, const RenderMesh* mesh,
                           const glm::vec3& boxMin, const glm::vec3& boxMax,
                           int placement = -1, int licBoundaryMode = 1,
                           const postfields::ComputedSnapshot* snap = nullptr);

    bool getTextureDims(const std::string& name, int& dimX, int& dimY, int& dimZ) const;
    // Strongest vector of the sampled field, or 0 when the field is unknown.
    float getMaxMagnitude(const std::string& name, int placement = -1, const RenderMesh* mesh = nullptr,
                          const postfields::ComputedSnapshot* snap = nullptr) const;
    // Deprecated licBoundaryMode variants — forward to canonical; mode ignored.
    bool getTextureDims(const std::string& name, int licBoundaryMode, int& dimX, int& dimY, int& dimZ) const;
    bool getTextureDims(const std::string& name, int placement, int licBoundaryMode, const RenderMesh* mesh, int& dimX, int& dimY, int& dimZ,
                        const postfields::ComputedSnapshot* snap = nullptr) const;

    void invalidate(const std::string& name);
    void invalidateAll();
    void shutdown();

    // Resolution of the LIC vector texture for a field carried by `mesh`.
    //
    // Shared by the cache (which compares it against the cached entry, so the
    // texture is not rebuilt every frame) and by the builder (which fills the
    // grid) - the two must never disagree.
    struct GridDims { int x = 0, y = 0, z = 0; };
    static GridDims chooseGridDims(const RenderMesh& mesh, size_t count, bool isCell);

    // CPU side of the LIC vector texture: samples `data` (the field) onto a
    // regular grid covering the mesh bounds. No GL involved, so it is directly
    // testable.
    bool buildGrid(const RenderMesh& mesh, const glm::vec3* data, size_t count, bool isCell,
                   std::vector<glm::vec3>& outGrid, int& outDimX, int& outDimY, int& outDimZ) const;

private:
    static constexpr size_t kMaxEntries = 4;

    struct Entry {
        GlTexture tex;
        int dimX = 0, dimY = 0, dimZ = 0;
        // Strongest vector of the sampled field (the LIC makes its magnitude
        // cut-off relative to it).
        float maxMagnitude = 0.0f;
        // lruIt is only meaningful while hasLru is true; comparing a
        // default-constructed (singular) iterator against m_lru.end() is UB.
        std::list<std::string>::iterator lruIt;
        bool hasLru = false;
    };

    void touch(Entry& e, const std::string& key);
    void evictIfNeeded();

    GLuint buildTexture(const std::string& name, const RenderMesh* mesh,
                        const glm::vec3& boxMin, const glm::vec3& boxMax,
                        int placement, int licBoundaryMode,
                        const postfields::ComputedSnapshot* snap);
    bool resolveField(const RenderMesh& mesh, const std::string& name, int placement,
                      const glm::vec3*& outData, size_t& outCount, bool& outIsCell,
                      const postfields::ComputedSnapshot* snap) const;

    static std::string makeKey(const std::string& name, bool isCell);
    // Deprecated compat — ignores mode, forwards to canonical makeKey(name,isCell)
    static std::string makeKey(const std::string& name, bool isCell, int licBoundaryMode);

    std::unordered_map<std::string, Entry> m_entries;
    std::list<std::string> m_lru;
    GlBuffer m_pbo[2];
    int m_pboIndex = 0;
    bool m_pboInitialized = false;
};