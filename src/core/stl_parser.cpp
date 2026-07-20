#include "core/mesh_loader.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <cmath>
#include <sstream>
#include <unordered_map>
#include <cstdint>
#include <tuple>

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
    std::string word;
    for (std::streamsize i = 0; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(buf[i]);
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (!word.empty()) {
                if (mesh_utils::toUpper(word) == "SOLID") hasSolid = true;
                word.clear();
            }
            continue;
        }
        // Reject anything that isn't a normal printable ASCII / text char.
        if (c < 0x20 || c > 0x7E) { hasNonPrintable = true; break; }
        word.push_back(static_cast<char>(c));
    }
    if (!word.empty() && mesh_utils::toUpper(word) == "SOLID") hasSolid = true;

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
    // Store flat vertex data first, then deduplicate into an indexed mesh.
    std::vector<float> flatVerts;  // 9 floats per triangle

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
            // Normals are recomputed geometrically by computeNormals() on the
            // indexed mesh; the stored normal is not used for rendering.
        }
        else if (kw == "VERTEX") {
            float x, y, z;
            // Validate that all three floats parsed; a malformed token between
            // vertices must not silently inject NaN/garbage into the vertex stream.
            if (!(iss >> x >> y >> z)) {
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
    // Normals are left empty on purpose so the renderer's computeNormals()
    // computes correct geometry-based normals (sharp-edge split + smoothing)
    // on the merged indexed layout.
    std::unordered_map<VertexKey, int, VertexKeyHash> posToIndex;

    mesh.vertices.reserve(flatVerts.size());
    const size_t triCount = flatVerts.size() / 9;
    mesh.indices.reserve(triCount * 3);

    for (size_t t = 0; t < triCount; ++t) {
        for (int k = 0; k < 3; ++k) {
            size_t base = t * 9 + k * 3;
            float x = flatVerts[base + 0], y = flatVerts[base + 1], z = flatVerts[base + 2];
            VertexKey key = vertexKey(x, y, z);
            int idx;
            auto it = posToIndex.find(key);
            if (it != posToIndex.end()) {
                idx = it->second;
            } else {
                idx = static_cast<int>(mesh.vertices.size() / 3);
                mesh.vertices.push_back(x);
                mesh.vertices.push_back(y);
                mesh.vertices.push_back(z);
                posToIndex[key] = idx;
            }
            mesh.indices.push_back(idx);
        }
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

    // Deduplicate shared vertices via a position hash so the GPU receives an
    // indexed mesh instead of 3x-expanded flat data. Normals are left empty so
    // the renderer's computeNormals() computes correct geometry-based normals.
    std::unordered_map<VertexKey, int, VertexKeyHash> posToIndex;

    mesh.vertices.reserve(triCount * 3);
    mesh.indices.reserve(triCount * 3);

    for (uint32_t i = 0; i < triCount; i++) {
        float n[3], v[3][3];
        uint16_t attr;
        // Verify each 50-byte record was fully read. A truncated/corrupt file
        // that stops mid-record must break rather than append partial floats as
        // valid vertices (which would yield out-of-range indices downstream).
        file.read(reinterpret_cast<char*>(n), 12);
        if (file.gcount() != 12) break;
        file.read(reinterpret_cast<char*>(v), 36);
        if (file.gcount() != 36) break;
        file.read(reinterpret_cast<char*>(&attr), 2);
        if (file.gcount() != 2) break;

        for (int j = 0; j < 3; j++) {
            float x = v[j][0], y = v[j][1], z = v[j][2];
            // capture raw corner BEFORE the 1/4096 dedup (mesh-quality welds at 1e-8)
            mesh.flatVerts.insert(mesh.flatVerts.end(), { x, y, z });
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
                std::cerr << "STL Parser: non-finite binary vertex, aborting" << std::endl;
                mesh = RenderMesh{};
                mesh.bounds = BoundingVolume{};
                file.close();
                return mesh;
            }
            VertexKey key = vertexKey(x, y, z);
            int idx;
            auto it = posToIndex.find(key);
            if (it != posToIndex.end()) {
                idx = it->second;
            } else {
                idx = static_cast<int>(mesh.vertices.size() / 3);
                mesh.vertices.push_back(x);
                mesh.vertices.push_back(y);
                mesh.vertices.push_back(z);
                posToIndex[key] = idx;
            }
            mesh.indices.push_back(idx);
        }
    }

    file.close();

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
