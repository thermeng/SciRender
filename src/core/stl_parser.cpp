#include "core/mesh_loader.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <cstdint>
#include <tuple>
#include <charconv>
#include <cctype>

// ── STL Parser ──────────────────────────────────────────────────────────────

// Returns true if the first up-to-512 bytes look like ASCII STL text: contain the
// "solid" keyword AND have no non-printable (control) bytes beyond whitespace.
static bool looksLikeAsciiSTL(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return false;

    char buf[512];
    file.read(buf, sizeof(buf));
    std::streamsize n = file.gcount();
    if (n == 0) return false;

    bool hasSolid = false;
    bool hasNonPrintable = false;
    // Check for "solid" keyword using zero-alloc first-char + length check
    int wordLen = 0;
    char wordFirst = 0;
    for (std::streamsize i = 0; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(buf[i]);
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (wordLen > 0) {
                if (wordLen == 5 && (::toupper(wordFirst) == 'S')) hasSolid = true;
                wordLen = 0;
            }
            continue;
        }
        // Reject anything that isn't a normal printable ASCII / text char.
        if (c < 0x20 || c > 0x7E) { hasNonPrintable = true; break; }
        if (wordLen == 0) wordFirst = static_cast<char>(c);
        ++wordLen;
    }
    if (wordLen > 0 && wordLen == 5 && (::toupper(wordFirst) == 'S')) hasSolid = true;

    // ASCII STL iff it mentions "solid" and contains no binary (control) bytes.
    return hasSolid && !hasNonPrintable;
}

