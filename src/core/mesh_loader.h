#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <optional>
#include <charconv>
#include <fstream>
#include <tuple>
#include <cmath>
#include <algorithm>

#include <glm/glm.hpp>

// ── Bounding Volume (High-Precision Double Precision) ───────────────────────

struct BoundingVolume {
    // Tight axis-aligned bounding box limits (double precision for massive coordinates)
    double minX = 0.0, maxX = 0.0;
    double minY = 0.0, maxY = 0.0;
    double minZ = 0.0, maxZ = 0.0;

    // Derived metrics
    double centerX = 0.0, centerY = 0.0, centerZ = 0.0;
    double extent = 1.0;           // Max dimension (maxX-minX, maxY-minY, maxZ-minZ)
    double worldRadius = 0.0;      // Bounding sphere radius (extent * 0.5)

    // Convenience accessors
    double width() const { return maxX - minX; }
    double height() const { return maxY - minY; }
    double depth() const { return maxZ - minZ; }
};

// ── Dataset Attributes (Scalar Data Maps) ───────────────────────────────────

struct DatasetAttributes {
    // Point data scalars (per-vertex, 1 component)
    std::unordered_map<std::string, std::vector<float>> pointScalars;
    // Cell data scalars (per-cell, 1 component, averaged to vertices during parsing)
    std::unordered_map<std::string, std::vector<float>> cellScalars;
    // VTK VECTORS — per-point 3-component, interleaved [x,y,z]
    std::unordered_map<std::string, std::vector<float>> pointVectors;
    // Multi-component point field data (2, 4+ components) — stored as interleaved floats
    std::unordered_map<std::string, std::vector<float>> pointFieldData;
    // Multi-component cell field data (2, 4+ components)
    std::unordered_map<std::string, std::vector<float>> cellFieldData;
    // Component counts for multi-component field data (sidecar)
    std::unordered_map<std::string, int> pointFieldComponents;
    std::unordered_map<std::string, int> cellFieldComponents;
    // FieldData (arbitrary named arrays, typically per-cell or global metadata)
    std::unordered_map<std::string, std::vector<float>> fieldData;
    // VTK TENSORS — per-point or per-cell, 9 components (3×3 matrix) per element
    std::unordered_map<std::string, std::vector<float>> pointTensors;
    std::unordered_map<std::string, std::vector<float>> cellTensors;
    // Component counts for tensors (should always be 9)
    std::unordered_map<std::string, int> pointTensorComponents;
    std::unordered_map<std::string, int> cellTensorComponents;
    // VTK TEXTURE_COORDINATES — per-point, 2 or 3 components
    std::unordered_map<std::string, std::vector<float>> pointTexCoords;
    std::unordered_map<std::string, std::vector<float>> cellTexCoords;
    // Component counts for texture coordinates (2 or 3)
    std::unordered_map<std::string, int> pointTexCoordComponents;
    std::unordered_map<std::string, int> cellTexCoordComponents;

    // Global scalar range boundaries (Required by renderer & color LUT mapping)
    // For backward compat; per-field ranges are authoritative.
    float scalarMin = 0.0f;
    float scalarMax = 1.0f;
    // Per-field min/max for every scalar field (point + cell). Survives field switches.
    std::unordered_map<std::string, std::pair<float,float>> pointScalarRanges;
    std::unordered_map<std::string, std::pair<float,float>> cellScalarRanges;
};

// ── Render Mesh (GPU-Facing, Clean Geometry) ────────────────────────────────

struct RenderMesh {
    // Core GPU arrays (float for GPU upload)
    std::vector<float> vertices;   // x,y,z interleaved
    std::vector<uint32_t> indices; // Triangle indices (32-bit, matches GL_UNSIGNED_INT)
    std::vector<float> normals;    // nx,ny,nz interleaved
    std::vector<float> scalars;    // Active scalar field (per-vertex, optional)
    std::string scalarName = "";   // Name of active scalar field
    std::vector<std::string> availableScalarNames; // all point-scalar field names for the QML switcher

