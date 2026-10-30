#pragma once

#include "core/cell_topology.h"
#include "core/mesh_loader.h"
#include "mesh/Mesh.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cfd {
namespace mesh_build {

// Hash/edge helpers shared by the 3D surface extraction path.
inline std::uint64_t edgeKey(std::uint32_t a, std::uint32_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<std::uint64_t>(a) << 32) | static_cast<std::uint64_t>(b);
}

constexpr std::size_t kNoNeighbor = static_cast<std::size_t>(-1);

// Dimension policy for building a RenderMesh from an fvm::Mesh<Dim>.
//
// Encapsulates every dimension-dependent emission decision (vertex z-component,
// 2D fan-triangulation vs. 3D boundary-surface extraction, cell topology/type,
// cell-center z-component) so buildRenderMesh() in cfd_mesh_adapter.cpp can be
// written once, dimension-agnostic, delegating to Policy<Dim>::*.
template<std::size_t Dim>
struct MeshBuildPolicy {
    static constexpr std::size_t dim = Dim;
    static constexpr bool is3D = (Dim == 3);

    // Emit one vertex's 3 components into rm.vertices.
    static void emitVertex(RenderMesh& rm, const auto& v) {
        rm.vertices.push_back(static_cast<float>(v[0]));
        rm.vertices.push_back(static_cast<float>(v[1]));
        if constexpr (Dim == 3) {
            rm.vertices.push_back(static_cast<float>(v[2]));
        } else {
            rm.vertices.push_back(0.0f); // 2D CFD lives in the z = 0 plane
        }
    }

    // Build all vertices.
    static void buildVertices(RenderMesh& rm, const fvm::Mesh<Dim>& m) {
        const auto& verts = m.vertices();
        rm.vertices.reserve(verts.size() * 3);
        for (const auto& v : verts) emitVertex(rm, v);
    }

    // Build the surface index buffer (+ cell edge indices in 3D).
    static void buildSurface(RenderMesh& rm, const fvm::Mesh<Dim>& m) {
        if constexpr (Dim == 2) {
            // Every cell is a surface polygon: fan-triangulate it.
            for (const auto& cell : m.cells()) {
                if (cell.size() < 3) continue;
                for (std::size_t i = 1; i + 1 < cell.size(); ++i) {
                    rm.indices.push_back(static_cast<std::uint32_t>(cell[0]));
                    rm.indices.push_back(static_cast<std::uint32_t>(cell[i]));
                    rm.indices.push_back(static_cast<std::uint32_t>(cell[i + 1]));
                }
            }
        } else {
            // A volume mesh renders its external surface: only faces without a
            // neighbour on the other side.
            std::vector<std::uint32_t> edges;
            std::unordered_set<std::uint64_t> seenEdges;
            for (std::size_t f = 0; f < m.nFaces(); ++f) {
                if (m.neighbor(f) != kNoNeighbor) continue;
                const auto& loop = m.faceVertices(f);
                if (loop.size() < 3) continue;
                for (std::size_t i = 1; i + 1 < loop.size(); ++i) {
                    rm.indices.push_back(static_cast<std::uint32_t>(loop[0]));
                    rm.indices.push_back(static_cast<std::uint32_t>(loop[i]));
                    rm.indices.push_back(static_cast<std::uint32_t>(loop[i + 1]));
                }
                for (std::size_t i = 0; i < loop.size(); ++i) {
                    const auto a = static_cast<std::uint32_t>(loop[i]);
                    const auto b = static_cast<std::uint32_t>(loop[(i + 1) % loop.size()]);
                    if (seenEdges.insert(edgeKey(a, b)).second) {
                        edges.push_back(a);
                        edges.push_back(b);
                    }
                }
            }
            rm.cellEdgeIndices = std::move(edges);
        }
    }

    // Emit cell connectivity + type for one cell. canonicalCell is provided by
    // the adapter for 3D (it needs the mesh's face loops); for 2D it is trivial.
    static void emitCellTopology(RenderMesh& rm, const fvm::Mesh<Dim>& m,
                                std::size_t cellIndex,
                                const std::function<void(const fvm::Mesh<Dim>&, std::size_t,
                                                         std::vector<std::size_t>&,
                                                         std::uint8_t&)>& canonicalCell) {
        if constexpr (Dim == 3) {
            std::vector<std::size_t> canonical;
            std::uint8_t type = cell_topology::kPolyhedron;
            canonicalCell(m, cellIndex, canonical, type);
            for (std::size_t vi : canonical) {
                rm.cellConnectivity.push_back(static_cast<std::uint32_t>(vi));
            }
            rm.cellTypes.push_back(type);
            if (cell_topology::isVolumeCell(type)) ++rm.unstructuredVolumeCellCount;
        } else {
            const auto& cell = m.cells()[cellIndex];
            for (std::size_t vi : cell) {
                rm.cellConnectivity.push_back(static_cast<std::uint32_t>(vi));
            }
            rm.cellTypes.push_back(static_cast<std::uint8_t>(
                cell.size() == 3 ? 5 : (cell.size() == 4 ? 9 : 7)));
        }
        rm.cellOffsets.push_back(static_cast<std::uint32_t>(rm.cellConnectivity.size()));
    }

    // Build all cell topology entries.
    static void buildCellTopology(RenderMesh& rm, const fvm::Mesh<Dim>& m,
                                 const std::function<void(const fvm::Mesh<Dim>&, std::size_t,
                                                          std::vector<std::size_t>&,
                                                          std::uint8_t&)>& canonicalCell) {
        std::size_t cellIndex = 0;
        for (const auto& cell : m.cells()) {
            (void)cell;
            emitCellTopology(rm, m, cellIndex, canonicalCell);
            ++cellIndex;
        }
    }

    // Emit one cell center's 3 components.
    static void emitCellCenter(RenderMesh& rm, const auto& cc) {
        if constexpr (Dim == 3) {
            rm.cellCenters.emplace_back(static_cast<float>(cc[0]),
                                        static_cast<float>(cc[1]),
                                        static_cast<float>(cc[2]));
        } else {
            rm.cellCenters.emplace_back(static_cast<float>(cc[0]),
                                        static_cast<float>(cc[1]),
                                        0.0f);
        }
    }

    // Build all cell centers.
    static void buildCellCenters(RenderMesh& rm, const fvm::Mesh<Dim>& m) {
        rm.cellCenters.reserve(m.nCells());
        for (std::size_t c = 0; c < m.nCells(); ++c) {
            const auto cc = m.cellCenter(c);
            emitCellCenter(rm, cc);
        }
    }
};

} // namespace mesh_build
} // namespace cfd
