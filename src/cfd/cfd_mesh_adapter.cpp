#include "cfd/cfd_mesh_adapter.h"

#include "io/ANSYSMeshReader.h"
#include "io/GmshReader.h"
#include "mesh/Mesh.h"
#include "core/mesh_loader.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace cfd {

static std::string sniffFormat(const std::string& path) {
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

MeshLoadResult loadMesh(const std::string& path) {
    MeshLoadResult res;
    std::string fmt = sniffFormat(path);
    try {
        if (fmt == "fluent") {
            auto r = fvm::ANSYSMeshReader<2>::read(path);
            res.mesh2 = std::make_shared<const fvm::Mesh<2>>(std::move(r.mesh));
            // build patch list
            for (const auto& kv : res.mesh2->boundaryGroups()) {
                PatchInfo p; p.name = kv.first; p.faceCount = kv.second.size();
                res.patches.push_back(p);
            }
        } else {
            auto r = fvm::GmshReader<2>::read(path);
            // GmshReader is templated; force 2D usage here (phase 1 scope)
            res.mesh2 = std::make_shared<const fvm::Mesh<2>>(std::move(r.mesh));
            for (const auto& kv : res.mesh2->boundaryGroups()) {
                PatchInfo p; p.name = kv.first; p.faceCount = kv.second.size();
                res.patches.push_back(p);
            }
        }
    } catch (const std::exception& e) {
        res.error = e.what();
        return res;
    }

    // Build RenderMesh from fvm::Mesh<2>
    const auto& m = *res.mesh2;
    RenderMesh rm;
    rm.datasetType = "CFD MESH";
    rm.fileFormat = "MSH";

    // vertices (pad z=0)
    const auto& verts = m.vertices();
    for (const auto& v : verts) {
        rm.vertices.push_back(static_cast<float>(v[0]));
        rm.vertices.push_back(static_cast<float>(v[1]));
        rm.vertices.push_back(0.0f);
    }
    rm.normals.assign(rm.vertices.size(), 0.0f);
    for (size_t i = 0; i < rm.normals.size(); i += 3) rm.normals[i+2] = 1.0f;

    // patch id per vertex (boundary vertices get patch index+1)
    std::vector<int> vertexPatch(rm.vertices.size()/3, 0);
    // map vertex -> patch id via boundary groups
    int pid = 1;
    for (const auto& kv : m.boundaryGroups()) {
        for (std::size_t fi : kv.second) {
            for (std::size_t vi : m.faceVertices(fi)) {
                if (vi < vertexPatch.size()) vertexPatch[vi] = pid;
            }
        }
        ++pid;
    }
    // scalars (per vertex) = patch id
    rm.scalars.resize(rm.vertices.size()/3);
    for (size_t i = 0; i < rm.scalars.size(); ++i) rm.scalars[i] = static_cast<float>(vertexPatch[i]);
    rm.scalarName = "patchId";
    rm.availableScalarNames.push_back("patchId");
    mesh_utils::ensureAttributes(rm).cellScalars["patchId"] = {}; // placeholder

    // triangulate 2D cells (fvm cells are polygons)
    const auto& cells = m.cells();
    uint32_t off = 0;
    for (const auto& cell : cells) {
        if (cell.size() < 3) continue;
        // fan triangulation
        for (size_t i = 1; i + 1 < cell.size(); ++i) {
            rm.indices.push_back(static_cast<uint32_t>(cell[0]));
            rm.indices.push_back(static_cast<uint32_t>(cell[i]));
            rm.indices.push_back(static_cast<uint32_t>(cell[i+1]));
        }
        // cellConnectivity / offsets / types
        for (std::size_t vi : cell) rm.cellConnectivity.push_back(static_cast<uint32_t>(vi));
        rm.cellOffsets.push_back(static_cast<uint32_t>(rm.cellConnectivity.size()));
        rm.cellTypes.push_back(cell.size() == 3 ? 5 : (cell.size() == 4 ? 9 : 7));
    }

    // cellCenters
    for (std::size_t c = 0; c < m.nCells(); ++c) {
        auto cc = m.cellCenter(c);
        rm.cellCenters.emplace_back(static_cast<float>(cc[0]), static_cast<float>(cc[1]), 0.0f);
    }
    rm.cellVectorCount = static_cast<size_t>(m.nCells());

    rm.sourcePointCount = static_cast<int>(rm.vertices.size() / 3);
    mesh_utils::computeBounds(rm);
    mesh_utils::computeNormals(rm);
    mesh_utils::finalizeGeometrySignatures(rm);

    res.renderMesh = std::make_shared<const RenderMesh>(std::move(rm));
    return res;
}

} // namespace cfd