    // VTK VECTORS — stored as a single contiguous buffer of vec3 runs. All
    // fields share the same per-vertex count (= vertices.size()/3). The offset
    // (in vec3 units) of each field's run is recorded in pointVectorOffset.
    // This replaces the old pointer-chasing std::map<std::string,vector<float>>.
    std::vector<glm::vec3> pointVectorsData;                       // contiguous, stride 3
    std::unordered_map<std::string, size_t> pointVectorOffset;     // vec3-offset per field name
    std::vector<std::string> availableVectorNames;                // for a QML switcher
    std::string vectorName = "";                                  // active vector field name
    // Per-POINT vector count (== numPoints at parse time, BEFORE flat-shading
    // splits vertices). Vectors are stored one vec3 per source point; the glyph
    // builder and vectorFieldData must clamp runs to THIS count, not to the
    // (larger, post-split) geometry vertex count, or a field over-reads into
    // the next field's run / past the buffer end.
    size_t pointVectorCount = 0;
    bool meshHasVectors() const { return !pointVectorsData.empty(); }

    // Resolves a vector field's contiguous data. Returns the base pointer and
    // the number of vec3 elements, or (nullptr, 0) when the field is unknown.
    const glm::vec3* vectorFieldData(const std::string& name, size_t& count) const {
        auto it = pointVectorOffset.find(name);
        if (it == pointVectorOffset.end()) { count = 0; return nullptr; }
        count = pointVectorsData.size() - it->second;
        // A field spans exactly pointVectorCount vec3 (one per source point);
        // clamp to that rather than the split geometry vertex count so a field
        // never reads into the next field's run or past the buffer.
        if (pointVectorCount != 0 && count > pointVectorCount) count = pointVectorCount;
        return pointVectorsData.data() + it->second;
    }

    std::vector<glm::vec3> cellCenters;
    std::vector<glm::vec3> cellVectorsData;
    std::unordered_map<std::string, size_t> cellVectorOffset;
    std::vector<std::string> availableCellVectorNames;
    std::string cellVectorName = "";
    size_t cellVectorCount = 0;

    const glm::vec3* cellVectorFieldData(const std::string& name, size_t& count) const {
        auto it = cellVectorOffset.find(name);
        if (it == cellVectorOffset.end()) { count = 0; return nullptr; }
        count = cellVectorsData.size() - it->second;
        if (cellVectorCount != 0 && count > cellVectorCount) count = cellVectorCount;
        return cellVectorsData.data() + it->second;
    }

    bool meshHasCellVectors() const { return !cellVectorsData.empty(); }

    // Scalar-data availability: true when the mesh carries at least one scalar
    // field accessible at grid nodes. For structured-grid datasets the surface
    // mesh may have been boundary-extracted (fewer vertices than nodes), which
    // clears `scalars` but preserves the original per-node field in
    // `attributes->pointScalars` — so both are checked.
    bool hasScalarData() const {
        return !scalars.empty()
            || (attributes && !attributes->pointScalars.empty());
    }

    // Volume-grid eligibility: true when gridDim describes at least one 3D cell
    // along every axis (2+ nodes per axis). This is the precondition for both
    // volume rendering (3D texture) and marching-cubes isosurface extraction.
    bool hasVolumeGrid() const {
        return gridDimX > 1 && gridDimY > 1 && gridDimZ > 1;
    }

    // Volume-rendering eligibility: a 3D grid WITH a scalar field. (Volume
    // rendering can also work with non-cell grids, so we accept gridDim > 0
    // here rather than the stricter > 1 required by isosurface extraction.)
    bool hasVolumeData() const {
        return gridDimX > 0 && hasScalarData();
    }

    // High-precision bounding volume (double for camera-relative precision)
    BoundingVolume bounds;

    // When true, the parser pre-computed bounds incrementally during parsing and
    // finalizeSurfaceMesh should skip the separate computeBounds pass. Default
    // false so VTK/OBJ parsers fall through to computeBounds as before.
    bool hasBounds = false;

    // Optional dataset attributes (point/cell scalar maps)
    std::optional<DatasetAttributes> attributes;

    // Source metadata for the info panel
    std::string datasetType = ""; // VTK DATASET token (e.g. STRUCTURED_GRID) or "STL"
    std::string fileFormat  = ""; // "VTK" or "STL"

    // ponytail: true when the dataset is fundamentally a point set (POLYDATA
    // with only POINTS, or a STRUCTURED_POINTS grid) so the renderer draws
    // GL_POINTS instead of requiring triangle topology.
    bool renderAsPoints = false;