static bool isBinarySTL(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return false;

    char header[80];
    file.read(header, 80);
    if (file.gcount() != 80) return false;

    uint32_t triCount = 0;
    file.read(reinterpret_cast<char*>(&triCount), 4);
    if (file.gcount() != 4) return false;

    // Some valid binary STLs carry trailing/appended data (color blocks, comments);
    // the strict-equality check below rejected them. Relax to >= so they still load.
    file.seekg(0, std::ios::end);
    auto fileSize = file.tellg();

    // Decide by the explicit "solid" ASCII marker first; only fall back to the
    // size gate when the file does NOT look like ASCII text. This prevents a
    // mis-sized ASCII file from being parsed as garbage binary (the classic STL
    // failure mode). An ASCII file whose first token happens to be "solid" is
    // always treated as ASCII; everything else is binary only if its size matches.
    if (looksLikeAsciiSTL(filePath)) return false;
    return (fileSize >= static_cast<std::streamoff>(84 + static_cast<std::streamoff>(triCount) * 50));
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
    std::vector<float> flatVerts;  // 9 floats per triangle

    while (std::getline(file, line)) {
        const char* raw = line.data();
        const char* rawEnd = raw + line.size();

        // Skip leading whitespace (zero-alloc trim)
        const char* p = mesh_utils::skipWhitespace(raw, rawEnd);
        if (p >= rawEnd) continue;

        // Dispatch on first character of keyword (zero-alloc keyword check)
        // STL keywords: solid, endsolid, facet, endfacet, vertex, endloop, endloop, outer, endloop
        // Only 'f' (facet) and 'v' (vertex) contain numeric data we need.
        char kw0 = static_cast<char>(::toupper(static_cast<unsigned char>(*p)));
        if (kw0 != 'F' && kw0 != 'V') continue;

        // Advance past keyword (skip non-whitespace)
        const char* kwEnd = mesh_utils::skipToken(p, rawEnd);
        // For 'facet', skip the second word "normal"
        if (kw0 == 'F') {
            p = mesh_utils::skipWhitespace(kwEnd, rawEnd);
            if (p < rawEnd) {
                // Skip "normal" keyword
                const char* normalEnd = mesh_utils::skipToken(p, rawEnd);
                p = mesh_utils::skipWhitespace(normalEnd, rawEnd);
            }
        } else {
            p = mesh_utils::skipWhitespace(kwEnd, rawEnd);
        }

        if (p >= rawEnd) continue;

        if (kw0 == 'F') {
            // Parse 3 floats for facet normal
            if (p < rawEnd) {
                auto [ptr0, ec0] = std::from_chars(p, rawEnd, faceNormal[0]);
                if (ptr0 < rawEnd && ec0 == std::errc()) {
                    ptr0 = mesh_utils::skipWhitespace(ptr0, rawEnd);
                    auto [ptr1, ec1] = std::from_chars(ptr0, rawEnd, faceNormal[1]);
                    if (ptr1 < rawEnd && ec1 == std::errc()) {
                        ptr1 = mesh_utils::skipWhitespace(ptr1, rawEnd);
                        std::from_chars(ptr1, rawEnd, faceNormal[2]);
                    }
                }
            }
            // Normals are recomputed geometrically by computeNormals() on the
            // indexed mesh; the stored normal is not used for rendering.
        }
        else if (kw0 == 'V') {
            float x, y, z;
            auto [ptr0, ec0] = std::from_chars(p, rawEnd, x);
            if (ec0 != std::errc()) {
                std::cerr << "STL Parser: malformed VERTEX line, skipping" << std::endl;
                continue;
            }
            ptr0 = mesh_utils::skipWhitespace(ptr0, rawEnd);
            auto [ptr1, ec1] = std::from_chars(ptr0, rawEnd, y);
            if (ec1 != std::errc()) {
                std::cerr << "STL Parser: malformed VERTEX line, skipping" << std::endl;
                continue;
            }
            ptr1 = mesh_utils::skipWhitespace(ptr1, rawEnd);
            auto [ptr2, ec2] = std::from_chars(ptr1, rawEnd, z);
            if (ec2 != std::errc()) {
                std::cerr << "STL Parser: malformed VERTEX line, skipping" << std::endl;
                continue;
            }
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
                std::cerr << "STL Parser: non-finite VERTEX coordinate, skipping" << std::endl;
                continue;
            }
            flatVerts.push_back(x);
            flatVerts.push_back(y);
            flatVerts.push_back(z);
        }
    }
    file.close();

    if (flatVerts.empty()) {
        std::cerr << "STL Parser: No vertices loaded from " << filePath << std::endl;
        return mesh;
    }

    // Convert flat to indexed: deduplicate shared vertices via a position hash
    // so the GPU receives an indexed mesh instead of 3x-expanded flat data.
    // Sort-based dedup: O(N log N) sort beats per-vertex hash lookups on
    // large meshes (better cache behavior, no hash collision overhead).
    mesh_utils::indexFlatTriangles(flatVerts, mesh.vertices, mesh.indices);

    // Hand the raw per-corner positions (9 floats/tri) to the mesh-quality
    // analyzer; it welds these at trimesh's 1e-8 tolerance. The rendered indexed
    // mesh keeps the looser 1/4096 dedup, so the two stay separate on purpose.
    mesh.flatVerts = std::move(flatVerts);

    mesh_utils::finalizeSurfaceMesh(mesh, "STL", "STL", "STL Parser (ASCII): Loaded ");
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

    // Deduplicate shared vertices via a sort-based approach (see parseSTLAscii)
    // so the GPU receives an indexed mesh instead of 3x-expanded flat data.
    mesh.vertices.reserve(triCount * 3 * 3); // upper bound
    mesh.indices.reserve(triCount * 3);

    std::vector<float> flatVerts;
    flatVerts.reserve(triCount * 9);

    for (uint32_t i = 0; i < triCount; i++) {
        // Single 50-byte read per triangle (was 3 separate reads + checks).
        char record[50];
        file.read(record, sizeof(record));
        if (file.gcount() != 50) break;

        float n[3];
        float v0[3], v1[3], v2[3];
        uint16_t attr;
        std::memcpy(n, record,       12);
        std::memcpy(v0, record + 12, 12);
        std::memcpy(v1, record + 24, 12);
        std::memcpy(v2, record + 36, 12);
        std::memcpy(&attr, record + 48, 2);
        (void)n; (void)attr;

        for (float* vp : { v0, v1, v2 }) {
            float x = vp[0], y = vp[1], z = vp[2];
            // capture raw corner BEFORE the dedup (mesh-quality welds at 1e-8)
            flatVerts.push_back(x);
            flatVerts.push_back(y);
            flatVerts.push_back(z);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
                std::cerr << "STL Parser: non-finite binary vertex, aborting" << std::endl;
                mesh = RenderMesh{};
                mesh.bounds = BoundingVolume{};
                file.close();
                return mesh;
            }
        }
    }

    file.close();

    // Sort-based dedup (same algorithm as ASCII path)
    mesh_utils::indexFlatTriangles(flatVerts, mesh.vertices, mesh.indices);

    mesh.flatVerts = std::move(flatVerts);
    mesh_utils::finalizeSurfaceMesh(mesh, "STL", "STL", "STL Parser (Binary): Loaded ");
    return mesh;
}

RenderMesh parseSTL(const std::string& filePath) {
    // Determine format up front so a mis-detection can be diagnosed instead of
    // silently yielding an empty mesh.
    bool isBinary = isBinarySTL(filePath);
    if (isBinary) {
        RenderMesh mesh = parseSTLBinary(filePath);
        if (mesh.vertices.empty() && mesh.indices.empty()) {
            std::cerr << "STL Parser: binary detection passed but no triangles "
                         "parsed (corrupt or mis-detected file): " << filePath << std::endl;
        }
        return mesh;
    }

    return parseSTLAscii(filePath);
}
