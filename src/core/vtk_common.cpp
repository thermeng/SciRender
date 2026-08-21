#include "core/vtk_common.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

namespace vtk_common {

// ── Structured grid topology ─────────────────────────────────────────────────
// Boundary quads of the grid; one 4-vertex entry per quad. Shared by both VTK
// adapters (the XML twin used to compute this list and throw it away, then
// rebuild volume cells inline).
std::vector<std::vector<uint32_t>> generateStructuredGridSurface(RenderMesh& mesh,
                                                                 int dX, int dY, int dZ) {
    std::vector<std::vector<uint32_t>> cellToVertices;
    auto idx = [&](int x, int y, int z) { return x + y * dX + z * dX * dY; };
    auto addQuad = [&](int a, int b, int c, int d) {
        mesh.indices.push_back(static_cast<uint32_t>(a));
        mesh.indices.push_back(static_cast<uint32_t>(b));
        mesh.indices.push_back(static_cast<uint32_t>(c));
        mesh.indices.push_back(static_cast<uint32_t>(a));
        mesh.indices.push_back(static_cast<uint32_t>(c));
        mesh.indices.push_back(static_cast<uint32_t>(d));
        cellToVertices.push_back({
            static_cast<uint32_t>(a), static_cast<uint32_t>(b),
            static_cast<uint32_t>(c), static_cast<uint32_t>(d)
        });
    };

    const int cx = std::max(1, dX - 1);
    const int cy = std::max(1, dY - 1);
    const int cz = std::max(1, dZ - 1);

    const bool is3D = (dX > 1 && dY > 1 && dZ > 1);
    if (is3D) {
        for (int y = 0; y < cy; ++y)
            for (int x = 0; x < cx; ++x)
                addQuad(idx(x, y, 0), idx(x, y + 1, 0), idx(x + 1, y + 1, 0), idx(x + 1, y, 0));
        for (int y = 0; y < cy; ++y)
            for (int x = 0; x < cx; ++x)
                addQuad(idx(x, y, dZ - 1), idx(x + 1, y, dZ - 1), idx(x + 1, y + 1, dZ - 1), idx(x, y + 1, dZ - 1));
        for (int z = 0; z < cz; ++z)
            for (int y = 0; y < cy; ++y)
                addQuad(idx(0, y, z), idx(0, y, z + 1), idx(0, y + 1, z + 1), idx(0, y + 1, z));
        for (int z = 0; z < cz; ++z)
            for (int y = 0; y < cy; ++y)
                addQuad(idx(dX - 1, y, z), idx(dX - 1, y + 1, z), idx(dX - 1, y + 1, z + 1), idx(dX - 1, y, z + 1));
        for (int z = 0; z < cz; ++z)
            for (int x = 0; x < cx; ++x)
                addQuad(idx(x, 0, z), idx(x + 1, 0, z), idx(x + 1, 0, z + 1), idx(x, 0, z + 1));
        for (int z = 0; z < cz; ++z)
            for (int x = 0; x < cx; ++x)
                addQuad(idx(x, dY - 1, z), idx(x, dY - 1, z + 1), idx(x + 1, dY - 1, z + 1), idx(x + 1, dY - 1, z));
    } else if (dZ == 1) {
        for (int y = 0; y + 1 < dY; ++y)
            for (int x = 0; x + 1 < dX; ++x)
                addQuad(idx(x, y, 0), idx(x + 1, y, 0), idx(x + 1, y + 1, 0), idx(x, y + 1, 0));
    } else if (dY == 1) {
        for (int z = 0; z + 1 < dZ; ++z)
            for (int x = 0; x + 1 < dX; ++x)
                addQuad(idx(x, 0, z), idx(x + 1, 0, z), idx(x + 1, 0, z + 1), idx(x, 0, z + 1));
    } else if (dX == 1) {
        for (int z = 0; z + 1 < dZ; ++z)
            for (int y = 0; y + 1 < dY; ++y)
                addQuad(idx(0, y, z), idx(0, y + 1, z), idx(0, y + 1, z + 1), idx(0, y, z + 1));
    }
    return cellToVertices;
}

// Volume cells: hexes in 3D, single quad plane when an axis has one node.
// Cell order is the z-major (z, then y, then x) sweep — cell-indexed data
// arrays map onto these in file order.
std::vector<std::vector<uint32_t>> generateStructuredGridCells(int dX, int dY, int dZ) {
    std::vector<std::vector<uint32_t>> cellToVertices;
    auto idx = [&](int x, int y, int z) { return x + y * dX + z * dX * dY; };
    const int cx = std::max(1, dX - 1);
    const int cy = std::max(1, dY - 1);
    const int cz = std::max(1, dZ - 1);
    cellToVertices.reserve(static_cast<size_t>(cx) * cy * cz);

    if (dX > 1 && dY > 1 && dZ > 1) {
        for (int z = 0; z + 1 < dZ; ++z)
            for (int y = 0; y + 1 < dY; ++y)
                for (int x = 0; x + 1 < dX; ++x) {
                    uint32_t i0 = static_cast<uint32_t>(idx(x, y, z));
                    uint32_t i1 = static_cast<uint32_t>(idx(x + 1, y, z));
                    uint32_t i2 = static_cast<uint32_t>(idx(x + 1, y + 1, z));
                    uint32_t i3 = static_cast<uint32_t>(idx(x, y + 1, z));
                    uint32_t i4 = static_cast<uint32_t>(idx(x, y, z + 1));
                    uint32_t i5 = static_cast<uint32_t>(idx(x + 1, y, z + 1));
                    uint32_t i6 = static_cast<uint32_t>(idx(x + 1, y + 1, z + 1));
                    uint32_t i7 = static_cast<uint32_t>(idx(x, y + 1, z + 1));
                    cellToVertices.push_back({ i0, i1, i2, i3, i4, i5, i6, i7 });
                }
    } else if (dZ == 1) {
        for (int y = 0; y + 1 < dY; ++y)
            for (int x = 0; x + 1 < dX; ++x)
                cellToVertices.push_back({
                    static_cast<uint32_t>(idx(x, y, 0)),
                    static_cast<uint32_t>(idx(x + 1, y, 0)),
                    static_cast<uint32_t>(idx(x + 1, y + 1, 0)),
                    static_cast<uint32_t>(idx(x, y + 1, 0)) });
    } else if (dY == 1) {
        for (int z = 0; z + 1 < dZ; ++z)
            for (int x = 0; x + 1 < dX; ++x)
                cellToVertices.push_back({
                    static_cast<uint32_t>(idx(x, 0, z)),
                    static_cast<uint32_t>(idx(x + 1, 0, z)),
                    static_cast<uint32_t>(idx(x + 1, 0, z + 1)),
                    static_cast<uint32_t>(idx(x, 0, z + 1)) });
    } else if (dX == 1) {
        for (int z = 0; z + 1 < dZ; ++z)
            for (int y = 0; y + 1 < dY; ++y)
                cellToVertices.push_back({
                    static_cast<uint32_t>(idx(0, y, z)),
                    static_cast<uint32_t>(idx(0, y + 1, z)),
                    static_cast<uint32_t>(idx(0, y + 1, z + 1)),
                    static_cast<uint32_t>(idx(0, y, z + 1)) });
    }
    return cellToVertices;
}

// ── Finalize ─────────────────────────────────────────────────────────────────

void calculateScalarRanges(RenderMesh& mesh) {
    // Ensure attributes are allocated
    if (!mesh.attributes.has_value()) {
        mesh.attributes = DatasetAttributes();
    }

    if (!mesh.scalarName.empty() && mesh.attributes->pointScalars.count(mesh.scalarName)) {
        const auto& activeVec = mesh.attributes->pointScalars[mesh.scalarName];
        if (activeVec.empty()) return;

        float minVal = std::numeric_limits<float>::max();
        float maxVal = -std::numeric_limits<float>::max();
        for (float val : activeVec) {
            if (val < minVal) minVal = val;
            if (val > maxVal) maxVal = val;
        }

        mesh.attributes->scalarMin = minVal;
        mesh.attributes->scalarMax = maxVal;

        if (std::abs(mesh.attributes->scalarMax - mesh.attributes->scalarMin) < 1e-6f) {
            mesh.attributes->scalarMax = mesh.attributes->scalarMin + 1.0f;
        }
    }
}

void finalizeVTKMesh(RenderMesh& mesh, const FinalizeContext& ctx) {
    if (mesh.vertices.empty()) {
        std::cerr << ctx.logLabel << " Error: Empty data sequence." << std::endl;
        return;
    }

    if (mesh.indices.empty() && ctx.datasetType != "POLYDATA" && !mesh.renderAsPoints) {
        std::cerr << ctx.logLabel << " Error: topology produced no triangles and not a point set; mesh will not render." << std::endl;
        return;
    }

    {
        const uint32_t vCount = static_cast<uint32_t>(mesh.vertices.size() / 3);
        bool badIndex = false;
        for (uint32_t idx : mesh.indices) {
            if (idx >= vCount) { badIndex = true; break; }
        }
        if (badIndex) {
            std::cerr << ctx.logLabel << " Error: topology references vertex index >= vertex count ("
                      << vCount << "); dropping indices to avoid out-of-range GPU fetch." << std::endl;
            mesh.indices.clear();
            return;
        }
    }

    mesh_utils::extrapolateCellDataToPoints(mesh, ctx.globalCellToVertices,
                                            ctx.cellScalarsStorage, ctx.cellVectorsStorage);

    if (mesh.attributes.has_value()) {
        // Flatten per-point vectors into one contiguous vec3 buffer with a
        // per-field offset (shared_ptr/zero-copy glyph pipeline expects this).
        const size_t perVertex = mesh.vertices.size() / 3;
        mesh.pointVectorCount = perVertex; // per-POINT count, set BEFORE computeNormals splits
        for (const auto& [name, vecArr] : mesh.attributes->pointVectors) {
            if (vecArr.size() < perVertex * 3) continue; // skip unusable field
            mesh.pointVectorOffset[name] = mesh.pointVectorsData.size();
            for (size_t v = 0; v < perVertex; ++v) {
                mesh.pointVectorsData.emplace_back(
                    vecArr[v * 3 + 0], vecArr[v * 3 + 1], vecArr[v * 3 + 2]);
            }
        }
    }

    {
        // Flatten per-cell vectors into the contiguous glyph buffer and record
        // each cell's centroid — this is what makes CELL_DATA vectors usable by
        // the glyph pass and cell-indexed overlays.
        const size_t cellCount = ctx.globalCellToVertices.size();
        mesh.cellVectorCount = cellCount;
        for (const auto& [name, raw] : ctx.cellVectorsStorage) {
            if (cellCount == 0 || raw.size() < cellCount * 3) continue;
            mesh.cellVectorOffset[name] = mesh.cellVectorsData.size();
            for (size_t c = 0; c < cellCount; ++c) {
                mesh.cellVectorsData.emplace_back(
                    raw[c * 3 + 0], raw[c * 3 + 1], raw[c * 3 + 2]);
            }
            mesh.availableCellVectorNames.push_back(name);
        }
        if (mesh.cellVectorName.empty() && !mesh.availableCellVectorNames.empty()) {
            mesh.cellVectorName = mesh.availableCellVectorNames.front();
        }
        if (cellCount != 0 && !mesh.vertices.empty()) {
            mesh.cellCenters.reserve(cellCount);
            for (size_t c = 0; c < cellCount; ++c) {
                const auto& corners = ctx.globalCellToVertices[c];
                float cx = 0.0f, cy = 0.0f, cz = 0.0f;
                for (uint32_t vi : corners) {
                    const size_t base = static_cast<size_t>(vi) * 3;
                    cx += mesh.vertices[base + 0];
                    cy += mesh.vertices[base + 1];
                    cz += mesh.vertices[base + 2];
                }
                const float n = static_cast<float>(corners.size());
                mesh.cellCenters.emplace_back(cx / n, cy / n, cz / n);
            }
        }
    }

    const bool hasAttributes = mesh.attributes.has_value();

    if (hasAttributes && !mesh.attributes->pointScalars.empty()) {
        if (mesh.scalarName.empty() || !mesh.attributes->pointScalars.count(mesh.scalarName)) {
            mesh.scalarName = mesh.attributes->pointScalars.begin()->first;
        }
        const std::vector<float>& active = mesh.attributes->pointScalars[mesh.scalarName];
        size_t vCount = mesh.vertices.size() / 3;
        if (!active.empty() && active.size() == vCount) {
            mesh.scalars = active;
        } else {
            std::cerr << ctx.logLabel << " Warning: active scalar '" << mesh.scalarName
                      << "' is not 1-component per vertex; scalar coloring disabled." << std::endl;
            mesh.scalars.clear();
        }
    } else {
        mesh.scalars.clear();
    }

    // Expose every field name so the UI can switch fields
    if (hasAttributes) {
        for (const auto& [name, _] : mesh.attributes->pointScalars) {
            mesh.availableScalarNames.push_back(name);
        }
        for (const auto& [name, _] : mesh.attributes->pointVectors) {
            mesh.availableVectorNames.push_back(name);
        }
        std::sort(mesh.availableScalarNames.begin(), mesh.availableScalarNames.end());
        std::sort(mesh.availableVectorNames.begin(), mesh.availableVectorNames.end());
    }

    if (mesh.vectorName.empty() && !mesh.availableVectorNames.empty()) {
        mesh.vectorName = mesh.availableVectorNames.front();
    }

    calculateScalarRanges(mesh);
    mesh_utils::computeBounds(mesh);

    mesh.sourcePointCount = static_cast<int>(mesh.vertices.size() / 3);

    // flatVerts is lazily computed via ensureFlatVerts() — only built when
    // mesh_quality analysis actually runs, saving 3x index-count memory.

    if (mesh.normals.empty() && !mesh.indices.empty()) {
        mesh_utils::computeNormals(mesh);
    }
}

} // namespace vtk_common