    // Structured grid dimensions (set for STRUCTURED_GRID / RECTILINEAR_GRID /
    // IMAGEDATA). Zero for unstructured/polydata meshes.
    int gridDimX = 0, gridDimY = 0, gridDimZ = 0;

    // Raw per-face corner positions (9 floats per triangle), captured BEFORE
    // the parser's position dedup. The mesh-quality analyzer welds these at
    // trimesh's 1e-8 tolerance to match script.py; the rendered indexed mesh
    // uses the (looser 1/4096) dedup for shading, so the two must stay separate.
    // For VTK/OBJ parsers, this is lazily computed on first access to avoid
    // the 3x memory cost when quality analysis is not needed.
    // Mutable because it is a lazy cache — logically derived from vertices+indices.
    mutable std::vector<float> flatVerts;

    // Lazily compute flatVerts from vertices+indices if not already populated.
    // Called by mesh_quality analysis. No-op if flatVerts is already filled.
    void ensureFlatVerts() const {
        if (!flatVerts.empty() || indices.empty() || vertices.empty()) return;
        flatVerts.reserve(indices.size() * 3);
        for (uint32_t idx : indices) {
            const float* p = &vertices[idx * 3];
            flatVerts.push_back(p[0]);
            flatVerts.push_back(p[1]);
            flatVerts.push_back(p[2]);
        }
    }

    // Per-rendered-vertex source index for scalar fields. Identity for the
    // original vertices; for duplicates created by computeNormals()'s
    // sharp-edge splitting, the index of the source vertex whose attributes
    // (scalar value) the duplicate carries. Populated by computeNormals();
    // empty when no split occurred (identity implied). Scalar re-uploads that
    // arrive in the pre-split per-node index space — e.g. a field read
    // straight from attributes->pointScalars — are gathered through this map
    // so the GPU scalar buffer always matches the post-split vertex count.
    std::vector<uint32_t> vertexSourceIndex;

    // True/topological point count of the source geometry — the number of
    // distinct vertices (after position dedup for STL). This is what tools like
    // ParaView report. It must be captured BEFORE computeNormals() splits sharp
    // edges, since that pass duplicates vertices purely for shading (one flat
    // normal per sharp-edge side) and would otherwise inflate the displayed
    // "Points" count. Defaults to the post-split vertex count if unset.
    int sourcePointCount = -1;

    // Cell-edge index list for cell-boundary wireframe (ParaView "Wireframe"
    // mode). Pairs of vertex indices into the same `vertices` array, emitted
    // in the pre-normal-split vertex space (indices remain valid after
    // computeNormals() because it only appends duplicate vertices). Populated
    // by extractCellEdges() / generateStructuredGridCellEdges() in VTK parsers,
    // or extractTriEdges() in STL/OBJ parsers. Empty when no cell topology is
    // available -- the renderer falls back to the triangle-edge (GL_LINE) approach.
    std::vector<uint32_t> cellEdgeIndices;

    // Geometry signature for animation fast path: hash of vertices+indices.
    // Computed once at parse time (FNV-1a, off-thread). Used to detect
    // topology-unchanged frames and route through updateScalars() instead of
    // full upload. 0 means not computed (e.g., empty mesh).
    uint64_t geometryHash = 0;
    // Scalar content hash for volume/glyph skip (1.3)
    uint64_t scalarHash = 0;
    uint64_t vectorHash = 0;
    // Estimated memory footprint for cache budgeting (bytes)
    size_t estimatedBytes = 0;

    // Default constructor
    RenderMesh() = default;
};

// ── Utility Functions (Shared by all parsers) ────────────────────────────────

namespace mesh_utils {
    // String processing helpers (heap-allocating, kept for backward compat)
    std::string trim(const std::string& s);
    std::string toUpper(const std::string& s);

    // ── Vertex-dedup position key (shared by STL / OBJ parsers) ──────────────
    // Quantize a coordinate into a signed fixed-point integer at a 1/4096-unit
    // tolerance (far finer than STL float precision). Clamped to ~25 bits of
    // magnitude so each axis value is bounded and the composite key below is stable.
    inline int64_t quantizedCoord(double v) {
        const int64_t q = 1 << 12; // 1/4096 unit tolerance
        int64_t ix = static_cast<int64_t>(std::llround(v * static_cast<double>(q)));
        const int64_t lim = (int64_t(1) << 25) - 1;
        if (ix > lim) ix = lim; else if (ix < -lim) ix = -lim;
        return ix;
    }

