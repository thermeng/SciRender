#pragma once

// ── Shared VTK adapter tail ──────────────────────────────────────────────────
// Single home for the finalize + triangulation logic that both VTK adapters
// (legacy .vtk in vtk_parser.cpp, XML family in vtk_xml_parser.cpp) previously
// carried as copy-pasted twins. The copies had drifted (int32 vs int64
// connectivity keys; the XML twin dropped cell vectors, cell centers and the
// active-scalar range at finalize) — this module closes that churn surface.
//
// Both adapters produce the same Mesh data model; everything here operates on
// RenderMesh + plain storage maps so neither adapter needs to expose internals.

#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/mesh_loader.h"

namespace vtk_common {

// ── Polygon / line / strip triangulation ─────────────────────────────────────
// All take raw "[count p0 p1 ...]" records and append to mesh.indices while
// returning one entry per emitted cell (per-triangle for strips).

template<typename IntType>
std::vector<std::vector<uint32_t>> triangulatePolygons(RenderMesh& mesh,
                                                       const std::vector<IntType>& rawPolygonData,
                                                       int numPolys);

template<typename IntType>
std::vector<std::vector<uint32_t>> triangulateLines(RenderMesh& mesh,
                                                    const std::vector<IntType>& rawLineData,
                                                    int numLines);

template<typename IntType>
std::vector<std::vector<uint32_t>> triangulateTriangleStrips(RenderMesh& mesh,
                                                             const std::vector<IntType>& rawStripData,
                                                             int numStrips);

// Unstructured cells: flat "[count v0 ...]" records + per-cell VTK type codes.
// Emits surface triangles for volumetric cell types (tetra/hex/pyramid/wedge/
// prism/voxel/pixel/quad), fan-triangulates polygons, skips higher-order
// types >= 21. Returns one vertex list per input cell (volumetric types keep
// ALL corners — this feeds cell-data extrapolation and cell-center placement;
// strips expand to one entry per triangle).
template<typename IntType>
std::vector<std::vector<uint32_t>> triangulateUnstructuredCells(
    RenderMesh& mesh,
    const std::vector<IntType>& rawCellData,
    const std::vector<IntType>& cellTypes,
    int totalCells);

// ── Structured grid topology ─────────────────────────────────────────────────

// Boundary quads of a dX×dY×dZ structured grid (2D grids emit their single
// quad plane). Appends 6 indices per quad; returns one 4-vertex list per quad.
std::vector<std::vector<uint32_t>> generateStructuredGridSurface(RenderMesh& mesh,
                                                                 int dX, int dY, int dZ);

// Volume cells of a dX×dY×dZ structured grid: hexes in 3D, quads when an axis
// has a single node. One 8-vertex (or 4-vertex) list per cell.
std::vector<std::vector<uint32_t>> generateStructuredGridCells(int dX, int dY, int dZ);

// ── Finalize ─────────────────────────────────────────────────────────────────

// Everything finalizeVTKMesh reads out of an adapter's parse context. The two
// storage maps are the pre-extrapolation CELL_DATA arrays keyed by field name.
struct FinalizeContext {
    const std::vector<std::vector<uint32_t>>& globalCellToVertices;
    const std::unordered_map<std::string, std::vector<float>>& cellScalarsStorage;
    const std::unordered_map<std::string, std::vector<float>>& cellVectorsStorage;
    // Message prefix for errors/warnings ("VTK Parser" / "VTK XML Parser").
    const char* logLabel;
    const std::string& datasetType;
};

// Post-parse tail shared by both adapters, in fixed order: empty/topology
// validation → out-of-range index guard → cell-data extrapolation to points →
// point-vector flattening into the contiguous vec3 buffer → cell-vector
// flattening (+ names/defaults/count) → cell centers → active-scalar
// selection → sorted field-name lists → default vector field → scalar range →
// bounds → sourcePointCount → normals.
void finalizeVTKMesh(RenderMesh& mesh, const FinalizeContext& ctx);

// Active point-scalar min/max on mesh.attributes (allocates attributes when
// absent; bumps max by +1 for a flat field). Called by finalizeVTKMesh.
void calculateScalarRanges(RenderMesh& mesh);

// ── Template implementations ─────────────────────────────────────────────────

template<typename IntType>
std::vector<std::vector<uint32_t>> triangulatePolygons(RenderMesh& mesh,
                                                       const std::vector<IntType>& rawPolygonData,
                                                       int numPolys) {
    int idx = 0;
    std::vector<std::vector<uint32_t>> cellToVertices(numPolys);
    for (int p = 0; p < numPolys; ++p) {
        if (idx >= static_cast<int>(rawPolygonData.size())) break;
        int nPoints = static_cast<int>(rawPolygonData[idx++]);
        if (nPoints < 0 || idx + nPoints > static_cast<int>(rawPolygonData.size())) break;

        for (int i = 0; i < nPoints; ++i) {
            cellToVertices[p].push_back(static_cast<uint32_t>(rawPolygonData[idx + i]));
        }

        for (int i = 1; i < nPoints - 1; ++i) {
            mesh.indices.push_back(static_cast<uint32_t>(rawPolygonData[idx + 0]));
            mesh.indices.push_back(static_cast<uint32_t>(rawPolygonData[idx + i]));
            mesh.indices.push_back(static_cast<uint32_t>(rawPolygonData[idx + i + 1]));
        }
        idx += nPoints;
    }
    return cellToVertices;
}

template<typename IntType>
std::vector<std::vector<uint32_t>> triangulateLines(RenderMesh& mesh,
                                                    const std::vector<IntType>& rawLineData,
                                                    int numLines) {
    int idx = 0;
    std::vector<std::vector<uint32_t>> cellToVertices(numLines);
    for (int l = 0; l < numLines; ++l) {
        if (idx >= static_cast<int>(rawLineData.size())) break;
        int nPoints = static_cast<int>(rawLineData[idx++]);
        if (nPoints < 0 || idx + nPoints > static_cast<int>(rawLineData.size())) break;

        for (int i = 0; i < nPoints; ++i) {
            cellToVertices[l].push_back(static_cast<uint32_t>(rawLineData[idx + i]));
        }

        for (int i = 0; i + 1 < nPoints; ++i) {
            uint32_t a = static_cast<uint32_t>(rawLineData[idx + i]);
            uint32_t b = static_cast<uint32_t>(rawLineData[idx + i + 1]);
            mesh.indices.insert(mesh.indices.end(), { a, b });
        }
        idx += nPoints;
    }
    return cellToVertices;
}

template<typename IntType>
std::vector<std::vector<uint32_t>> triangulateTriangleStrips(RenderMesh& mesh,
                                                             const std::vector<IntType>& rawStripData,
                                                             int numStrips) {
    // A triangle strip is a BAND of triangles, not one polygon. Store each
    // triangle as its own 3-vertex cell so the cyclic cell-edge emitter draws
    // true per-triangle boundaries (3 edges, no diagonal, no spurious wrap
    // edge). Keeping the whole strip as a single "cell" makes the emitter wrap
    // with a long v_{N-1}->v0 edge across the entire strip — garbage on strip
    // meshes (e.g. lung.vtk).
    std::vector<std::vector<uint32_t>> cellToVertices;
    cellToVertices.reserve(numStrips > 0 ? numStrips * 3 : 0);
    int idx = 0;
    for (int s = 0; s < numStrips; ++s) {
        if (idx >= static_cast<int>(rawStripData.size())) break;
        int nPoints = static_cast<int>(rawStripData[idx++]);
        if (nPoints < 0 || idx + nPoints > static_cast<int>(rawStripData.size())) break;

        for (int i = 0; i < nPoints - 2; ++i) {
            uint32_t i0 = static_cast<uint32_t>(rawStripData[idx + i]);
            uint32_t i1 = static_cast<uint32_t>(rawStripData[idx + i + 1]);
            uint32_t i2 = static_cast<uint32_t>(rawStripData[idx + i + 2]);
            // triangle as a cell (winding only affects the cyclic edge order;
            // all 3 edges are emitted either way)
            cellToVertices.push_back({ i0, i1, i2 });
            if (i % 2 == 0) mesh.indices.insert(mesh.indices.end(), { i0, i1, i2 });
            else mesh.indices.insert(mesh.indices.end(), { i0, i2, i1 });
        }
        idx += nPoints;
    }
    return cellToVertices;
}

template<typename IntType>
std::vector<std::vector<uint32_t>> triangulateUnstructuredCells(
    RenderMesh& mesh,
    const std::vector<IntType>& rawCellData,
    const std::vector<IntType>& cellTypes,
    int totalCells) {
    mesh.indices.clear();
    std::vector<std::vector<uint32_t>> cellToVertices(totalCells);
    int idx = 0;
    for (int c = 0; c < totalCells; ++c) {
        if (idx >= static_cast<int>(rawCellData.size())) break;
        int numPointsInCell = static_cast<int>(rawCellData[idx++]);
        // Guard against a malformed/truncated cell claiming more points than
        // the buffer holds — reading past the end is UB / a crash.
        if (numPointsInCell < 0 || idx + numPointsInCell > static_cast<int>(rawCellData.size())) break;

        for (int i = 0; i < numPointsInCell; ++i) {
            cellToVertices[c].push_back(static_cast<uint32_t>(rawCellData[idx + i]));
        }

        int type = (c < static_cast<int>(cellTypes.size())) ? static_cast<int>(cellTypes[c]) : 0;
        if (type == 0) {
            if (numPointsInCell == 3) type = 5;   // VTK_TRIANGLE
            if (numPointsInCell == 4) type = 9;   // VTK_QUAD
            if (numPointsInCell == 8) type = 12;  // VTK_HEXAHEDRON
        }

        switch (type) {
        case 1:  // VTK_VERTEX
        case 2:  // VTK_POLY_VERTEX
        case 3:  // VTK_LINE
        case 4:  // VTK_POLY_LINE
            break;
        case 5: // VTK_TRIANGLE
            if (idx + 2 < static_cast<int>(rawCellData.size())) {
                mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + 0]));
                mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + 1]));
                mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + 2]));
            }
            break;
        case 6: { // VTK_TRIANGLE_STRIP
            for (int i = 0; i + 2 < numPointsInCell; ++i) {
                if (idx + i + 2 < static_cast<int>(rawCellData.size())) {
                    if (i & 1) {
                        mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i + 1]));
                        mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i]));
                        mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i + 2]));
                    } else {
                        mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i]));
                        mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i + 1]));
                        mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i + 2]));
                    }
                }
            }
            break;
        }
        case 7: // VTK_POLYGON (triangle fan)
            for (int i = 1; i < numPointsInCell - 1; ++i) {
                if (idx + i + 1 < static_cast<int>(rawCellData.size())) {
                    mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + 0]));
                    mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i]));
                    mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i + 1]));
                }
            }
            break;
        case 8: { // VTK_PIXEL (4 pts: BL, BR, TL, TR)
            if (idx + 3 < static_cast<int>(rawCellData.size())) {
                uint32_t i0 = static_cast<uint32_t>(rawCellData[idx + 0]), i1 = static_cast<uint32_t>(rawCellData[idx + 1]);
                uint32_t i2 = static_cast<uint32_t>(rawCellData[idx + 2]), i3 = static_cast<uint32_t>(rawCellData[idx + 3]);
                mesh.indices.insert(mesh.indices.end(), { i0, i2, i1, i0, i1, i3 });
            }
            break;
        }
        case 9: // VTK_QUAD
            if (idx + 3 < static_cast<int>(rawCellData.size())) {
                uint32_t i0 = static_cast<uint32_t>(rawCellData[idx + 0]), i1 = static_cast<uint32_t>(rawCellData[idx + 1]);
                uint32_t i2 = static_cast<uint32_t>(rawCellData[idx + 2]), i3 = static_cast<uint32_t>(rawCellData[idx + 3]);
                mesh.indices.insert(mesh.indices.end(), { i0, i1, i2, i0, i2, i3 });
            }
            break;
        case 10: { // VTK_TETRA
            if (idx + 3 < static_cast<int>(rawCellData.size())) {
                uint32_t i0 = static_cast<uint32_t>(rawCellData[idx + 0]), i1 = static_cast<uint32_t>(rawCellData[idx + 1]);
                uint32_t i2 = static_cast<uint32_t>(rawCellData[idx + 2]), i3 = static_cast<uint32_t>(rawCellData[idx + 3]);
                mesh.indices.insert(mesh.indices.end(), {
                    i0, i2, i1,   i0, i1, i3,
                    i1, i2, i3,   i2, i0, i3
                });
            }
            break;
        }
        case 11: { // VTK_VOXEL — structured-grid corner ordering, permute to HEX (0,1,3,2,4,5,7,6)
            if (idx + 7 < static_cast<int>(rawCellData.size())) {
                uint32_t h0 = static_cast<uint32_t>(rawCellData[idx + 0]), h1 = static_cast<uint32_t>(rawCellData[idx + 1]);
                uint32_t h2 = static_cast<uint32_t>(rawCellData[idx + 3]), h3 = static_cast<uint32_t>(rawCellData[idx + 2]);
                uint32_t h4 = static_cast<uint32_t>(rawCellData[idx + 4]), h5 = static_cast<uint32_t>(rawCellData[idx + 5]);
                uint32_t h6 = static_cast<uint32_t>(rawCellData[idx + 7]), h7 = static_cast<uint32_t>(rawCellData[idx + 6]);
                cellToVertices[c] = {h0, h1, h2, h3, h4, h5, h6, h7};
                mesh.indices.insert(mesh.indices.end(), {
                    h0, h3, h1, h1, h3, h2, h4, h5, h7, h5, h6, h7,
                    h0, h1, h4, h1, h5, h4, h2, h3, h6, h3, h7, h6,
                    h0, h4, h3, h3, h4, h7, h1, h2, h5, h2, h6, h5
                });
            }
            break;
        }
        case 12: // VTK_HEXAHEDRON
            if (idx + 7 < static_cast<int>(rawCellData.size())) {
                uint32_t i0 = static_cast<uint32_t>(rawCellData[idx + 0]), i1 = static_cast<uint32_t>(rawCellData[idx + 1]);
                uint32_t i2 = static_cast<uint32_t>(rawCellData[idx + 2]), i3 = static_cast<uint32_t>(rawCellData[idx + 3]);
                uint32_t i4 = static_cast<uint32_t>(rawCellData[idx + 4]), i5 = static_cast<uint32_t>(rawCellData[idx + 5]);
                uint32_t i6 = static_cast<uint32_t>(rawCellData[idx + 6]), i7 = static_cast<uint32_t>(rawCellData[idx + 7]);
                mesh.indices.insert(mesh.indices.end(), {
                    i0, i3, i1, i1, i3, i2, i4, i5, i7, i5, i6, i7,
                    i0, i1, i4, i1, i5, i4, i2, i3, i6, i3, i7, i6,
                    i0, i4, i3, i3, i4, i7, i1, i2, i5, i2, i6, i5
                });
            }
            break;
        case 13: { // VTK_WEDGE
            if (idx + 5 < static_cast<int>(rawCellData.size())) {
                uint32_t i0 = static_cast<uint32_t>(rawCellData[idx + 0]), i1 = static_cast<uint32_t>(rawCellData[idx + 1]);
                uint32_t i2 = static_cast<uint32_t>(rawCellData[idx + 2]);
                uint32_t i3 = static_cast<uint32_t>(rawCellData[idx + 3]), i4 = static_cast<uint32_t>(rawCellData[idx + 4]);
                uint32_t i5 = static_cast<uint32_t>(rawCellData[idx + 5]);
                mesh.indices.insert(mesh.indices.end(), {
                    i0, i2, i1,
                    i3, i4, i5,
                    i0, i1, i4, i0, i4, i3,
                    i1, i2, i5, i1, i5, i4,
                    i2, i0, i3, i2, i3, i5
                });
            }
            break;
        }
        case 14: { // VTK_PYRAMID
            if (idx + 4 < static_cast<int>(rawCellData.size())) {
                uint32_t i0 = static_cast<uint32_t>(rawCellData[idx + 0]), i1 = static_cast<uint32_t>(rawCellData[idx + 1]);
                uint32_t i2 = static_cast<uint32_t>(rawCellData[idx + 2]), i3 = static_cast<uint32_t>(rawCellData[idx + 3]);
                uint32_t i4 = static_cast<uint32_t>(rawCellData[idx + 4]);
                mesh.indices.insert(mesh.indices.end(), {
                    i0, i2, i1, i0, i3, i2,
                    i0, i1, i4, i1, i2, i4, i2, i3, i4, i3, i0, i4
                });
            }
            break;
        }
        case 15: { // VTK_PENTAGONAL_PRISM (10 vertices)
            if (idx + 9 < static_cast<int>(rawCellData.size())) {
                uint32_t v0 = static_cast<uint32_t>(rawCellData[idx + 0]), v1 = static_cast<uint32_t>(rawCellData[idx + 1]);
                uint32_t v2 = static_cast<uint32_t>(rawCellData[idx + 2]), v3 = static_cast<uint32_t>(rawCellData[idx + 3]);
                uint32_t v4 = static_cast<uint32_t>(rawCellData[idx + 4]);
                uint32_t v5 = static_cast<uint32_t>(rawCellData[idx + 5]), v6 = static_cast<uint32_t>(rawCellData[idx + 6]);
                uint32_t v7 = static_cast<uint32_t>(rawCellData[idx + 7]), v8 = static_cast<uint32_t>(rawCellData[idx + 8]);
                uint32_t v9 = static_cast<uint32_t>(rawCellData[idx + 9]);
                cellToVertices[c] = {v0, v1, v2, v3, v4, v5, v6, v7, v8, v9};
                mesh.indices.insert(mesh.indices.end(), {
                    v5, v7, v6, v5, v8, v7, v5, v9, v8,
                    v4, v2, v3, v4, v3, v1, v4, v1, v0,
                    v0, v1, v6, v0, v6, v5,
                    v1, v2, v7, v1, v7, v6,
                    v2, v3, v8, v2, v8, v7,
                    v3, v4, v9, v3, v9, v8,
                    v0, v5, v9, v0, v9, v4
                });
            }
            break;
        }
        default:
            if (type >= 21) {
                // Higher-order cells need subdivision/refinement; fan-
                // triangulating them produces garbage geometry, so skip.
                std::cerr << "VTK Parser Warning: unsupported higher-order cell type "
                          << type << " (requires subdivision); skipping." << std::endl;
            } else {
                for (int i = 1; i < numPointsInCell - 1; ++i) {
                    if (idx + i + 1 < static_cast<int>(rawCellData.size())) {
                        mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + 0]));
                        mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i]));
                        mesh.indices.push_back(static_cast<uint32_t>(rawCellData[idx + i + 1]));
                    }
                }
            }
            break;
        }
        idx += numPointsInCell;
    }
    return cellToVertices;
}

} // namespace vtk_common
