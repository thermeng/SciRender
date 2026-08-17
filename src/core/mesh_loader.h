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

    // Global scalar range boundaries (Required by renderer & color LUT mapping)
    float scalarMin = 0.0f;
    float scalarMax = 1.0f;
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

    // True/topological point count of the source geometry — the number of
    // distinct vertices (after position dedup for STL). This is what tools like
    // ParaView report. It must be captured BEFORE computeNormals() splits sharp
    // edges, since that pass duplicates vertices purely for shading (one flat
    // normal per sharp-edge side) and would otherwise inflate the displayed
    // "Points" count. Defaults to the post-split vertex count if unset.
    int sourcePointCount = -1;

    // Default constructor
    RenderMesh() = default;
};

// ── Utility Functions (Shared by all parsers) ────────────────────────────────

namespace mesh_utils {
    // String processing helpers (heap-allocating, kept for backward compat)
    std::string trim(const std::string& s);
    std::string toUpper(const std::string& s);

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

    // Fast double-parse from a text buffer (VTK legacy SCALARS/VECTORS use double
    // in the ifstream >> path; from_chars handles double directly).
    template<typename T>
    inline size_t parseAsciiRangeFloat(const char* begin, const char* end, std::vector<T>& out) {
        return parseAsciiRange<T>(begin, end, out);
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

    // Read ASCII integer values from an ifstream (16-bit path for SHORT types).
    inline size_t readAsciiValues16(std::ifstream& f, size_t count, std::vector<int16_t>& out) {
        return readAsciiValues(f, count, out);
    }

    // Read ASCII unsigned char values from an ifstream.
    inline size_t readAsciiValuesU8(std::ifstream& f, size_t count, std::vector<uint8_t>& out) {
        return readAsciiValues(f, count, out);
    }

    // Read ASCII int64 / uint64 values from an ifstream.
    inline size_t readAsciiValues64(std::ifstream& f, size_t count, std::vector<int64_t>& out) {
        return readAsciiValues(f, count, out);
    }

    // Read ASCII uint32 values from an ifstream.
    inline size_t readAsciiValuesU32(std::ifstream& f, size_t count, std::vector<uint32_t>& out) {
        return readAsciiValues(f, count, out);
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