    // Collision-free position key: a tuple of the three quantized axis values is
    // exactly injective, so ONLY truly coincident (within tolerance) vertices
    // ever merge — the correct dedup semantics.
    using PositionKey = std::tuple<int64_t, int64_t, int64_t>;

    struct PositionKeyHash {
        size_t operator()(const PositionKey& k) const noexcept {
            // Hash the three bounded integers; collisions here only cost a bucket
            // comparison — correctness comes from tuple equality, not the hash.
            uint64_t h = static_cast<uint64_t>(std::get<0>(k)) * 73856093u;
            h ^= static_cast<uint64_t>(std::get<1>(k)) * 19349663u + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            h ^= static_cast<uint64_t>(std::get<2>(k)) * 83492791u + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            return static_cast<size_t>(h);
        }
    };

    inline PositionKey positionKey(float x, float y, float z) {
        return PositionKey{
            quantizedCoord(static_cast<double>(x)),
            quantizedCoord(static_cast<double>(y)),
            quantizedCoord(static_cast<double>(z))
        };
    }

    // Zero-allocation string helpers for hot parsing paths
    inline void toUpperInPlace(std::string& s) {
        for (char& c : s) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    }

    // Case-insensitive string comparison (no allocation)
    inline bool iequal(const std::string& a, const char* b) {
        size_t i = 0;
        for (; i < a.size() && b[i] != '\0'; ++i) {
            if (::toupper(static_cast<unsigned char>(a[i])) != ::toupper(static_cast<unsigned char>(b[i])))
                return false;
        }
        return i == a.size() && b[i] == '\0';
    }

