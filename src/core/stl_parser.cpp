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

    // Incremental bounds tracking (avoids sentinel values — init from first vertex)
    double minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
    bool boundsInit = false;

    while (std::getline(file, line)) {
        const char* raw = line.data();
        const char* rawEnd = raw + line.size();

        // Skip leading whitespace (zero-alloc trim)
        const char* p = mesh_utils::skipWhitespace(raw, rawEnd);
        if (p >= rawEnd) continue;

        // Dispatch on first character of keyword (zero-alloc keyword check)
        // STL keywords: solid, endsolid, facet, endfacet, vertex, endloop, outer, endloop
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

            // Track bounds incrementally on raw corners (pre-dedup). Bounds of raw
            // corners equal bounds of deduped vertices within 1/4096 units.
            double dx = static_cast<double>(x), dy = static_cast<double>(y), dz = static_cast<double>(z);
            if (!boundsInit) {
                minX = dx; minY = dy; minZ = dz;
                maxX = dx; maxY = dy; maxZ = dz;
                boundsInit = true;
            } else {
                if (dx < minX) minX = dx; if (dx > maxX) maxX = dx;
                if (dy < minY) minY = dy; if (dy > maxY) maxY = dy;
                if (dz < minZ) minZ = dz; if (dz > maxZ) maxZ = dz;
            }
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

    // Cell-edge wireframe fallback: STL has no cell topology, so extract
    // deduplicated triangle edges from the indexed mesh (before computeNormals
    // splits vertices — indices stay valid after the split).
    mesh.cellEdgeIndices = mesh_utils::extractTriEdges(mesh.indices);

    // Finalize pre-computed bounds (finalizeSurfaceMesh will skip computeBounds
    // via the hasBounds flag). Raw corner bounds equal post-dedup bounds within
    // 1/4096 units — numerically equivalent for axis-aligned bounding box.
    mesh.bounds.minX = minX; mesh.bounds.maxX = maxX;
    mesh.bounds.minY = minY; mesh.bounds.maxY = maxY;
    mesh.bounds.minZ = minZ; mesh.bounds.maxZ = maxZ;
    mesh.bounds.centerX = (minX + maxX) * 0.5;
    mesh.bounds.centerY = (minY + maxY) * 0.5;
    mesh.bounds.centerZ = (minZ + maxZ) * 0.5;
    double dx = maxX - minX, dy = maxY - minY, dz = maxZ - minZ;
    mesh.bounds.extent = std::max({ dx, dy, dz });
    if (mesh.bounds.extent < 0.001) mesh.bounds.extent = 1.0;
    mesh.bounds.worldRadius = mesh.bounds.extent * 0.5;
    mesh.hasBounds = true;

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

    // Bulk-read all triangle data into a single contiguous buffer to eliminate
    // per-triangle I/O overhead (was 1 read call per triangle — 3M syscalls for 1M tris).
    // Binary STL record layout: 12-byte normal + 3*12-byte vertices + 2-byte attr = 50 bytes.
    constexpr size_t kRecordSize = 50;
    const size_t dataSize = static_cast<size_t>(triCount) * kRecordSize;
    std::vector<char> buffer(dataSize);
    file.read(buffer.data(), static_cast<std::streamsize>(dataSize));
    std::streamsize bytesRead = file.gcount();
    file.close();

    // Handle truncated/corrupt files: floor to complete records
    uint32_t actualTris = static_cast<uint32_t>(bytesRead / kRecordSize);
    if (actualTris == 0) {
        std::cerr << "STL Parser: No triangle data read" << std::endl;
        return mesh;
    }

    // Pre-size flatVerts for direct indexed writes (eliminates push_back overhead)
    std::vector<float> flatVerts(static_cast<size_t>(actualTris) * 9);

    // Initialize bounds from the first triangle's first vertex (avoids sentinel
    // values like 1e300 that add unnecessary comparisons on every vertex).
    const float* firstV = reinterpret_cast<const float*>(buffer.data() + 12);
    double minX = static_cast<double>(firstV[0]), minY = static_cast<double>(firstV[1]), minZ = static_cast<double>(firstV[2]);
    double maxX = static_cast<double>(firstV[0]), maxY = static_cast<double>(firstV[1]), maxZ = static_cast<double>(firstV[2]);

    const char* recBase = buffer.data();
    for (uint32_t t = 0; t < actualTris; t++) {
        // Bulk-copy 36 bytes of vertex data directly into the pre-sized flatVerts buffer
        float* out = flatVerts.data() + static_cast<size_t>(t) * 9;
        std::memcpy(out, recBase + static_cast<size_t>(t) * kRecordSize + 12, 36);

        // Validate finiteness and track bounds on all 3 vertex positions (9 floats)
        for (int i = 0; i < 9; i += 3) {
            float x = out[i], y = out[i + 1], z = out[i + 2];
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
                std::cerr << "STL Parser: non-finite binary vertex, aborting" << std::endl;
                mesh = RenderMesh{};
                mesh.bounds = BoundingVolume{};
                return mesh;
            }
            double dx = static_cast<double>(x), dy = static_cast<double>(y), dz = static_cast<double>(z);
            if (dx < minX) minX = dx; if (dx > maxX) maxX = dx;
            if (dy < minY) minY = dy; if (dy > maxY) maxY = dy;
            if (dz < minZ) minZ = dz; if (dz > maxZ) maxZ = dz;
        }
    }

    // Sort-based dedup (same algorithm as ASCII path)
    mesh_utils::indexFlatTriangles(flatVerts, mesh.vertices, mesh.indices);

    // Hand the raw per-corner positions (9 floats/tri) to the mesh-quality
    // analyzer; it welds these at trimesh's 1e-8 tolerance. The rendered indexed
    // mesh keeps the looser 1/4096 dedup, so the two stay separate on purpose.
    mesh.flatVerts = std::move(flatVerts);

    // Cell-edge wireframe fallback: STL has no cell topology, so extract
    // deduplicated triangle edges from the indexed mesh (before computeNormals
    // splits vertices — indices stay valid after the split).
    mesh.cellEdgeIndices = mesh_utils::extractTriEdges(mesh.indices);

    // Finalize pre-computed bounds (finalizeSurfaceMesh will skip computeBounds
    // via the hasBounds flag). Raw corner bounds equal post-dedup bounds within
    // 1/4096 units — numerically equivalent for axis-aligned bounding box.
    mesh.bounds.minX = minX; mesh.bounds.maxX = maxX;
    mesh.bounds.minY = minY; mesh.bounds.maxY = maxY;
    mesh.bounds.minZ = minZ; mesh.bounds.maxZ = maxZ;
    mesh.bounds.centerX = (minX + maxX) * 0.5;
    mesh.bounds.centerY = (minY + maxY) * 0.5;
    mesh.bounds.centerZ = (minZ + maxZ) * 0.5;
    double dx = maxX - minX, dy = maxY - minY, dz = maxZ - minZ;
    mesh.bounds.extent = std::max({ dx, dy, dz });
    if (mesh.bounds.extent < 0.001) mesh.bounds.extent = 1.0;
    mesh.bounds.worldRadius = mesh.bounds.extent * 0.5;
    mesh.hasBounds = true;

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
