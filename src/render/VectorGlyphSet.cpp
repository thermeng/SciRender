#include "render/VectorGlyphSet.h"
#include "core/mesh_loader.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <limits>
#include <unordered_set>

void buildUnitArrow(std::vector<float>& verts, std::vector<float>& norms, std::vector<unsigned int>& idx) {
    const int SEG = 8;
    const float rs = 0.04f, rh = 0.12f, yHead = 0.75f;
    for (int i = 0; i < SEG; ++i) {
        float a0 = (i / (float)SEG) * 2.0f * 3.14159265f;
        float a1 = ((i + 1) / (float)SEG) * 2.0f * 3.14159265f;
        int b = (int)verts.size() / 3;
        auto P = [&](float y, float r, float ang) { verts.push_back(cosf(ang) * r); verts.push_back(y); verts.push_back(sinf(ang) * r); };
        auto N = [&](float ang) { norms.push_back(cosf(ang)); norms.push_back(0.0f); norms.push_back(sinf(ang)); };
        P(0, rs, a0); N(a0); P(0, rs, a1); N(a1); P(yHead, rs, a1); N(a1); P(yHead, rs, a0); N(a0);
        idx.insert(idx.end(), { (unsigned)b, (unsigned)b + 1, (unsigned)b + 2, (unsigned)b, (unsigned)b + 2, (unsigned)b + 3 });
    }
    for (int i = 0; i < SEG; ++i) {
        float a0 = (i / (float)SEG) * 2.0f * 3.14159265f;
        float a1 = ((i + 1) / (float)SEG) * 2.0f * 3.14159265f;
        int b = (int)verts.size() / 3;
        auto P = [&](float y, float r, float ang) { verts.push_back(cosf(ang) * r); verts.push_back(y); verts.push_back(sinf(ang) * r); };
        glm::vec3 n0 = glm::normalize(glm::vec3(cosf(a0) * rh, 0.25f, sinf(a0) * rh));
        glm::vec3 n1 = glm::normalize(glm::vec3(cosf(a1) * rh, 0.25f, sinf(a1) * rh));
        P(yHead, rh, a0); norms.insert(norms.end(), { n0.x, n0.y, n0.z });
        P(yHead, rh, a1); norms.insert(norms.end(), { n1.x, n1.y, n1.z });
        P(1.0f, 0.0f, 0.0f); norms.insert(norms.end(), { n0.x, n0.y, n0.z });
        idx.insert(idx.end(), { (unsigned)b, (unsigned)b + 1, (unsigned)b + 2 });
    }
}

void VectorGlyphSet::teardownGL() {
    if (vao) glDeleteVertexArrays(1, &vao);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (nbo) glDeleteBuffers(1, &nbo);
    if (ebo) glDeleteBuffers(1, &ebo);
    if (instVBO) glDeleteBuffers(1, &instVBO);
    vao = 0; vbo = 0; nbo = 0; ebo = 0; instVBO = 0;
    glyphIndexCount = 0; instanceCount = 0;
}

void VectorGlyphSet::shutdown() {
    teardownGL();
}

