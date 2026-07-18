#include "render/VectorGlyphSet.h"
#include "core/mesh_loader.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <limits>

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

void VectorGlyphSet::rebuild(const RenderMesh& mesh, int stride) {
    teardownGL();

    if (mesh.pointVectorsData.empty()) return;

    const std::string& field = mesh.vectorName.empty()
        ? mesh.availableVectorNames.front()
        : mesh.vectorName;
    size_t count = 0;
    const glm::vec3* data = mesh.vectorFieldData(field, count);
    if (!data || count == 0) return;

    int numPts = static_cast<int>(mesh.vertices.size() / 3);
    stride = std::max(1, stride);
    // Track min/max magnitude across the FULL field (every point), not just the
    // strided render sample. magMin/magMax also drive the vector colorbar legend
    // (renderer.cpp), so using only the subsampled subset made glyph colors and
    // the legend scale disagree when vectorStride > 1.
    float mMin = std::numeric_limits<float>::max();
    float mMax = -std::numeric_limits<float>::max();
    for (int i = 0; i < numPts; ++i) {
        float dx = data[i].x, dy = data[i].y, dz = data[i].z;
        float m = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!std::isfinite(m)) continue;
        if (m < mMin) mMin = m;
        if (m > mMax) mMax = m;
    }
    std::vector<float> inst;
    for (int i = 0; i < numPts; i += stride) {
        float dx = data[i].x, dy = data[i].y, dz = data[i].z;
        // skip near-zero vectors so the cloud isn't cluttered with dots
        if (dx * dx + dy * dy + dz * dz < 1e-12f) continue;
        inst.push_back(mesh.vertices[i * 3 + 0]);
        inst.push_back(mesh.vertices[i * 3 + 1]);
        inst.push_back(mesh.vertices[i * 3 + 2]);
        inst.push_back(dx); inst.push_back(dy); inst.push_back(dz);
    }
    if (inst.empty()) return;
    // All-zero field: keep a sane [0,0] range instead of (max,-max) clamp artifacts.
    if (mMin > mMax) { mMin = 0.0f; mMax = 0.0f; }
    magMin = mMin;
    magMax = mMax;

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
