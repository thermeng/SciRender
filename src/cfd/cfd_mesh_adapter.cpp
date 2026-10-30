#include "cfd/cfd_mesh_adapter.h"

#include "cfd/mesh_build_policy.h"
#include "io/ANSYSMeshReader.h"
#include "io/GmshReader.h"
#include "mesh/Mesh.h"
#include "core/cell_topology.h"
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
// npos for a face that has no second cell (see cfd::mesh_build::kNoNeighbor).

// Canonical VTK corner order for a 3D cell. The reader already canonicalizes
// ANSYS cells; Gmsh files are canonical by construction. Re-canonicalizing
// here keeps the RenderMesh independent of the source and, more importantly,
// lets a cell that cannot be laid out as a tet/hex/wedge/pyramid be marked
// unsupported (kPolyhedron) so the volume and isosurface paths skip it.
void canonicalCell(const fvm::Mesh<3>& m, std::size_t cellIndex,
                   std::vector<std::size_t>& out, std::uint8_t& type) {
    const auto& faceIds = m.cellFaces(cellIndex);
    std::vector<std::vector<std::size_t>> loops;
    loops.reserve(faceIds.size());
    for (std::size_t f : faceIds) loops.push_back(m.faceVertices(f));

    out = m.cells()[cellIndex];
    type = cell_topology::canonicalize(out, loops, m.vertices());
    if (type == cell_topology::kPolyhedron) out = m.cells()[cellIndex];
}

template<std::size_t Dim>
std::shared_ptr<const RenderMesh> buildRenderMesh(const fvm::Mesh<Dim>& m) {
    using Policy = cfd::mesh_build::MeshBuildPolicy<Dim>;

    RenderMesh rm;
    rm.datasetType = "CFD MESH";
    rm.fileFormat = "MSH";

    Policy::buildVertices(rm, m);
    Policy::buildSurface(rm, m);

    // Cell topology (shared by the post-field extrapolation, glyph placement
    // and the least-squares gradient neighbour builder).
    auto canonicalCellFn = [](const fvm::Mesh<Dim>& mesh,
                              std::size_t cellIndex,
                              std::vector<std::size_t>& out,
                              std::uint8_t& type) {
        if constexpr (Dim == 3) {
            canonicalCell(mesh, cellIndex, out, type);
        } else {
            (void)mesh; (void)cellIndex; (void)out; (void)type;
        }
    };
    Policy::buildCellTopology(rm, m, canonicalCellFn);

    // Cell centres
    Policy::buildCellCenters(rm, m);
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
