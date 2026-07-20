#include "core/obj_parser.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <unordered_map>
#include <cstdint>
#include <cmath>

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
        mesh.vertices.insert(mesh.vertices.end(), { x, y, z });
        posToIndex.emplace(key, idx);
        return idx;
    };

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string tag; ss >> tag;
        if (tag == "v") {
            float x, y, z;
            if (!(ss >> x >> y >> z)) continue;
            rawVerts.insert(rawVerts.end(), { x, y, z });
        } else if (tag == "f") {
            std::vector<uint32_t> face;
            std::string tok;
            while (ss >> tok) {
                // take text before first '/', parse as 1-based index (neg = from end)
                size_t slash = tok.find('/');
                int vi = std::stoi(tok.substr(0, slash));
                int n = static_cast<int>(rawVerts.size() / 3);
                if (vi < 0) vi += n + 1;
                if (vi < 1 || vi > n) continue;
                face.push_back(addVertex(rawVerts[(vi-1)*3], rawVerts[(vi-1)*3+1], rawVerts[(vi-1)*3+2]));
            }
            // ponytail: fan-triangulate (convex). Non-convex/holed faces would
            // need ear-clipping; add if real datasets break.
            for (size_t i = 1; i + 1 < face.size(); ++i)
                mesh.indices.insert(mesh.indices.end(), { face[0], face[i], face[i+1] });
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