void VectorGlyphSet::rebuild(const RenderMesh& mesh, int stride, const std::string& fieldName, int magTransform) {
    teardownGL();
    (void)magTransform; // Range is stored raw; the shader applies txMag() itself.

    if (mesh.pointVectorsData.empty()) return;

    // Field selection: prefer the requested field, then the mesh's active field,
    // then the first available. Validate each candidate against the actual field
    // table (vectorFieldData) rather than assuming presence, so an unknown name
    // falls back to a valid field instead of silently rendering nothing.
    size_t count = 0;
    const glm::vec3* data = nullptr;
    auto tryField = [&](const std::string& name) -> bool {
        if (name.empty()) return false;
        size_t c = 0;
        const glm::vec3* d = mesh.vectorFieldData(name, c);
        if (d && c > 0) { data = d; count = c; return true; }
        return false;
    };
    if (!tryField(fieldName) && !tryField(mesh.vectorName) &&
        !(mesh.availableVectorNames.empty() ? false : tryField(mesh.availableVectorNames.front()))) {
        return;
    }

    int numPts = static_cast<int>(mesh.vertices.size() / 3);
    // A field's run may be shorter than the vertex count for malformed/partial
    // data; never read past the valid run (or past the vertex array).
    const int limit = std::min(numPts, static_cast<int>(count));
    stride = std::max(1, stride);
    // Track min/max magnitude across the FULL field (every point), not just the
    // strided render sample. magMin/magMax also drive the vector colorbar legend
    // (renderer.cpp), so using only the subsampled subset made glyph colors and
    // the legend scale disagree when vectorStride > 1.
    float mMin = std::numeric_limits<float>::max();
    float mMax = -std::numeric_limits<float>::max();
    for (int i = 0; i < limit; ++i) {
        float dx = data[i].x, dy = data[i].y, dz = data[i].z;
        float m = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!std::isfinite(m)) continue;
        if (m < mMin) mMin = m;
        if (m > mMax) mMax = m;
    }
    std::vector<float> inst;
    // Datasets often carry duplicate coincident point coordinates (distinct point
    // indices at the same xyz). Without dedup the glyph builder emits one arrow
    // per index, so stacked arrows appear at each shared location. Collapse
    // instances that share a quantized origin to a single arrow. The quantum is
    // relative to the mesh extent so it stays robust across tiny and huge meshes
    // (an absolute quantum wrongly merges/splits points at extreme scales).
    // Note: only the first vector seen at a shared location is kept.
    const float extent = static_cast<float>(mesh.bounds.extent);
    const float q = std::max(extent * 1e-5f, 1e-20f);
    std::unordered_set<uint64_t> emitted;
    auto originKey = [q](float x, float y, float z) {
        int ix = static_cast<int>(std::round(x / q));
        int iy = static_cast<int>(std::round(y / q));
        int iz = static_cast<int>(std::round(z / q));
        uint64_t h = 1469598103934665603ULL;
        auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ULL; };
        mix(static_cast<uint64_t>(ix) + 0x80000000ULL);
        mix(static_cast<uint64_t>(iy) + 0x80000000ULL);
        mix(static_cast<uint64_t>(iz) + 0x80000000ULL);
        return h;
    };
    for (int i = 0; i < limit; i += stride) {
        float dx = data[i].x, dy = data[i].y, dz = data[i].z;
        // skip near-zero vectors so the cloud isn't cluttered with dots
        if (dx * dx + dy * dy + dz * dz < 1e-12f) continue;
        float ox = mesh.vertices[i * 3 + 0];
        float oy = mesh.vertices[i * 3 + 1];
        float oz = mesh.vertices[i * 3 + 2];
        if (!emitted.insert(originKey(ox, oy, oz)).second) continue;
        inst.push_back(ox);
        inst.push_back(oy);
        inst.push_back(oz);
        inst.push_back(dx); inst.push_back(dy); inst.push_back(dz);
    }
    if (inst.empty()) return;
    // All-zero field: keep a sane [0,0] range instead of (max,-max) clamp artifacts.
    if (mMin > mMax) { mMin = 0.0f; mMax = 0.0f; }
    // Raw (untransformed) magnitude range. The shader applies the magnitude
    // transform itself via txMag(); the colorbar legend (renderer.cpp) inverts
    // the transform for tick labels.
    magMin = mMin;
    magMax = mMax;
    meshExtent = static_cast<float>(mesh.bounds.extent);

    std::vector<float> av, an; std::vector<unsigned int> ai;
    buildUnitArrow(av, an, ai);

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &nbo);
    glGenBuffers(1, &ebo);
    glGenBuffers(1, &instVBO);

    glBindVertexArray(vao);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, av.size() * sizeof(float), av.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindBuffer(GL_ARRAY_BUFFER, nbo);
    glBufferData(GL_ARRAY_BUFFER, an.size() * sizeof(float), an.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, ai.size() * sizeof(unsigned int), ai.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, instVBO);
    glBufferData(GL_ARRAY_BUFFER, inst.size() * sizeof(float), inst.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(2);
    glVertexAttribDivisor(2, 1);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribDivisor(3, 1);

    glBindVertexArray(0);
    glyphIndexCount = static_cast<int>(ai.size());
    instanceCount = static_cast<int>(inst.size() / 6);
}
