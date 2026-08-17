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

// Quantize a coordinate into a signed fixed-point integer at a 1/4096-unit
// tolerance (far finer than STL float precision). Clamped to ~25 bits of
// magnitude so each axis value is bounded and the composite key below is stable.
static inline int64_t quantizedCoord(double v) {
    const int64_t q = 1 << 12; // 1/4096 unit tolerance — far finer than STL float precision
    int64_t ix = static_cast<int64_t>(std::llround(v * static_cast<double>(q)));
    const int64_t lim = (int64_t(1) << 25) - 1;
    if (ix > lim) ix = lim; else if (ix < -lim) ix = -lim;
    return ix;
}

// Collision-FREE position key. The previous XOR-of-products hash (B2) was not
// injective: two genuinely distinct coordinates could fold onto the same key and
// be merged into one vertex, collapsing distinct surface points. A tuple of the
// three quantized axis values is exactly injective, so ONLY truly coincident
// (within tolerance) vertices ever merge — the correct dedup semantics.
using VertexKey = std::tuple<int64_t, int64_t, int64_t>;

struct VertexKeyHash {
    size_t operator()(const VertexKey& k) const noexcept {
        // Hash the three bounded integers; collisions here only cost a bucket
        // comparison — correctness comes from tuple equality, not the hash.
        uint64_t h = static_cast<uint64_t>(std::get<0>(k)) * 73856093u;
        h ^= static_cast<uint64_t>(std::get<1>(k)) * 19349663u + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h ^= static_cast<uint64_t>(std::get<2>(k)) * 83492791u + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return static_cast<size_t>(h);
    }
};

