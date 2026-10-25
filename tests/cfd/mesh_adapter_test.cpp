// cfd::loadMesh — mesh file dispatch and the RenderMesh built for the viewport.
//
// The 3D case exists because the CFD page used to hard-code the 2D reader, so a
// perfectly valid 3D mesh failed with "file dimension 3 does not match expected
// 2". These tests pin down the dimension sniffing and, more importantly, the
// geometry the renderer is handed: real xyz vertices, an external boundary
// surface (not every internal cell face), and cell topology that the post-field
// extrapolation can use.

#include "cfd/cfd_mesh_adapter.h"

#include "core/mesh_loader.h"
#include "mesh/Mesh.h"
#include "support/test_support.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <utility>

namespace {

std::pair<std::uint32_t, std::uint32_t> sortedEdge(std::uint32_t a, std::uint32_t b) {
    return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
}

} // namespace

TEST_CASE("cfd.mesh_adapter.cavity_2d_still_loads") {
    test_support::requireSample("cfd/CavityQuad_40x40.msh");
    const auto res = cfd::loadMesh(test_support::samplePath("cfd/CavityQuad_40x40.msh").string());

    REQUIRE_MESSAGE(res.error.empty(), res.error);
    CHECK(res.dim == 2);
    REQUIRE(res.mesh2);
    CHECK_FALSE(res.mesh3);
    CHECK(res.mesh2->nCells() > 0);
    CHECK_FALSE(res.patches.empty());
    REQUIRE(res.renderMesh);
    CHECK(res.renderMesh->sourcePointCount == static_cast<int>(res.mesh2->vertices().size()));
    CHECK(res.renderMesh->vertices.size() / 3 >= res.mesh2->vertices().size());
    CHECK_FALSE(res.renderMesh->indices.empty());
    // 2D meshes are drawn in the z = 0 plane.
    for (std::size_t i = 2; i < res.renderMesh->vertices.size(); i += 3) {
        CHECK(res.renderMesh->vertices[i] == doctest::Approx(0.0f));
    }
}

TEST_CASE("cfd.mesh_adapter.pipe_3d_loads_with_surface_and_topology") {
    test_support::requireSample("cfd/pipe_3d.msh");
    const auto res = cfd::loadMesh(test_support::samplePath("cfd/pipe_3d.msh").string());

    REQUIRE_MESSAGE(res.error.empty(), res.error);
    CHECK(res.dim == 3);
    REQUIRE(res.mesh3);
    CHECK_FALSE(res.mesh2);

    const auto& mesh = *res.mesh3;
    // Counts straight from the file's section headers: 8fc cells, 1ea0 faces.
    // The nodes are split across two zones (5ca + 7fa), 3364 in total. They also
    // prove the reader reconstructed every cell (a dropped polyhedron would
    // move nCells).
    CHECK(mesh.nCells() == 2300);
    CHECK(mesh.nFaces() == 7840);
    CHECK(mesh.vertices().size() == 3364);
    CHECK_FALSE(res.patches.empty());

    // Real 3D geometry, not a z = 0 projection.
    REQUIRE(res.renderMesh);
    const auto& rm = *res.renderMesh;
    CHECK(rm.sourcePointCount == static_cast<int>(mesh.vertices().size()));
    CHECK(rm.vertices.size() / 3 >= mesh.vertices().size());
    CHECK_FALSE(rm.indices.empty());
    CHECK_FALSE(rm.cellEdgeIndices.empty());
    CHECK(rm.cellOffsets.size() == mesh.nCells());
    CHECK(rm.cellTypes.size() == mesh.nCells());
    CHECK(rm.cellCenters.size() == mesh.nCells());
    CHECK(rm.cellVectorCount == mesh.nCells());

    float zmin = rm.vertices[2];
    float zmax = rm.vertices[2];
    for (std::size_t i = 2; i < rm.vertices.size(); i += 3) {
        zmin = std::min(zmin, rm.vertices[i]);
        zmax = std::max(zmax, rm.vertices[i]);
    }
    CHECK(zmax - zmin > 0.0f);

    // The rendered surface must be the external boundary: triangles reference
    // only real vertices, and every triangle edge is shared by exactly two
    // triangles (a closed manifold). Internal cell faces would produce
    // non-manifold edges with counts of 4+.
    //
    // computeNormals() duplicates vertices at sharp edges for flat shading, so
    // every rendered index is mapped back to its source point before the edges
    // are compared; the split is a shading detail, not a topological one.
    auto sourceVertex = [&](std::uint32_t rendered) {
        if (rendered < rm.vertexSourceIndex.size()) return rm.vertexSourceIndex[rendered];
        return rendered;
    };
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> edgeUse;
    for (std::size_t t = 0; t + 2 < rm.indices.size(); t += 3) {
        const std::uint32_t a = sourceVertex(rm.indices[t]);
        const std::uint32_t b = sourceVertex(rm.indices[t + 1]);
        const std::uint32_t c = sourceVertex(rm.indices[t + 2]);
        REQUIRE(a < mesh.vertices().size());
        REQUIRE(b < mesh.vertices().size());
        REQUIRE(c < mesh.vertices().size());
        ++edgeUse[sortedEdge(a, b)];
        ++edgeUse[sortedEdge(b, c)];
        ++edgeUse[sortedEdge(c, a)];
    }
    int openEdges = 0;
    int nonManifoldEdges = 0;
    for (const auto& [edge, count] : edgeUse) {
        (void)edge;
        if (count < 2) ++openEdges;
        if (count > 2) ++nonManifoldEdges;
    }
    CHECK(openEdges == 0);
    CHECK(nonManifoldEdges == 0);

    // Every boundary face is a quad here, so the fan emits exactly two
    // triangles per boundary face.
    std::size_t boundaryFaces = 0;
    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        if (mesh.neighbor(f) == static_cast<std::size_t>(-1)) ++boundaryFaces;
    }
    CHECK(rm.indices.size() / 3 == boundaryFaces * 2);

    // Cell geometry: every volume finite and positive, and the sum of the
    // cells cannot exceed the axis-aligned bounding box (it may be much less
    // for a non-convex domain, but never more).
    double totalVolume = 0.0;
    double lo[3] = {mesh.vertices()[0][0], mesh.vertices()[0][1], mesh.vertices()[0][2]};
    double hi[3] = {lo[0], lo[1], lo[2]};
    for (const auto& v : mesh.vertices()) {
        for (int d = 0; d < 3; ++d) {
            lo[d] = std::min(lo[d], v[d]);
            hi[d] = std::max(hi[d], v[d]);
        }
    }
    for (std::size_t c = 0; c < mesh.nCells(); ++c) {
        const double vol = mesh.cellVolume(c);
        CHECK(std::isfinite(vol));
        CHECK(vol > 0.0);
        totalVolume += vol;
    }
    const double bboxVolume = (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]);
    CHECK(totalVolume > 0.0);
    CHECK(totalVolume <= bboxVolume * (1.0 + 1e-9));
}