    // Case-insensitive comparison for two string_views
    inline bool iequal(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (::toupper(static_cast<unsigned char>(a[i])) != ::toupper(static_cast<unsigned char>(b[i])))
                return false;
        }
        return true;
    }

    // Advance pointer past whitespace (zero allocation trim)
    inline const char* skipWhitespace(const char* p, const char* end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
        return p;
    }

    // Advance pointer to end of current token (non-whitespace)
    inline const char* skipToken(const char* p, const char* end) {
        while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') ++p;
        return p;
    }

    // Case-insensitive first-N-chars comparison (for keyword dispatch)
    inline bool iStartsWith(std::string_view input, std::string_view prefix) {
        if (input.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (::toupper(static_cast<unsigned char>(input[i])) != ::toupper(static_cast<unsigned char>(prefix[i])))
                return false;
        }
        return true;
    }

    // Endianness handling helpers
    bool isLittleEndian();
    void byteSwap(float* val);
    void byteSwap(double* val);
    void byteSwap(int* val);
    void byteSwap(uint32_t* val);
    void byteSwap(int16_t* val);
    void byteSwap(uint16_t* val);
    void byteSwap(uint8_t* val);
    void byteSwap(int64_t* val);
    void byteSwap(uint64_t* val);

    // Geometry & bounding math computations
    // Computes center, extent, and worldRadius from a flat vertex array
    void computeBounds(RenderMesh& mesh);

    void computeNormals(RenderMesh& mesh);

    // Animation fast path: geometry/content hashing (FNV-1a, off-thread)
    uint64_t hashBytes(const void* data, size_t len, uint64_t seed = 1469598103934665603ULL);
    uint64_t computeGeometryHash(const RenderMesh& mesh);
    uint64_t computeScalarHash(const std::vector<float>& scalars);
    uint64_t computeVectorHash(const RenderMesh& mesh);
    size_t computeEstimatedBytes(const RenderMesh& mesh);
    void finalizeGeometrySignatures(RenderMesh& mesh); // fills geometryHash/scalarHash/vectorHash/estimatedBytes

    // Convert a flat per-corner triangle soup (9 floats per triangle, may share
    // positions) into an indexed mesh by sort-based position dedup: groups
    // identical quantized positions, emits one vertex per unique position, and
    // fills the corner index buffer. O(N log N) sort beats per-vertex hash
    // lookups on large meshes (better cache behavior, no hash collision overhead).
    void indexFlatTriangles(const std::vector<float>& flatVerts,
                            std::vector<float>& outVertices,
                            std::vector<uint32_t>& outIndices);

    // Common tail for surface-mesh parsers (STL / OBJ): compute bounds, record
    // the topological point count (deduped, pre-normal-split — computeNormals
    // duplicates vertices at sharp edges which would otherwise inflate the
    // displayed "Points" value), tag dataset type/format, ensure normals exist,
    // and log a load summary.
    void finalizeSurfaceMesh(RenderMesh& mesh,
                             const std::string& datasetType,
                             const std::string& fileFormat,
                             const char* logLabel);

    // ── Fast ASCII numeric parsing ──────────────────────────────────────────────
    // Replaces std::istringstream/operator>> which carries per-token locale and
    // virtual-dispatch overhead. These use std::from_chars (zero locale, no
    // formatting state) for 5–20× speedup on ASCII data blocks.

    // Parse all whitespace-separated numeric values from a text buffer into `out`.
    // Appends to `out` (does not clear). Returns number of values parsed.
    template<typename T>
    inline size_t parseAsciiRange(const char* begin, const char* end, std::vector<T>& out) {
        size_t parsed = 0;
        const char* p = begin;
        while (p < end) {
            // skip whitespace
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
            if (p >= end) break;
            T val;
            auto [ptr, ec] = std::from_chars(p, end, val);
            if (ec != std::errc()) break;
            out.push_back(val);
            p = ptr;
            ++parsed;
        }
        return parsed;
    }

    // Read `count` ASCII numeric values from an ifstream, line by line via
    // std::getline (buffered) + from_chars. Returns number of values parsed.
    // Uses getline (not char-by-char get()) so the stream's internal buffer
    // handles buffering; from_chars handles the fast numeric scan per line.
    template<typename T>
    inline size_t readAsciiValues(std::ifstream& f, size_t count, std::vector<T>& out) {
        out.resize(count);
        size_t parsed = 0;
        std::string line;
        while (parsed < count && std::getline(f, line)) {
            const char* p = line.data();
            const char* end = p + line.size();
            while (parsed < count) {
                while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
                if (p >= end) break;
                T val;
                auto [ptr, ec] = std::from_chars(p, end, val);
                if (ec != std::errc()) break;
                out[parsed++] = val;
                p = ptr;
            }
        }
        if (parsed < count) out.resize(parsed);
        return parsed;
    }

    // Extrapolate cell-centered scalar/vector data to point data by averaging
    // over all cells incident to each point (one-ring weighting).
    // Pre-collects field metadata into a flat array to avoid per-cell hash
    // lookups, and computes contribution counts in a single vertex pass.
     void extrapolateCellDataToPoints(
        RenderMesh& mesh,
        const std::vector<std::vector<uint32_t>>& globalCellToVertices,
        const std::unordered_map<std::string, std::vector<float>>& cellScalarsStorage,
        const std::unordered_map<std::string, std::vector<float>>& cellVectorsStorage
    );

    // ── Cell-edge extraction (ParaView-style wireframe) ───────────────────────
    // ParaView "Surface Edges": only boundary face edges, interior volume faces
    // are hidden. We build a face → count map (sorted vertex tuple) and keep
    // only faces with count==1. Edges of those faces are then deduplicated.
    // For 2D cells (tri/quad/polygon) the face is the cell itself → always kept.
    // When cellType is 0 type is inferred from vertex count (2->line,3->tri,4->quad,8->hex).

    namespace detail {
        inline std::vector<std::vector<uint32_t>> cellBoundaryFaces(
            const std::vector<uint32_t>& v, int type) {
            std::vector<std::vector<uint32_t>> faces;
            switch (type) {
            case 1: case 2: break;
            case 3: case 4:
                for (size_t i = 0; i + 1 < v.size(); ++i) faces.push_back({v[i], v[i+1]});
                // For edge counting we treat lines as faces of 2 verts; kept as boundary
                return faces;
            case 5: // tri
                if (v.size()>=3) faces.push_back({v[0],v[1],v[2]});
                break;
            case 6: // strip -> each tri
                for (size_t i=0;i+2<v.size();++i) faces.push_back({v[i],v[i+1],v[i+2]});
                break;
            case 7: // polygon
                if (!v.empty()) faces.push_back(v);
                break;
            case 8: // pixel
                if (v.size()>=4) faces.push_back({v[0],v[1],v[3],v[2]});
                break;
            case 9: // quad
                if (v.size()>=4) faces.push_back({v[0],v[1],v[2],v[3]});
                break;
            case 10: // tetra 4 tris
                if (v.size()>=4) {
                    faces.push_back({v[0],v[1],v[2]});
                    faces.push_back({v[0],v[1],v[3]});
                    faces.push_back({v[1],v[2],v[3]});
                    faces.push_back({v[2],v[0],v[3]});
                }
                break;
            case 11: case 12: // hex/voxel 6 quads
                if (v.size()>=8) {
                    faces.push_back({v[0],v[1],v[2],v[3]});
                    faces.push_back({v[4],v[5],v[6],v[7]});
                    faces.push_back({v[0],v[1],v[5],v[4]});
                    faces.push_back({v[1],v[2],v[6],v[5]});
                    faces.push_back({v[2],v[3],v[7],v[6]});
                    faces.push_back({v[3],v[0],v[4],v[7]});
                }
                break;
            case 13: // wedge 2 tris +3 quads
                if (v.size()>=6) {
                    faces.push_back({v[0],v[1],v[2]});
                    faces.push_back({v[3],v[4],v[5]});
                    faces.push_back({v[0],v[1],v[4],v[3]});
                    faces.push_back({v[1],v[2],v[5],v[4]});
                    faces.push_back({v[2],v[0],v[3],v[5]});
                }
                break;
            case 14: // pyramid 1 quad +4 tris
                if (v.size()>=5) {
                    faces.push_back({v[0],v[1],v[2],v[3]});
                    faces.push_back({v[0],v[1],v[4]});
                    faces.push_back({v[1],v[2],v[4]});
                    faces.push_back({v[2],v[3],v[4]});
                    faces.push_back({v[3],v[0],v[4]});
                }
                break;
            case 15: // pent prism 2 pent +5 quads
                if (v.size()>=10) {
                    faces.push_back({v[0],v[1],v[2],v[3],v[4]});
                    faces.push_back({v[5],v[6],v[7],v[8],v[9]});
                    faces.push_back({v[0],v[1],v[6],v[5]});
                    faces.push_back({v[1],v[2],v[7],v[6]});
                    faces.push_back({v[2],v[3],v[8],v[7]});
                    faces.push_back({v[3],v[4],v[9],v[8]});
                    faces.push_back({v[0],v[4],v[9],v[5]});
                }
                break;
            default:
                if (!v.empty()) faces.push_back(v);
                break;
            }
            return faces;
        }
        inline std::vector<uint32_t> sortedKey(const std::vector<uint32_t>& f) {
            std::vector<uint32_t> k=f; std::sort(k.begin(), k.end()); return k;
        }
        struct VecHash { size_t operator()(const std::vector<uint32_t>& v) const noexcept {
            size_t h= v.size()* 1315423911u;
            for(uint32_t x: v) h ^= std::hash<uint32_t>{}(x) + 0x9e3779b9 + (h<<6) + (h>>2);
            return h; } };
    }

    // Extract deduplicated cell-boundary edges from cell-to-vertex connectivity.
    // For each cell, emits the topological edges according to the VTK cell type
    // (hex = 12 cube edges, tetra = 6, quad = 4 cycle, etc.). When cellType is 0
    // or the cellTypes vector is empty, type is inferred from the vertex count
    // (2->line, 3->triangle, 4->quad, 8->hex). Returns a flat uint32_t vector of
    // edge pairs (a,b), each pair sorted as (min,max) and globally deduplicated.
    // ParaView-style: only edges of boundary faces (face count==1) are kept, so
    // interior volume edges are hidden.
    template<typename IntType>
    std::vector<uint32_t> extractCellEdges(
        const std::vector<std::vector<uint32_t>>& cellToVertices,
        const std::vector<IntType>& cellTypes) {
        // Resolve types
        std::vector<int> resolvedTypes(cellToVertices.size(),0);
        for (size_t c=0;c<cellToVertices.size();++c) {
            const auto& v=cellToVertices[c];
            int type = (c < cellTypes.size()) ? static_cast<int>(cellTypes[c]) : 0;
            if (type==0) {
                switch (v.size()) { case 2:type=3;break; case 3:type=5;break; case 4:type=9;break; case 8:type=12;break; default:type=0;break; }
            }
            // tolerance for mislabeled types already handled in parser before this call
            resolvedTypes[c]=type;
        }
        // Face count map
        std::unordered_map<std::vector<uint32_t>, int, detail::VecHash> faceCount;
        faceCount.reserve(cellToVertices.size()*6);
        for (size_t c=0;c<cellToVertices.size();++c) {
            auto faces = detail::cellBoundaryFaces(cellToVertices[c], resolvedTypes[c]);
            for (auto &f: faces) {
                if (f.size()<3 && !(resolvedTypes[c]==3||resolvedTypes[c]==4)) continue;
                // For line cells, faces are edges (2 verts) -> treat as boundary always, skip counting
                if (f.size()==2) continue;
                auto k = detail::sortedKey(f);
                ++faceCount[k];
            }
        }
        std::vector<uint64_t> edgeKeys;
        edgeKeys.reserve(cellToVertices.size()*12);
        auto emitEdge = [&](uint32_t a, uint32_t b){ if(a>b) std::swap(a,b); edgeKeys.push_back((uint64_t(a)<<32)|b); };
        for (size_t c=0;c<cellToVertices.size();++c) {
            const auto& v=cellToVertices[c];
            int type=resolvedTypes[c];
            // Line cells: always emit
            if (type==3||type==4) {
                for(size_t i=0;i+1<v.size();++i) emitEdge(v[i], v[i+1]);
                continue;
            }
            auto faces = detail::cellBoundaryFaces(v, type);
            for (auto &f: faces) {
                if (f.size()<3) continue;
                auto k = detail::sortedKey(f);
                auto it = faceCount.find(k);
                if (it!=faceCount.end() && it->second!=1) continue; // interior face
                // Emit cycle edges of this boundary face
                for (size_t i=0;i<f.size();++i) emitEdge(f[i], f[(i+1)%f.size()]);
            }
            // For triangle/quad/polygon where face is the cell itself, the above already emitted.
            // For volume cells the per-face emission replaces the old per-cell 12/6/8 edge tables.
        }
        std::sort(edgeKeys.begin(), edgeKeys.end());
        edgeKeys.erase(std::unique(edgeKeys.begin(), edgeKeys.end()), edgeKeys.end());
        std::vector<uint32_t> out; out.reserve(edgeKeys.size()*2);
        for(uint64_t k: edgeKeys){ out.push_back(uint32_t(k>>32)); out.push_back(uint32_t(k&0xFFFFFFFF)); }
        return out;
    }

    // Extract deduplicated triangle edges from a flat triangle index list
    // (three indices per triangle). Each edge is stored as a sorted (min,max)
    // uint32_t pair and globally deduplicated. Used by STL/OBJ parsers that
    // lack original cell topology — the fallback to triangle-edge wireframe.
    std::vector<uint32_t> extractTriEdges(const std::vector<uint32_t>& indices);

}