static inline VertexKey vertexKey(float x, float y, float z) {
    return VertexKey{
        quantizedCoord(static_cast<double>(x)),
        quantizedCoord(static_cast<double>(y)),
        quantizedCoord(static_cast<double>(z))
    };
}

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
    const size_t triCount = flatVerts.size() / 9;
    const size_t cornerCount = triCount * 3;

    struct VertEntry {
        VertexKey key;
        uint32_t flatIdx;   // index into flatVerts (corner index * 3)
    };
    std::vector<VertEntry> entries(cornerCount);
    for (size_t i = 0; i < cornerCount; ++i) {
        size_t base = i * 3;
        entries[i] = { vertexKey(flatVerts[base], flatVerts[base + 1], flatVerts[base + 2]),
                       static_cast<uint32_t>(i) };
    }

    // Sort by quantized position — groups identical vertices together
    std::sort(entries.begin(), entries.end(), [](const VertEntry& a, const VertEntry& b) {
        if (std::get<0>(a.key) != std::get<0>(b.key)) return std::get<0>(a.key) < std::get<0>(b.key);
        if (std::get<1>(a.key) != std::get<1>(b.key)) return std::get<1>(a.key) < std::get<1>(b.key);
        return std::get<2>(a.key) < std::get<2>(b.key);
    });

    // Scan sorted entries: first occurrence of each unique key gets a new vertex index;
    // subsequent occurrences reuse that index.
    std::vector<uint32_t> cornerToIdx(cornerCount); // flat corner → indexed vertex
    mesh.vertices.reserve(cornerCount * 3);          // upper bound
    for (size_t i = 0; i < cornerCount; ) {
        size_t j = i + 1;
        while (j < cornerCount && entries[j].key == entries[i].key) ++j;
        // All entries[i..j) share the same position — emit one vertex
        uint32_t vidx = static_cast<uint32_t>(mesh.vertices.size() / 3);
        size_t base = entries[i].flatIdx * 3;
        mesh.vertices.push_back(flatVerts[base + 0]);
        mesh.vertices.push_back(flatVerts[base + 1]);
        mesh.vertices.push_back(flatVerts[base + 2]);
        for (size_t k = i; k < j; ++k) {
            cornerToIdx[entries[k].flatIdx] = vidx;
        }
        i = j;
    }

    mesh.indices.reserve(cornerCount);
    for (size_t t = 0; t < triCount; ++t) {
        mesh.indices.push_back(cornerToIdx[t * 3 + 0]);
        mesh.indices.push_back(cornerToIdx[t * 3 + 1]);
        mesh.indices.push_back(cornerToIdx[t * 3 + 2]);
    }

    // Hand the raw per-corner positions (9 floats/tri) to the mesh-quality
    // analyzer; it welds these at trimesh's 1e-8 tolerance. The rendered indexed
    // mesh keeps the looser 1/4096 dedup, so the two stay separate on purpose.
    mesh.flatVerts = std::move(flatVerts);

    mesh_utils::computeBounds(mesh);

    // Record the topological point count (deduped, pre-normal-split) so the UI
    // can report the true vertex count ParaView shows. computeNormals() below
    // will duplicate vertices at sharp edges for shading, which would otherwise
    // inflate the displayed "Points" value.
    mesh.sourcePointCount = static_cast<int>(mesh.vertices.size() / 3);

    std::cout << "STL Parser (ASCII): Loaded " << triCount << " triangles, "
        << mesh.vertices.size() / 3 << " unique vertices (deduped)" << std::endl;
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
    const size_t cornerCount = flatVerts.size() / 3;
    struct VertEntry {
        VertexKey key;
        uint32_t flatIdx;
    };
    std::vector<VertEntry> entries(cornerCount);
    for (size_t i = 0; i < cornerCount; ++i) {
        size_t base = i * 3;
        entries[i] = { vertexKey(flatVerts[base], flatVerts[base + 1], flatVerts[base + 2]),
                       static_cast<uint32_t>(i) };
    }
    std::sort(entries.begin(), entries.end(), [](const VertEntry& a, const VertEntry& b) {
        if (std::get<0>(a.key) != std::get<0>(b.key)) return std::get<0>(a.key) < std::get<0>(b.key);
        if (std::get<1>(a.key) != std::get<1>(b.key)) return std::get<1>(a.key) < std::get<1>(b.key);
        return std::get<2>(a.key) < std::get<2>(b.key);
    });
    std::vector<uint32_t> cornerToIdx(cornerCount);
    for (size_t i = 0; i < cornerCount; ) {
        size_t j = i + 1;
        while (j < cornerCount && entries[j].key == entries[i].key) ++j;
        uint32_t vidx = static_cast<uint32_t>(mesh.vertices.size() / 3);
        size_t base = entries[i].flatIdx * 3;
        mesh.vertices.push_back(flatVerts[base + 0]);
        mesh.vertices.push_back(flatVerts[base + 1]);
        mesh.vertices.push_back(flatVerts[base + 2]);
        for (size_t k = i; k < j; ++k) {
            cornerToIdx[entries[k].flatIdx] = vidx;
        }
        i = j;
    }
    const size_t triCountDedup = cornerCount / 3;
    for (size_t t = 0; t < triCountDedup; ++t) {
        mesh.indices.push_back(cornerToIdx[t * 3 + 0]);
        mesh.indices.push_back(cornerToIdx[t * 3 + 1]);
        mesh.indices.push_back(cornerToIdx[t * 3 + 2]);
    }

    mesh.flatVerts = std::move(flatVerts);
    mesh_utils::computeBounds(mesh);

    // Record the topological point count (deduped, pre-normal-split) so the UI
    // can report the true vertex count ParaView shows. computeNormals() below
    // will duplicate vertices at sharp edges for shading, which would otherwise
    // inflate the displayed "Points" value.
    mesh.sourcePointCount = static_cast<int>(mesh.vertices.size() / 3);

    std::cout << "STL Parser (Binary): Loaded " << triCount << " triangles, "
        << mesh.vertices.size() / 3 << " unique vertices (deduped)" << std::endl;
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
        mesh.datasetType = "STL";
        mesh.fileFormat = "STL";
        if (!mesh.vertices.empty() && !mesh.indices.empty()
            && mesh.normals.empty()) {
            mesh_utils::computeNormals(mesh);
        }
        return mesh;
    }

    RenderMesh mesh = parseSTLAscii(filePath);
    mesh.datasetType = "STL";
    mesh.fileFormat = "STL";

    // STL parsers leave normals empty (computed geometrically from the indexed
    // layout). Ensure real normals exist before upload so the renderer/LOD
    // decimate path can read them; computeNormals splits sharp edges and
    // averages smooth regions for correct shading.
    if (mesh.normals.empty() && !mesh.vertices.empty() && !mesh.indices.empty()) {
        mesh_utils::computeNormals(mesh);
    }
    return mesh;
}
