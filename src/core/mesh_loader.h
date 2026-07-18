#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <optional>

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
    // Point data scalars (per-vertex)
    std::map<std::string, std::vector<float>> pointScalars;
    // Cell data scalars (per-cell, averaged to vertices during parsing)
    std::map<std::string, std::vector<float>> cellScalars;
    // VTK VECTORS — per-point 3-component, interleaved [x,y,z]
    std::map<std::string, std::vector<float>> pointVectors;

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
    bool meshHasVectors() const { return !pointVectorsData.empty(); }

    // Resolves a vector field's contiguous data. Returns the base pointer and
    // the number of vec3 elements, or (nullptr, 0) when the field is unknown.
    const glm::vec3* vectorFieldData(const std::string& name, size_t& count) const {
        auto it = pointVectorOffset.find(name);
        if (it == pointVectorOffset.end()) { count = 0; return nullptr; }
        count = pointVectorsData.size() - it->second;
        // A field always spans a full per-vertex count; clamp to that if the
        // buffer holds trailing runs of other fields.
        const size_t perVertex = vertices.empty() ? 0 : vertices.size() / 3;
        if (perVertex != 0 && count > perVertex) count = perVertex;
        return pointVectorsData.data() + it->second;
    }

    // High-precision bounding volume (double for camera-relative precision)
    BoundingVolume bounds;

    // Optional dataset attributes (point/cell scalar maps)
    std::optional<DatasetAttributes> attributes;

    // Source metadata for the info panel
    std::string datasetType = ""; // VTK DATASET token (e.g. STRUCTURED_GRID) or "STL"
    std::string fileFormat  = ""; // "VTK" or "STL"

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
    // String processing helpers
    std::string trim(const std::string& s);
    std::string toUpper(const std::string& s);

    // Endianness handling helpers
    bool isLittleEndian();
    void byteSwap(float* val);
    void byteSwap(double* val);
    void byteSwap(int* val);
    void byteSwap(int16_t* val);
    void byteSwap(uint16_t* val);
    void byteSwap(uint8_t* val);
    void byteSwap(int64_t* val);
    void byteSwap(uint64_t* val);

    // Geometry & bounding math computations
    // Computes center, extent, and worldRadius from a flat vertex array
    void computeBounds(RenderMesh& mesh);

    // Computes smooth per-vertex normals from indexed geometry configurations
    void computeNormals(RenderMesh& mesh);
}

// ── VTK Parser Definition ───────────────────────────────────────────────────
// Parses Legacy VTK formats (supporting ASCII/BINARY and UNSTRUCTURED/STRUCTURED grids)
RenderMesh parseVTK(const std::string& filePath);

// ── STL Parser Definition ───────────────────────────────────────────────────
// Parses ASCII and Binary STL formats
RenderMesh parseSTL(const std::string& filePath);

// ── Extension-based Dispatcher ──────────────────────────────────────────────
// Inspects the file extension (.vtk, .stl) and routes to the correct parser
RenderMesh loadMeshFile(const std::string& filePath);