// ── VTK XML Parser Definition ───────────────────────────────────────────────
// Parses VTK XML formats (.vtu / .vts / .vti / .vtp / .vtr) — ASCII and binary
// (inline base64 or appended base64).
RenderMesh parseVTKXML(const std::string& filePath);

// ── VTK MultiBlock XML Parser Definition ────────────────────────────────────
// Parses VTK MultiBlock files (.vtm) — references one or more .vtu/.vts/etc.
// datasets via <DataSet file="..."/> entries and merges them into one mesh.
RenderMesh parseMultiBlockXML(const std::string& filePath);

// ── RenderMesh merge utility ────────────────────────────────────────────────
// Concatenates vertices, indices, normals, scalars and vector data from
// multiple meshes into a single RenderMesh. Index buffers are rebased.
RenderMesh mergeRenderMeshes(const std::vector<RenderMesh>& meshes);

// ── VTK Legacy Parser Definition ────────────────────────────────────────────
// Parses Legacy VTK formats (supporting ASCII/BINARY and UNSTRUCTURED/STRUCTURED grids)
RenderMesh parseVTK(const std::string& filePath);

// ── STL Parser Definition ───────────────────────────────────────────────────
// Parses ASCII and Binary STL formats
RenderMesh parseSTL(const std::string& filePath);

// ── OBJ Parser Definition ───────────────────────────────────────────────────
// Parses Wavefront OBJ (.obj) — text `v` vertices + `f` faces.
RenderMesh parseOBJ(const std::string& filePath);

// ── Extension-based Dispatcher ──────────────────────────────────────────────
// Inspects the file extension (.vtk, .stl) and routes to the correct parser
RenderMesh loadMeshFile(const std::string& filePath);