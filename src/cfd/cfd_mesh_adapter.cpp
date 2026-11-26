#include "cfd/cfd_mesh_adapter.h"

#include "cfd/mesh_build_policy.h"
#include "io/ANSYSMeshReader.h"
#include "io/CGNSReader.h"
#include "io/GmshReader.h"
#include "mesh/Mesh.h"
#include "core/cell_topology.h"
#include "core/mesh_loader.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
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
    // HDF5 superblock signature: modern CGNS files are HDF5 containers.
    static const char kHdf5Magic[8] = {'\x89', 'H', 'D', 'F', '\r', '\n', '\x1a', '\n'};
    if (std::memcmp(buf, kHdf5Magic, 8) == 0) return "cgns";
    // extension fallback
    if (path.size() >= 5 && path.substr(path.size()-5) == ".cgns") return "cgns";
    if (path.size() >= 4 && path.substr(path.size()-4) == ".msh") return "fluent";
    return "gmsh";
}

// Boundary faces are identified by their neighbour: the mesh's face list uses
// npos for a face that has no second cell (see cfd::mesh_build::kNoNeighbor).

// Canonical VTK corner order for a 3D cell. The reader already canonicalizes
// imported cells; Gmsh files are canonical by construction. Re-canonicalizing
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

// Patch ids in boundary-group (sorted name) order, one per mesh face; -1 means
// the face belongs to no named patch.
std::vector<int> buildFacePatchMap(const fvm::Mesh<3>& m, std::vector<std::string>& names) {
    std::vector<int> facePatch(m.nFaces(), -1);
    for (const auto& [name, faces] : m.boundaryGroups()) {
        const int id = static_cast<int>(names.size());
        names.push_back(name);
        for (std::size_t f : faces) {
            if (f < facePatch.size()) facePatch[f] = id;
        }
    }
    // Boundary faces outside every named zone still need a stable category,
    // otherwise they are silently painted as the first patch.
    int neutralId = -1;
    for (std::size_t f = 0; f < m.nFaces(); ++f) {
        if (m.neighbor(f) != mesh_build::kNoNeighbor || facePatch[f] >= 0) continue;
        if (neutralId < 0) {
            neutralId = static_cast<int>(names.size());
            names.push_back("Boundary");
        }
        facePatch[f] = neutralId;
    }
    return facePatch;
}

// Duplicate surface vertices at patch boundaries so every emitted triangle has
// one constant patch id. Returns the map from each new vertex to its source
// vertex, which is composed with computeNormals()'s own split map afterwards.
std::vector<uint32_t> applyPatchField(RenderMesh& rm, const std::vector<int>& trianglePatch,
                                      const std::vector<std::string>& names) {
    if (names.empty() || trianglePatch.empty() ||
        trianglePatch.size() != rm.indices.size() / 3) return {};

    const std::size_t srcCount = rm.vertices.size() / 3;
    std::map<std::pair<uint32_t, int>, uint32_t> emitted;
    std::vector<float> patchValues(srcCount, 0.0f); // interior nodes are not rendered
    std::vector<uint32_t> indices;
    std::vector<uint32_t> sourceIndex(srcCount);
    for (std::size_t i = 0; i < srcCount; ++i) sourceIndex[i] = static_cast<uint32_t>(i);
    indices.reserve(rm.indices.size());
    rm.vertices.reserve(rm.vertices.size() * 2);

    // Keep every source vertex: cellConnectivity indexes the full node set for
    // volume/isosurface extraction. Append one duplicate per (node, patch) pair
    // so the rendered surface has a constant id per triangle.
    for (std::size_t t = 0; t < trianglePatch.size(); ++t) {
        const int pid = std::clamp(trianglePatch[t], 0, static_cast<int>(names.size()) - 1);
        for (int c = 0; c < 3; ++c) {
            const uint32_t src = rm.indices[t * 3 + c];
            if (src >= srcCount) continue;
            const auto key = std::make_pair(src, pid);
            auto it = emitted.find(key);
            uint32_t dst = 0;
            if (it == emitted.end()) {
                dst = static_cast<uint32_t>(sourceIndex.size());
                emitted.emplace(key, dst);
                rm.vertices.push_back(rm.vertices[src * 3]);
                rm.vertices.push_back(rm.vertices[src * 3 + 1]);
                rm.vertices.push_back(rm.vertices[src * 3 + 2]);
                patchValues.push_back(static_cast<float>(pid));
                sourceIndex.push_back(src);
            } else {
                dst = it->second;
            }
            indices.push_back(dst);
        }
    }
    rm.indices = std::move(indices);

    auto& attrs = mesh_utils::ensureAttributes(rm);
    attrs.pointScalars["Patch"] = std::move(patchValues);
    attrs.pointScalarRanges["Patch"] = {0.0f, static_cast<float>(names.size() - 1)};
    rm.patchNames = names;
    rm.availableScalarNames.push_back("Patch");
    return sourceIndex;
}

