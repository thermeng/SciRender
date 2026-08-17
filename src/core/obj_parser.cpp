#include "core/obj_parser.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <unordered_map>
#include <cstdint>
#include <cmath>
#include <charconv>
#include <cctype>

// ponytail: OBJ vertex dedup reuses the same injective quantized-key idea as
// stl_parser (only truly coincident verts merge). Kept local — one small map,
// no shared abstraction warranted for two callers.
static inline int64_t qc(double v) {
    const int64_t q = 1 << 12; // 1/4096 tolerance
    int64_t ix = static_cast<int64_t>(std::llround(v * static_cast<double>(q)));
    const int64_t lim = (int64_t(1) << 25) - 1;
    if (ix > lim) ix = lim; else if (ix < -lim) ix = -lim;
    return ix;
}
using VKey = std::tuple<int64_t, int64_t, int64_t>;
struct VKeyHash {
    size_t operator()(const VKey& k) const noexcept {
        uint64_t h = static_cast<uint64_t>(std::get<0>(k)) * 73856093u;
        h ^= static_cast<uint64_t>(std::get<1>(k)) * 19349663u + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h ^= static_cast<uint64_t>(std::get<2>(k)) * 83492791u + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return static_cast<size_t>(h);
    }
};

RenderMesh parseOBJ(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) throw std::runtime_error("OBJ: cannot open file: " + filePath);

    RenderMesh mesh;
    std::vector<float> rawVerts;          // 1-based source positions
    std::unordered_map<VKey, uint32_t, VKeyHash> posToIndex;

    auto addVertex = [&](float x, float y, float z) -> uint32_t {
        VKey key{ qc(x), qc(y), qc(z) };
        auto it = posToIndex.find(key);
        if (it != posToIndex.end()) return it->second;
        uint32_t idx = static_cast<uint32_t>(mesh.vertices.size() / 3);
        mesh.vertices.push_back(x);
        mesh.vertices.push_back(y);
        mesh.vertices.push_back(z);
        posToIndex.emplace(key, idx);
        return idx;
    };

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        const char* p = line.data();
        const char* end = p + line.size();
        while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;

        // read tag
        const char* tagStart = p;
        while (p < end && !std::isspace(static_cast<unsigned char>(*p))) ++p;
        std::string tag(tagStart, p - tagStart);

        while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;

        if (tag == "v") {
            float x, y, z;
            auto [ptr0, ec0] = std::from_chars(p, end, x);
            if (ec0 != std::errc()) continue;
            while (ptr0 < end && std::isspace(static_cast<unsigned char>(*ptr0))) ++ptr0;
            auto [ptr1, ec1] = std::from_chars(ptr0, end, y);
            if (ec1 != std::errc()) continue;
            while (ptr1 < end && std::isspace(static_cast<unsigned char>(*ptr1))) ++ptr1;
            auto [ptr2, ec2] = std::from_chars(ptr1, end, z);
            if (ec2 != std::errc()) continue;
            rawVerts.push_back(x);
            rawVerts.push_back(y);
            rawVerts.push_back(z);
        } else if (tag == "f") {
            std::vector<uint32_t> face;
            while (p < end) {
                const char* tokStart = p;
                while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '/') ++p;
                long vi = 0;
                auto [ptr, ec] = std::from_chars(tokStart, p, vi);
                if (ec == std::errc()) {
                    int n = static_cast<int>(rawVerts.size() / 3);
                    if (vi < 0) vi += n + 1;
                    if (vi >= 1 && vi <= n)
                        face.push_back(addVertex(rawVerts[(vi-1)*3], rawVerts[(vi-1)*3+1], rawVerts[(vi-1)*3+2]));
                }
                // advance to next whitespace-delimited token (skip /vt/vn parts)
                while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') ++p;
                while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
            }
            // ponytail: fan-triangulate (convex). Non-convex/holed faces would
            // need ear-clipping; add if real datasets break.
            for (size_t i = 1; i + 1 < face.size(); ++i) {
                mesh.indices.push_back(face[0]);
                mesh.indices.push_back(face[i]);
                mesh.indices.push_back(face[i+1]);
            }
        }
        // ponytail: vt/vn/l/o/g/etc. intentionally ignored (see header).
    }
    file.close();

    if (mesh.vertices.empty()) {
        std::cerr << "OBJ Parser: no vertices parsed (empty or non-OBJ file): " << filePath << std::endl;
        return mesh;
    }

    mesh.flatVerts = mesh.vertices; // ponytail: OBJ already indexed; flat == indexed here
    mesh_utils::computeBounds(mesh);
    mesh.sourcePointCount = static_cast<int>(mesh.vertices.size() / 3);
    mesh.datasetType = "OBJ";
    mesh.fileFormat = "OBJ";
    if (mesh.normals.empty())
        mesh_utils::computeNormals(mesh);

    std::cout << "OBJ Parser: " << mesh.indices.size() / 3 << " triangles, "
              << mesh.vertices.size() / 3 << " unique vertices" << std::endl;
    return mesh;
}
