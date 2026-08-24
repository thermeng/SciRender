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
#include <algorithm>

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
    std::vector<int> resolvedTypes(totalCells, 0);
    int idx = 0;
    for (int c = 0; c < totalCells; ++c) {
        if (idx >= static_cast<int>(rawCellData.size())) break;
        int numPointsInCell = static_cast<int>(rawCellData[idx++]);
        if (numPointsInCell < 0 || idx + numPointsInCell > static_cast<int>(rawCellData.size())) break;
        for (int i = 0; i < numPointsInCell; ++i) {
            cellToVertices[c].push_back(static_cast<uint32_t>(rawCellData[idx + i]));
        }
        int type = (c < static_cast<int>(cellTypes.size())) ? static_cast<int>(cellTypes[c]) : 0;
        if (type == 0) {
            if (numPointsInCell == 3) type = 5;
            if (numPointsInCell == 4) type = 9;
            if (numPointsInCell == 8) type = 12;
        }
        if (type == 11 && numPointsInCell == 4) {
            std::cerr << "VTK Parser Warning: cell " << c << " claims VOXEL(11) but has 4 pts -> treating as TETRA(10)" << std::endl;
            type = 10;
        } else if (type == 10 && numPointsInCell == 8) {
            std::cerr << "VTK Parser Warning: cell " << c << " claims TETRA(10) but has 8 pts -> treating as HEXAHEDRON(12)" << std::endl;
            type = 12;
        } else if (type == 13 && numPointsInCell == 5) {
            std::cerr << "VTK Parser Warning: cell " << c << " claims WEDGE(13) but has 5 pts -> treating as PYRAMID(14)" << std::endl;
            type = 14;
        } else if (type == 14 && numPointsInCell == 6) {
            std::cerr << "VTK Parser Warning: cell " << c << " claims PYRAMID(14) but has 6 pts -> treating as WEDGE(13)" << std::endl;
            type = 13;
        }
        resolvedTypes[c] = type;
        if (type == 11 && numPointsInCell >= 8) {
            uint32_t h0 = cellToVertices[c][0], h1 = cellToVertices[c][1];
            uint32_t h2 = cellToVertices[c][3], h3 = cellToVertices[c][2];
            uint32_t h4 = cellToVertices[c][4], h5 = cellToVertices[c][5];
            uint32_t h6 = cellToVertices[c][7], h7 = cellToVertices[c][6];
            cellToVertices[c] = {h0,h1,h2,h3,h4,h5,h6,h7};
            resolvedTypes[c] = 12;
        }
        idx += numPointsInCell;
    }
    // ParaView surface-only: face → count map
    auto sortedKey = [](const std::vector<uint32_t>& f){ std::vector<uint32_t> k=f; std::sort(k.begin(),k.end()); return k; };
    struct VecHash { size_t operator()(const std::vector<uint32_t>& v) const noexcept {
        size_t h=v.size()*1315423911u; for(uint32_t x: v) h ^= std::hash<uint32_t>{}(x)+0x9e3779b9+(h<<6)+(h>>2); return h; } };
    std::unordered_map<std::vector<uint32_t>, int, VecHash> faceCount;
    faceCount.reserve(totalCells*6);
    for (int c=0;c<totalCells;++c) {
        if (cellToVertices[c].empty()) continue;
        int type = resolvedTypes[c];
        const auto& v = cellToVertices[c];
        std::vector<std::vector<uint32_t>> faces;
        switch (type) {
        case 5: if(v.size()>=3) faces.push_back({v[0],v[1],v[2]}); break;
        case 7: if(!v.empty()) faces.push_back(v); break;
        case 8: if(v.size()>=4) faces.push_back({v[0],v[1],v[3],v[2]}); break;
        case 9: if(v.size()>=4) faces.push_back({v[0],v[1],v[2],v[3]}); break;
        case 10: if(v.size()>=4) { faces.push_back({v[0],v[1],v[2]}); faces.push_back({v[0],v[1],v[3]}); faces.push_back({v[1],v[2],v[3]}); faces.push_back({v[2],v[0],v[3]}); } break;
        case 12: if(v.size()>=8) { faces.push_back({v[0],v[1],v[2],v[3]}); faces.push_back({v[4],v[5],v[6],v[7]}); faces.push_back({v[0],v[1],v[5],v[4]}); faces.push_back({v[1],v[2],v[6],v[5]}); faces.push_back({v[2],v[3],v[7],v[6]}); faces.push_back({v[3],v[0],v[4],v[7]}); } break;
        case 13: if(v.size()>=6) { faces.push_back({v[0],v[1],v[2]}); faces.push_back({v[3],v[4],v[5]}); faces.push_back({v[0],v[1],v[4],v[3]}); faces.push_back({v[1],v[2],v[5],v[4]}); faces.push_back({v[2],v[0],v[3],v[5]}); } break;
        case 14: if(v.size()>=5) { faces.push_back({v[0],v[1],v[2],v[3]}); faces.push_back({v[0],v[1],v[4]}); faces.push_back({v[1],v[2],v[4]}); faces.push_back({v[2],v[3],v[4]}); faces.push_back({v[3],v[0],v[4]}); } break;
        case 15: if(v.size()>=10) { faces.push_back({v[0],v[1],v[2],v[3],v[4]}); faces.push_back({v[5],v[6],v[7],v[8],v[9]}); faces.push_back({v[0],v[1],v[6],v[5]}); faces.push_back({v[1],v[2],v[7],v[6]}); faces.push_back({v[2],v[3],v[8],v[7]}); faces.push_back({v[3],v[4],v[9],v[8]}); faces.push_back({v[0],v[4],v[9],v[5]}); } break;
        case 6: for(size_t i=0;i+2<v.size();++i) faces.push_back({v[i],v[i+1],v[i+2]}); break;
        default: if(!v.empty() && v.size()>=3) faces.push_back(v); break;
        }
        for (auto &f: faces) if(f.size()>=3) ++faceCount[sortedKey(f)];
    }
    auto isBoundary = [&](const std::vector<uint32_t>& f)->bool{
        if(f.size()<3) return false;
        auto it = faceCount.find(sortedKey(f));
        return it!=faceCount.end() && it->second==1;
    };
    // Emit only boundary faces, preserving original triangulation
    for (int c=0;c<totalCells;++c) {
        if (cellToVertices[c].empty()) continue;
        int type = resolvedTypes[c];
        const auto& v = cellToVertices[c];
        if (type>=21) {
            std::cerr << "VTK Parser Warning: unsupported higher-order cell type " << type << " (requires subdivision); skipping." << std::endl;
            continue;
        }
        if (type==1||type==2||type==3||type==4) continue;
        switch (type) {
        case 5: {
            std::vector<uint32_t> f={v[0],v[1],v[2]};
            if(isBoundary(f)) { mesh.indices.push_back(v[0]); mesh.indices.push_back(v[1]); mesh.indices.push_back(v[2]); }
            break;
        }
        case 7: {
            std::vector<uint32_t> f=v;
            if(isBoundary(f)) {
                for(size_t i=1;i+1<f.size();++i){ mesh.indices.push_back(f[0]); mesh.indices.push_back(f[i]); mesh.indices.push_back(f[i+1]); }
            }
            break;
        }
        case 8: {
            std::vector<uint32_t> f={v[0],v[1],v[3],v[2]};
            if(isBoundary(f)) { mesh.indices.insert(mesh.indices.end(), {v[0],v[2],v[1], v[0],v[1],v[3]}); }
            break;
        }
        case 9: {
            std::vector<uint32_t> f={v[0],v[1],v[2],v[3]};
            if(isBoundary(f)) { mesh.indices.insert(mesh.indices.end(), {v[0],v[1],v[2], v[0],v[2],v[3]}); }
            break;
        }
        case 10: {
            std::vector<std::vector<uint32_t>> faces={{v[0],v[1],v[2]},{v[0],v[1],v[3]},{v[1],v[2],v[3]},{v[2],v[0],v[3]}};
            std::vector<std::array<uint32_t,3>> tris={{v[0],v[2],v[1]},{v[0],v[1],v[3]},{v[1],v[2],v[3]},{v[2],v[0],v[3]}};
            for(size_t fi=0;fi<faces.size();++fi) if(isBoundary(faces[fi])) {
                mesh.indices.push_back(tris[fi][0]); mesh.indices.push_back(tris[fi][1]); mesh.indices.push_back(tris[fi][2]);
            }
            break;
        }
        case 12: {
            std::vector<std::vector<uint32_t>> faces={{v[0],v[1],v[2],v[3]},{v[4],v[5],v[6],v[7]},{v[0],v[1],v[5],v[4]},{v[1],v[2],v[6],v[5]},{v[2],v[3],v[7],v[6]},{v[3],v[0],v[4],v[7]}};
            // Old triangulations per face
            std::vector<std::pair<std::vector<uint32_t>, std::vector<uint32_t>>> hexTrisPerFace={
                {{v[0],v[3],v[1]}, {v[1],v[3],v[2]}},
                {{v[4],v[5],v[7]}, {v[5],v[6],v[7]}},
                {{v[0],v[1],v[4]}, {v[1],v[5],v[4]}},
                {{v[1],v[2],v[5]}, {v[2],v[6],v[5]}},
                {{v[2],v[3],v[6]}, {v[3],v[7],v[6]}},
                {{v[0],v[4],v[3]}, {v[3],v[4],v[7]}}
            };
            for(size_t fi=0;fi<faces.size();++fi) if(isBoundary(faces[fi])) {
                mesh.indices.push_back(hexTrisPerFace[fi].first[0]); mesh.indices.push_back(hexTrisPerFace[fi].first[1]); mesh.indices.push_back(hexTrisPerFace[fi].first[2]);
                mesh.indices.push_back(hexTrisPerFace[fi].second[0]); mesh.indices.push_back(hexTrisPerFace[fi].second[1]); mesh.indices.push_back(hexTrisPerFace[fi].second[2]);
            }
            break;
        }
        case 13: {
            std::vector<std::vector<uint32_t>> faces={{v[0],v[1],v[2]},{v[3],v[4],v[5]},{v[0],v[1],v[4],v[3]},{v[1],v[2],v[5],v[4]},{v[2],v[0],v[3],v[5]}};
            if(isBoundary(faces[0])) { mesh.indices.insert(mesh.indices.end(), {v[0],v[2],v[1]}); }
            if(isBoundary(faces[1])) { mesh.indices.insert(mesh.indices.end(), {v[3],v[4],v[5]}); }
            if(isBoundary(faces[2])) { mesh.indices.insert(mesh.indices.end(), {v[0],v[1],v[4], v[0],v[4],v[3]}); }
            if(isBoundary(faces[3])) { mesh.indices.insert(mesh.indices.end(), {v[1],v[2],v[5], v[1],v[5],v[4]}); }
            if(isBoundary(faces[4])) { mesh.indices.insert(mesh.indices.end(), {v[2],v[0],v[3], v[2],v[3],v[5]}); }
            break;
        }
        case 14: {
            std::vector<std::vector<uint32_t>> faces={{v[0],v[1],v[2],v[3]},{v[0],v[1],v[4]},{v[1],v[2],v[4]},{v[2],v[3],v[4]},{v[3],v[0],v[4]}};
            if(isBoundary(faces[0])) { mesh.indices.insert(mesh.indices.end(), {v[0],v[2],v[1], v[0],v[3],v[2]}); }
            if(isBoundary(faces[1])) { mesh.indices.insert(mesh.indices.end(), {v[0],v[1],v[4]}); }
            if(isBoundary(faces[2])) { mesh.indices.insert(mesh.indices.end(), {v[1],v[2],v[4]}); }
            if(isBoundary(faces[3])) { mesh.indices.insert(mesh.indices.end(), {v[2],v[3],v[4]}); }
            if(isBoundary(faces[4])) { mesh.indices.insert(mesh.indices.end(), {v[3],v[0],v[4]}); }
            break;
        }
        case 15: {
            // Pent prism: keep original triangulation but gated per face
            std::vector<std::vector<uint32_t>> faces={{v[0],v[1],v[2],v[3],v[4]},{v[5],v[6],v[7],v[8],v[9]},{v[0],v[1],v[6],v[5]},{v[1],v[2],v[7],v[6]},{v[2],v[3],v[8],v[7]},{v[3],v[4],v[9],v[8]},{v[0],v[4],v[9],v[5]}};
            if(isBoundary(faces[0])) { mesh.indices.insert(mesh.indices.end(), {v[5],v[7],v[6], v[5],v[8],v[7], v[5],v[9],v[8]}); }
            if(isBoundary(faces[1])) { mesh.indices.insert(mesh.indices.end(), {v[4],v[2],v[3], v[4],v[3],v[1], v[4],v[1],v[0]}); }
            if(isBoundary(faces[2])) { mesh.indices.insert(mesh.indices.end(), {v[0],v[1],v[6], v[0],v[6],v[5]}); }
            if(isBoundary(faces[3])) { mesh.indices.insert(mesh.indices.end(), {v[1],v[2],v[7], v[1],v[7],v[6]}); }
            if(isBoundary(faces[4])) { mesh.indices.insert(mesh.indices.end(), {v[2],v[3],v[8], v[2],v[8],v[7]}); }
            if(isBoundary(faces[5])) { mesh.indices.insert(mesh.indices.end(), {v[3],v[4],v[9], v[3],v[9],v[8]}); }
            if(isBoundary(faces[6])) { mesh.indices.insert(mesh.indices.end(), {v[0],v[5],v[9], v[0],v[9],v[4]}); }
            break;
        }
        case 6: {
            for(size_t i=0;i+2<v.size();++i){
                std::vector<uint32_t> f={v[i],v[i+1],v[i+2]};
                if(!isBoundary(f)) continue;
                if(i&1){ mesh.indices.push_back(v[i+1]); mesh.indices.push_back(v[i]); mesh.indices.push_back(v[i+2]); }
                else { mesh.indices.push_back(v[i]); mesh.indices.push_back(v[i+1]); mesh.indices.push_back(v[i+2]); }
            }
            break;
        }
        default: {
            std::vector<uint32_t> f=v;
            if(isBoundary(f)){
                for(size_t i=1;i+1<f.size();++i){ mesh.indices.push_back(f[0]); mesh.indices.push_back(f[i]); mesh.indices.push_back(f[i+1]); }
            }
            break;
        }
        }
    }
    return cellToVertices;
}

} // namespace vtk_common
