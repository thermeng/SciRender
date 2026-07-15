#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <optional>

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
    // ponytail: VTK VECTORS — per-point 3-component, interleaved [x,y,z]
    std::map<std::string, std::vector<float>> pointVectors;

    // Global scalar range boundaries (Required by renderer & color LUT mapping)
    float scalarMin = 0.0f;
    float scalarMax = 1.0f;
};

// ── Render Mesh (GPU-Facing, Clean Geometry) ────────────────────────────────

struct RenderMesh {
    // Core GPU arrays (float for GPU upload)
    std::vector<float> vertices;   // x,y,z interleaved
    std::vector<int> indices;      // Triangle indices
    std::vector<float> normals;    // nx,ny,nz interleaved
    std::vector<float> scalars;    // Active scalar field (per-vertex, optional)
    std::string scalarName = "";   // Name of active scalar field
    std::vector<std::string> availableScalarNames; // ponytail: all point-scalar field names for the QML switcher

    // ponytail: VTK VECTORS — unrolled so it aligns with unrolled `vertices`
    std::map<std::string, std::vector<float>> pointVectors; // interleaved [x,y,z] per unrolled vertex
    std::vector<std::string> availableVectorNames;          // for a QML switcher
    std::string vectorName = "";                            // active vector field name
    bool meshHasVectors() const { return !pointVectors.empty(); }

    // High-precision bounding volume (double for camera-relative precision)
    BoundingVolume bounds;

    // Optional dataset attributes (point/cell scalar maps)
    std::optional<DatasetAttributes> attributes;

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
    void byteSwap(int* val);

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