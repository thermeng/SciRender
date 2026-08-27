#include "render/streamlines/VectorGlyphSet.h"
#include "core/mesh_loader.h"
#include "core/FieldResolver.h"

#include <glad/gl.h>
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
    vao.reset();
    vbo.reset();
    nbo.reset();
    ebo.reset();
    instVBO.reset();
    glyphIndexCount = 0;
    instanceCount = 0;
    magMin = 0.0f;
    magMax = 1.0f;
    compMin[0] = compMin[1] = compMin[2] = 0.0f;
    compMax[0] = compMax[1] = compMax[2] = 0.0f;
    meshExtent = 1.0f;
}

void VectorGlyphSet::shutdown() {
    teardownGL();
}

void VectorGlyphSet::rebuild(const RenderMesh& mesh, int stride, const std::string& fieldName, int magTransform, int placement) {
    teardownGL();
    (void)magTransform;

    auto field = FieldResolver::resolveVector(mesh, fieldName, placement);
    if (!field.data || field.count == 0) return;
    bool cellCenter = field.isCell;
    const glm::vec3* data = field.data;
    size_t count = field.count;
    int numPts = cellCenter ? static_cast<int>(mesh.cellCenters.size())
                            : static_cast<int>(mesh.vertices.size() / 3);
    if (cellCenter && mesh.cellCenters.empty()) return;
    if (!cellCenter && mesh.pointVectorsData.empty()) return;

    const int limit = std::min(numPts, static_cast<int>(count));
    stride = std::max(1, stride);
    float mMin = std::numeric_limits<float>::max();
    float mMax = -std::numeric_limits<float>::max();
    float cMin[3] = {
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()
    };
    float cMax[3] = {
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max()
    };
    for (int i = 0; i < limit; ++i) {
        float dx = data[i].x, dy = data[i].y, dz = data[i].z;
        float m = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!std::isfinite(m)) continue;
        if (m < mMin) mMin = m;
        if (m > mMax) mMax = m;
        if (dx < cMin[0]) cMin[0] = dx;
        if (dx > cMax[0]) cMax[0] = dx;
        if (dy < cMin[1]) cMin[1] = dy;
        if (dy > cMax[1]) cMax[1] = dy;
        if (dz < cMin[2]) cMin[2] = dz;
        if (dz > cMax[2]) cMax[2] = dz;
    }
    std::vector<float> inst;
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
        if (dx * dx + dy * dy + dz * dz < 1e-12f) continue;
        float ox, oy, oz;
        if (cellCenter) {
            ox = mesh.cellCenters[i].x;
            oy = mesh.cellCenters[i].y;
            oz = mesh.cellCenters[i].z;
        } else {
            ox = mesh.vertices[i * 3 + 0];
            oy = mesh.vertices[i * 3 + 1];
            oz = mesh.vertices[i * 3 + 2];
        }
        if (!emitted.insert(originKey(ox, oy, oz)).second) continue;
        inst.push_back(ox);
        inst.push_back(oy);
        inst.push_back(oz);
        inst.push_back(dx); inst.push_back(dy); inst.push_back(dz);
    }
    if (inst.empty()) return;
    if (mMin > mMax) { mMin = 0.0f; mMax = 0.0f; }
    magMin = mMin;
    magMax = mMax;
    for (int i = 0; i < 3; ++i) {
        if (cMin[i] > cMax[i]) { cMin[i] = 0.0f; cMax[i] = 0.0f; }
        compMin[i] = cMin[i];
        compMax[i] = cMax[i];
    }
    meshExtent = static_cast<float>(mesh.bounds.extent);

    std::vector<float> av, an; std::vector<unsigned int> ai;
    buildUnitArrow(av, an, ai);

    glCreateVertexArrays(1, vao.ptr());
    glCreateBuffers(1, vbo.ptr());
    glCreateBuffers(1, nbo.ptr());
    glCreateBuffers(1, ebo.ptr());
    glCreateBuffers(1, instVBO.ptr());

    glEnableVertexArrayAttrib(vao, 0);
    glVertexArrayAttribFormat(vao, 0, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(vao, 0, 0);
    glNamedBufferData(vbo, av.size() * sizeof(float), av.data(), GL_STATIC_DRAW);
    glVertexArrayVertexBuffer(vao, 0, vbo, 0, 3 * sizeof(float));

    glEnableVertexArrayAttrib(vao, 1);
    glVertexArrayAttribFormat(vao, 1, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(vao, 1, 1);
    glNamedBufferData(nbo, an.size() * sizeof(float), an.data(), GL_STATIC_DRAW);
    glVertexArrayVertexBuffer(vao, 1, nbo, 0, 3 * sizeof(float));

    glNamedBufferData(ebo, ai.size() * sizeof(unsigned int), ai.data(), GL_STATIC_DRAW);
    glVertexArrayElementBuffer(vao, ebo);

    glEnableVertexArrayAttrib(vao, 2);
    glVertexArrayAttribFormat(vao, 2, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(vao, 2, 2);
    glVertexArrayVertexBuffer(vao, 2, instVBO, 0, 6 * sizeof(float));
    glVertexArrayVertexAttribDivisorEXT(vao, 2, 1);

    glEnableVertexArrayAttrib(vao, 3);
    glVertexArrayAttribFormat(vao, 3, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(vao, 3, 3);
    glVertexArrayVertexBuffer(vao, 3, instVBO, 3 * sizeof(float), 6 * sizeof(float));
    glVertexArrayVertexAttribDivisorEXT(vao, 3, 1);

    glNamedBufferData(instVBO, inst.size() * sizeof(float), inst.data(), GL_STATIC_DRAW);

    glyphIndexCount = static_cast<int>(ai.size());
    instanceCount = static_cast<int>(inst.size() / 6);
}

