#include "mesh/mesh_loader.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <cmath>
#include <sstream>

// ── STL Parser ──────────────────────────────────────────────────────────────

static bool isBinarySTL(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return false;

    char header[80];
    file.read(header, 80);
    if (file.gcount() != 80) return false;

    uint32_t triCount = 0;
    file.read(reinterpret_cast<char*>(&triCount), 4);
    if (file.gcount() != 4) return false;

    // Sanity check: file size should be 80 + 4 + triCount * 50
    file.seekg(0, std::ios::end);
    auto fileSize = file.tellg();
    return (fileSize == static_cast<std::streamoff>(84 + triCount * 50));
}

static RenderMesh parseSTLAscii(const std::string& filePath) {
    RenderMesh mesh;
    mesh.bounds = BoundingVolume{};

    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "STL Parser: Failed to open: " << filePath << std::endl;
        return mesh;
    }

    std::string line;
    float faceNormal[3] = { 0, 0, 0 };
    // Store flat data first, then index
    std::vector<float> flatVerts;  // 9 floats per triangle
    std::vector<float> flatNorms;  // 9 floats per triangle (same normal for all 3 verts)

    while (std::getline(file, line)) {
        line = mesh_utils::trim(line);
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string kw;
        iss >> kw;
        kw = mesh_utils::toUpper(kw);

        if (kw == "FACET") {
            iss >> kw; // NORMAL
            iss >> faceNormal[0] >> faceNormal[1] >> faceNormal[2];
            // If normal is zero/unknown, will be computed from geometry below
        }
        else if (kw == "VERTEX") {
            float x, y, z;
            iss >> x >> y >> z;
            flatVerts.push_back(x);
            flatVerts.push_back(y);
            flatVerts.push_back(z);
            // Per-face normal (same for all 3 vertices of this triangle)
            flatNorms.push_back(faceNormal[0]);
            flatNorms.push_back(faceNormal[1]);
            flatNorms.push_back(faceNormal[2]);
        }
    }
    file.close();

    if (flatVerts.empty()) {
        std::cerr << "STL Parser: No vertices loaded from " << filePath << std::endl;
        return mesh;
    }

    // Fix zero-normals: many STL files store (0,0,0) normals and rely on geometric computation
    for (size_t i = 0; i < flatVerts.size(); i += 9) {
        float nx = flatNorms[i], ny = flatNorms[i + 1], nz = flatNorms[i + 2];
        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len < 1e-10f) {
            // Compute normal from triangle edges: normalize(cross(v1 - v0, v2 - v0))
            float v0x = flatVerts[i], v0y = flatVerts[i + 1], v0z = flatVerts[i + 2];
            float v1x = flatVerts[i + 3], v1y = flatVerts[i + 4], v1z = flatVerts[i + 5];
            float v2x = flatVerts[i + 6], v2y = flatVerts[i + 7], v2z = flatVerts[i + 8];

            float e1x = v1x - v0x, e1y = v1y - v0y, e1z = v1z - v0z;
            float e2x = v2x - v0x, e2y = v2y - v0y, e2z = v2z - v0z;

            float cx = e1y * e2z - e1z * e2y;
            float cy = e1z * e2x - e1x * e2z;
            float cz = e1x * e2y - e1y * e2x;
            float clen = std::sqrt(cx * cx + cy * cy + cz * cz);
            if (clen > 1e-10f) {
                cx /= clen; cy /= clen; cz /= clen;
            }
            else {
                cx = 0.0f; cy = 0.0f; cz = 1.0f; // Degenerate triangle fallback
            }
            // Store computed normal for all 3 vertices of this triangle
            flatNorms[i] = cx; flatNorms[i + 1] = cy; flatNorms[i + 2] = cz;
            flatNorms[i + 3] = cx; flatNorms[i + 4] = cy; flatNorms[i + 5] = cz;
            flatNorms[i + 6] = cx; flatNorms[i + 7] = cy; flatNorms[i + 8] = cz;
        }
    }

    // Convert flat to indexed: deduplicate vertices
    mesh.vertices = flatVerts;  // For STL, we keep flat (each vertex is unique per face)
    mesh.normals = flatNorms;
    for (size_t i = 0; i < flatVerts.size() / 3; i++) {
        mesh.indices.push_back(static_cast<int>(i));
    }

    mesh_utils::computeBounds(mesh);

    std::cout << "STL Parser (ASCII): Loaded " << flatVerts.size() / 3 << " vertices, "
        << flatVerts.size() / 9 << " triangles" << std::endl;
    return mesh;
}

static RenderMesh parseSTLBinary(const std::string& filePath) {
    RenderMesh mesh;
    mesh.bounds = BoundingVolume{};

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "STL Parser: Failed to open: " << filePath << std::endl;
        return mesh;
    }

    char header[80];
    file.read(header, 80);

    uint32_t triCount = 0;
    file.read(reinterpret_cast<char*>(&triCount), 4);

    if (triCount == 0 || triCount > 50000000) {
        std::cerr << "STL Parser: Invalid triangle count: " << triCount << std::endl;
        return mesh;
    }

    // Correct allocation matching: 3 coordinates per vertex, 3 vertices per triangle = 9 * triCount
    mesh.vertices.reserve(triCount * 9);
    mesh.normals.reserve(triCount * 9);
    mesh.indices.reserve(triCount * 3);

    for (uint32_t i = 0; i < triCount; i++) {
        float n[3], v[3][3];
        file.read(reinterpret_cast<char*>(n), 12);
        file.read(reinterpret_cast<char*>(v), 36);
        uint16_t attr;
        file.read(reinterpret_cast<char*>(&attr), 2);

        // Check if normal is zero/unknown and compute from geometry if needed
        float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len < 1e-10f) {
            float e1x = v[1][0] - v[0][0], e1y = v[1][1] - v[0][1], e1z = v[1][2] - v[0][2];
            float e2x = v[2][0] - v[0][0], e2y = v[2][1] - v[0][1], e2z = v[2][2] - v[0][2];
            float cx = e1y * e2z - e1z * e2y;
            float cy = e1z * e2x - e1x * e2z;
            float cz = e1x * e2y - e1y * e2x;
            float clen = std::sqrt(cx * cx + cy * cy + cz * cz);
            if (clen > 1e-10f) {
                n[0] = cx / clen; n[1] = cy / clen; n[2] = cz / clen;
            }
            else {
                n[0] = 0.0f; n[1] = 0.0f; n[2] = 1.0f;
            }
        }

        // 3 vertices and matching normals (flat layout alignment fix)
        for (int j = 0; j < 3; j++) {
            // Push normal for EACH vertex to match structure
            mesh.normals.push_back(n[0]);
            mesh.normals.push_back(n[1]);
            mesh.normals.push_back(n[2]);

            mesh.vertices.push_back(v[j][0]);
            mesh.vertices.push_back(v[j][1]);
            mesh.vertices.push_back(v[j][2]);

            mesh.indices.push_back(static_cast<int>(i * 3 + j));
        }
    }
    file.close();

    mesh_utils::computeBounds(mesh);

    std::cout << "STL Parser (Binary): Loaded " << triCount << " triangles" << std::endl;
    return mesh;
}

RenderMesh parseSTL(const std::string& filePath) {
    if (isBinarySTL(filePath)) {
        return parseSTLBinary(filePath);
    }
    return parseSTLAscii(filePath);
}