#include "core/obj_parser.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <unordered_map>
#include <cstdint>
#include <cmath>
#include <charconv>
#include <cctype>

// ponytail: OBJ vertex dedup reuses the shared injective quantized-key from
// mesh_utils (only truly coincident verts merge) — see mesh_loader.h.

RenderMesh parseOBJ(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) throw std::runtime_error("OBJ: cannot open file: " + filePath);

    RenderMesh mesh;
    std::vector<float> rawVerts;          // 1-based source positions
    std::unordered_map<mesh_utils::PositionKey, uint32_t, mesh_utils::PositionKeyHash> posToIndex;

    auto addVertex = [&](float x, float y, float z) -> uint32_t {
        mesh_utils::PositionKey key{ mesh_utils::quantizedCoord(x), mesh_utils::quantizedCoord(y), mesh_utils::quantizedCoord(z) };
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

    // Cell-edge wireframe fallback: OBJ has no cell topology, so extract
    // deduplicated triangle edges from the indexed mesh (before computeNormals
    // splits vertices — indices stay valid after the split).
    mesh.cellEdgeIndices = mesh_utils::extractTriEdges(mesh.indices);

    mesh_utils::finalizeSurfaceMesh(mesh, "OBJ", "OBJ", "OBJ Parser: ");
    return mesh;
}
