#include "cfd/cfd_mesh_adapter.h"

#include "io/ANSYSMeshReader.h"
#include "io/GmshReader.h"
#include "mesh/Mesh.h"
#include "core/mesh_loader.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cfd {

namespace {

std::string sniffFormat(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "gmsh";
    char buf[16] = {0};
    in.read(buf, 15);
    std::string header(buf);
    if (header.find("(0") == 0) return "fluent";
    if (header.find("$Mesh") != std::string::npos) return "gmsh";
    // extension fallback
    if (path.size() >= 4 && path.substr(path.size()-4) == ".msh") return "fluent";
    return "gmsh";
}

// Boundary faces are identified by their neighbour: the mesh's face list uses
// npos for a face that has no second cell.
constexpr std::size_t kNoNeighbor = static_cast<std::size_t>(-1);

uint64_t edgeKey(std::uint32_t a, std::uint32_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<uint64_t>(a) << 32) | static_cast<uint64_t>(b);
}

// VTK cell type from the reconstructed face topology. ANSYS writes arbitrary
// element/node order, so the vertex count alone is not enough (a prism and a
// pyramid both have 5+ vertices); the face count and face sizes are.
int cellType3D(const fvm::Mesh<3>& m, std::size_t cell) {
    const auto& faces = m.cellFaces(cell);
    int tris = 0;
    int quads = 0;
    int other = 0;
    for (std::size_t f : faces) {
        const std::size_t n = m.faceVertices(f).size();
        if (n == 3) ++tris;
        else if (n == 4) ++quads;
        else ++other;
    }
    if (faces.size() == 4 && tris == 4) return 10;                    // VTK_TETRA
    if (faces.size() == 6 && quads == 6) return 12;                   // VTK_HEXAHEDRON
    if (faces.size() == 5 && tris == 2 && quads == 3) return 13;      // VTK_WEDGE
    if (faces.size() == 5 && tris == 4 && quads == 1) return 14;      // VTK_PYRAMID
    (void)other;
    return 42;                                                        // VTK_POLYHEDRON
}

template<std::size_t Dim>
std::shared_ptr<const RenderMesh> buildRenderMesh(const fvm::Mesh<Dim>& m) {
    RenderMesh rm;
    rm.datasetType = "CFD MESH";
    rm.fileFormat = "MSH";

    const auto& verts = m.vertices();
    rm.vertices.reserve(verts.size() * 3);
    for (const auto& v : verts) {
        rm.vertices.push_back(static_cast<float>(v[0]));
        rm.vertices.push_back(static_cast<float>(v[1]));
        if constexpr (Dim == 3) {
            rm.vertices.push_back(static_cast<float>(v[2]));
        } else {
            rm.vertices.push_back(0.0f);  // 2D CFD lives in the z=0 plane
        }
    }

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
        // A volume mesh must render its external surface: only faces without a
        // neighbour on the other side. Using the solver's face topology (instead
        // of a per-cell face-count heuristic) is exact for polyhedra.
        std::vector<std::uint32_t> edges;
        std::unordered_set<uint64_t> seenEdges;
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

    // Cell topology (shared by the post-field extrapolation, glyph placement
    // and the least-squares gradient neighbour builder).
    std::size_t cellIndex = 0;
    for (const auto& cell : m.cells()) {
        for (std::size_t vi : cell) rm.cellConnectivity.push_back(static_cast<std::uint32_t>(vi));
        rm.cellOffsets.push_back(static_cast<std::uint32_t>(rm.cellConnectivity.size()));
        if constexpr (Dim == 3) {
            rm.cellTypes.push_back(static_cast<std::uint8_t>(cellType3D(m, cellIndex)));
        } else {
            rm.cellTypes.push_back(static_cast<std::uint8_t>(
                cell.size() == 3 ? 5 : (cell.size() == 4 ? 9 : 7)));
        }
        ++cellIndex;
    }

    // Cell centres
    rm.cellCenters.reserve(m.nCells());
    for (std::size_t c = 0; c < m.nCells(); ++c) {
        const auto cc = m.cellCenter(c);
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
    rm.cellVectorCount = m.nCells();

    // Pre-normal-split point count (what the UI reports and what point fields
    // are sized against).
    rm.sourcePointCount = static_cast<int>(rm.vertices.size() / 3);
    mesh_utils::computeBounds(rm);
    mesh_utils::computeNormals(rm);
    if (rm.normals.empty() && !rm.vertices.empty()) {
        rm.normals.assign(rm.vertices.size(), 0.0f);
        for (std::size_t i = 2; i < rm.normals.size(); i += 3) rm.normals[i] = 1.0f;
    }
    mesh_utils::finalizeGeometrySignatures(rm);
    return std::make_shared<const RenderMesh>(std::move(rm));
}

template<std::size_t Dim>
void fillResult(const fvm::Mesh<Dim>& mesh, MeshLoadResult& res) {
    for (const auto& kv : mesh.boundaryGroups()) {
        PatchInfo p;
        p.name = kv.first;
        p.faceCount = kv.second.size();
        res.patches.push_back(std::move(p));
    }
}

} // namespace

MeshLoadResult loadMesh(const std::string& path) {
    MeshLoadResult res;
    const std::string fmt = sniffFormat(path);
    try {
        const int dim = (fmt == "fluent")
            ? fvm::ANSYSMeshReader<2>::detectDimension(path)
            : fvm::GmshReader<2>::detectDimension(path);
        res.dim = dim;

        if (dim == 3) {
            if (fmt == "fluent") {
                auto r = fvm::ANSYSMeshReader<3>::read(path);
                auto mesh = std::make_shared<const fvm::Mesh<3>>(std::move(r.mesh));
                fillResult(*mesh, res);
                res.mesh3 = std::move(mesh);
            } else {
                auto r = fvm::GmshReader<3>::read(path);
                auto mesh = std::make_shared<const fvm::Mesh<3>>(std::move(r.mesh));
                fillResult(*mesh, res);
                res.mesh3 = std::move(mesh);
            }
            if (!res.mesh3) {
                res.error = "The 3D mesh could not be read.";
                return res;
            }
            res.renderMesh = buildRenderMesh(*res.mesh3);
        } else {
            if (fmt == "fluent") {
                auto r = fvm::ANSYSMeshReader<2>::read(path);
                auto mesh = std::make_shared<const fvm::Mesh<2>>(std::move(r.mesh));
                fillResult(*mesh, res);
                res.mesh2 = std::move(mesh);
            } else {
                auto r = fvm::GmshReader<2>::read(path);
                auto mesh = std::make_shared<const fvm::Mesh<2>>(std::move(r.mesh));
                fillResult(*mesh, res);
                res.mesh2 = std::move(mesh);
            }
            if (!res.mesh2) {
                res.error = "The 2D mesh could not be read.";
                return res;
            }
            res.renderMesh = buildRenderMesh(*res.mesh2);
        }
    } catch (const std::exception& e) {
        res.error = e.what();
        return res;
    }

    if (!res.renderMesh || res.renderMesh->vertices.empty()) {
        res.error = "The mesh contains no geometry to render.";
    }
    return res;
}

} // namespace cfd