template<std::size_t Dim>
std::shared_ptr<const RenderMesh> buildRenderMesh(const fvm::Mesh<Dim>& m,
                                                  const char* fileFormat = "MSH") {
    using Policy = cfd::mesh_build::MeshBuildPolicy<Dim>;

    RenderMesh rm;
    rm.datasetType = "CFD MESH";
    rm.fileFormat = fileFormat;

    Policy::buildVertices(rm, m);
    std::vector<uint32_t> patchSource;
    if constexpr (Dim == 3) {
        std::vector<std::string> names;
        const auto facePatch = buildFacePatchMap(m, names);
        if (!names.empty()) {
            std::vector<int> trianglePatch;
            Policy::buildSurface(rm, m, &facePatch, &trianglePatch);
            patchSource = applyPatchField(rm, trianglePatch, names);
        } else {
            Policy::buildSurface(rm, m);
        }
    } else {
        Policy::buildSurface(rm, m);
    }

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
    rm.sourcePointCount = static_cast<int>(Dim == 3 ? m.vertices().size()
                                                    : rm.vertices.size() / 3);
    mesh_utils::computeBounds(rm);
    mesh_utils::computeNormals(rm);
    if constexpr (Dim == 3) {
        if (!patchSource.empty()) {
            auto& lut = rm.vertexSourceIndex;
            if (lut.empty()) {
                lut = patchSource;
            } else {
                for (uint32_t& s : lut) {
                    if (s < patchSource.size()) s = patchSource[s];
                }
            }
        }
    }
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
#ifndef SCIRENDER_HAVE_CGNS
    if (fmt == "cgns") {
        res.error = "CGNS support is not built in. Build the vendored stack "
                    "(python tools/build_cgns_deps.py) and reconfigure with a "
                    "vendored preset (or -DCMAKE_PREFIX_PATH pointing at its "
                    "install prefix).";
        return res;
    }
#endif
    try {
        int dim = 2;
        if (fmt == "fluent") {
            dim = fvm::ANSYSMeshReader<2>::detectDimension(path);
        } else if (fmt == "cgns") {
#ifdef SCIRENDER_HAVE_CGNS
            dim = fvm::CGNSReader<2>::detectDimension(path);
#endif
        } else {
            dim = fvm::GmshReader<2>::detectDimension(path);
        }
        res.dim = dim;

        if (dim == 3) {
            if (fmt == "fluent") {
                auto r = fvm::ANSYSMeshReader<3>::read(path);
                auto mesh = std::make_shared<const fvm::Mesh<3>>(std::move(r.mesh));
                fillResult(*mesh, res);
                res.mesh3 = std::move(mesh);
            } else if (fmt == "cgns") {
#ifdef SCIRENDER_HAVE_CGNS
                auto r = fvm::CGNSReader<3>::read(path);
                auto mesh = std::make_shared<const fvm::Mesh<3>>(std::move(r.mesh));
                fillResult(*mesh, res);
                res.mesh3 = std::move(mesh);
#endif
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
            res.renderMesh = buildRenderMesh(*res.mesh3, fmt == "cgns" ? "CGNS" : "MSH");
        } else {
            if (fmt == "fluent") {
                auto r = fvm::ANSYSMeshReader<2>::read(path);
                auto mesh = std::make_shared<const fvm::Mesh<2>>(std::move(r.mesh));
                fillResult(*mesh, res);
                res.mesh2 = std::move(mesh);
            } else if (fmt == "cgns") {
#ifdef SCIRENDER_HAVE_CGNS
                auto r = fvm::CGNSReader<2>::read(path);
                auto mesh = std::make_shared<const fvm::Mesh<2>>(std::move(r.mesh));
                fillResult(*mesh, res);
                res.mesh2 = std::move(mesh);
#endif
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
            res.renderMesh = buildRenderMesh(*res.mesh2, fmt == "cgns" ? "CGNS" : "MSH");
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
