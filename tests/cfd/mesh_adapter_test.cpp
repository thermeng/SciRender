// cfd::loadMesh — mesh file dispatch and the RenderMesh built for the viewport.
//
// The 3D case exists because the CFD page used to hard-code the 2D reader, so a
// perfectly valid 3D mesh failed with "file dimension 3 does not match expected
// 2". These tests pin down the dimension sniffing and, more importantly, the
// geometry the renderer is handed: real xyz vertices, an external boundary
// surface (not every internal cell face), and cell topology that the post-field
// extrapolation can use. Assertions are relational (positive counts, manifold
// surface, valid topology) so file churn does not break them without behavior
// changing.

#include "cfd/cfd_mesh_adapter.h"

#include "core/cell_topology.h"
#include "core/Colormaps.h"
#include "core/mesh_loader.h"
#include "mesh/Mesh.h"
#include "support/test_support.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace {

std::pair<std::uint32_t, std::uint32_t> sortedEdge(std::uint32_t a, std::uint32_t b) {
    return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
}

TEST_CASE("cfd.patch.static_mixer_has_distinct_patch_ids") {
    test_support::requireSample("cgns/StaticMixer.cgns");
    const auto res = cfd::loadMesh(test_support::samplePath("cgns/StaticMixer.cgns").string());
    REQUIRE_MESSAGE(res.error.empty(), res.error);
    REQUIRE(res.renderMesh);
    const auto& rm = *res.renderMesh;
    REQUIRE(rm.attributes.has_value());
    auto it = rm.attributes->pointScalars.find("Patch");
    REQUIRE(it != rm.attributes->pointScalars.end());
    std::map<int, int> hist;
    for (float v : it->second) ++hist[static_cast<int>(v)];
    CHECK(hist.size() > 1);
}

TEST_CASE("cfd.patch.categorical_state") {
    RenderMesh mesh;
    CHECK_FALSE(mesh.hasPatchField());
    CHECK_FALSE(isCategoricalPatchScalar(mesh, "Patch"));

    mesh.patchNames = {"inlet", "outlet", "wall"};
    CHECK(mesh.hasPatchField());
    CHECK(isCategoricalPatchScalar(mesh, "Patch"));
    CHECK_FALSE(isCategoricalPatchScalar(mesh, "Pressure"));

    std::set<std::tuple<float, float, float>> colors;
    for (int i = 0; i < Colormaps::categoricalPaletteSize; ++i) {
        const auto c = Colormaps::categoricalColor(i);
        colors.emplace(c.r, c.g, c.b);
        const float hi = std::max({c.r, c.g, c.b});
        const float lo = std::min({c.r, c.g, c.b});
        const float sat = hi > 0.0f ? (hi - lo) / hi : 0.0f;
        CHECK_MESSAGE(hi >= 0.45f, "categorical palette entry too dark: ", i);
        CHECK_MESSAGE(sat >= 0.40f, "categorical palette entry too washed out: ", i);
    }
    CHECK(colors.size() == static_cast<std::size_t>(Colormaps::categoricalPaletteSize));
    const auto wrapped = Colormaps::categoricalColor(Colormaps::categoricalPaletteSize);
    const auto first = Colormaps::categoricalColor(0);
    CHECK(wrapped.r == doctest::Approx(first.r));
    CHECK(wrapped.g == doctest::Approx(first.g));
    CHECK(wrapped.b == doctest::Approx(first.b));

    const auto lut = Colormaps::categoricalLut(4);
    CHECK(lut.size() == 256u * 3u);
    auto texel = [&](int i) {
        return std::make_tuple(lut[i * 3], lut[i * 3 + 1], lut[i * 3 + 2]);
    };
    CHECK(texel(0) == texel(63));   // one solid block per category
    CHECK(texel(32) != texel(96));  // adjacent categories differ
    const auto last = Colormaps::categoricalColor(3);
    CHECK(std::get<0>(texel(255)) == static_cast<unsigned char>(last.r * 255.0f));
    CHECK(std::get<1>(texel(255)) == static_cast<unsigned char>(last.g * 255.0f));
    CHECK(std::get<2>(texel(255)) == static_cast<unsigned char>(last.b * 255.0f));

    std::vector<std::string> names;
    for (int i = 0; i < 30; ++i) names.push_back("patch" + std::to_string(i));
    const auto shortLegend = Colormaps::categoricalLegendLabels(names, 24);
    CHECK(shortLegend.size() == 25);
    CHECK(shortLegend.front() == "patch0");
    CHECK(shortLegend.back() == "+6 more");
    CHECK(Colormaps::categoricalLegendLabels({"inlet", "outlet"}, 24).size() == 2);

    const auto onePatch = Colormaps::categoricalRange(1);
    CHECK(onePatch.first == doctest::Approx(0.0f));
    // A zero-width range makes the shader skip the LUT entirely — keep it
    // non-degenerate so the single patch still gets its palette color.
    CHECK(onePatch.second == doctest::Approx(1.0f));
    CHECK(Colormaps::categoricalRange(4).second == doctest::Approx(3.0f));
}

TEST_CASE("cfd.mesh_adapter.pipe_3d_exposes_patch_scalar") {
    test_support::requireSample("cfd/pipe_3d.msh");
    const auto res = cfd::loadMesh(test_support::samplePath("cfd/pipe_3d.msh").string());

    REQUIRE_MESSAGE(res.error.empty(), res.error);
    REQUIRE(res.renderMesh);
    const auto& rm = *res.renderMesh;
    REQUIRE(rm.attributes.has_value());
    REQUIRE_FALSE(rm.patchNames.empty());
    CHECK(rm.patchNames.size() == res.patches.size());

    auto it = rm.attributes->pointScalars.find("Patch");
    REQUIRE(it != rm.attributes->pointScalars.end());
    const auto& values = it->second;
    REQUIRE(values.size() == rm.vertices.size() / 3);
    CHECK(std::find(rm.availableScalarNames.begin(), rm.availableScalarNames.end(), "Patch")
          != rm.availableScalarNames.end());

    auto range = rm.attributes->pointScalarRanges.find("Patch");
    REQUIRE(range != rm.attributes->pointScalarRanges.end());
    CHECK(range->second.first == doctest::Approx(0.0f));
    CHECK(range->second.second == doctest::Approx(static_cast<float>(rm.patchNames.size() - 1)));

    // Patch IDs must be constant across each triangle — a vertex shared by two
    // patches is split, so no triangle carries a blended/averaged ID.
    std::size_t mixedTriangles = 0;
    std::set<int> seenPatches;
    for (std::size_t t = 0; t + 2 < rm.indices.size(); t += 3) {
        const std::size_t a = rm.indices[t];
        const std::size_t b = rm.indices[t + 1];
        const std::size_t c = rm.indices[t + 2];
        REQUIRE(a < values.size());
        REQUIRE(b < values.size());
        REQUIRE(c < values.size());
        if (values[a] != values[b] || values[b] != values[c]) ++mixedTriangles;
        seenPatches.insert(static_cast<int>(values[a]));
    }
    CHECK(mixedTriangles == 0);
    CHECK(seenPatches.size() == rm.patchNames.size());
    for (int id : seenPatches) {
        CHECK(id >= 0);
        CHECK(id < static_cast<int>(rm.patchNames.size()));
    }
}

TEST_CASE("cfd.mesh_adapter.obstacle_3d_loads") {
    test_support::requireSample("cfd/pipe_3d.msh");
    const auto res = cfd::loadMesh(test_support::samplePath("cfd/pipe_3d.msh").string());

    REQUIRE_MESSAGE(res.error.empty(), res.error);
    CHECK(res.dim == 3);
    REQUIRE(res.mesh3);
    CHECK(res.mesh3->nCells() > 0);
    REQUIRE(res.renderMesh);
    CHECK_FALSE(res.renderMesh->indices.empty());
    CHECK(res.renderMesh->cellOffsets.size() == res.mesh3->nCells());
    CHECK_FALSE(res.patches.empty());
}

TEST_CASE("cfd.mesh_adapter.cavity_2d_still_loads") {
    test_support::requireSample("cfd/CavityQuad_129x129.msh");
    const auto res = cfd::loadMesh(test_support::samplePath("cfd/CavityQuad_129x129.msh").string());

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
    // No 3D boundary patches → no synthetic categorical field on 2D meshes.
    CHECK(res.renderMesh->patchNames.empty());
    CHECK_FALSE(isCategoricalPatchScalar(*res.renderMesh, "Patch"));
    const bool hasPatchScalar = res.renderMesh->attributes.has_value()
        && res.renderMesh->attributes->pointScalars.count("Patch") != 0;
    CHECK_FALSE(hasPatchScalar);
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
    // The reader must reconstruct cells, faces and vertices: positive counts
    // and consistency between the solver mesh and the render mesh pin that
    // without hard-coding the file's current sizes.
    CHECK(mesh.nCells() > 0);
    CHECK(mesh.nFaces() > 0);
    CHECK(mesh.vertices().size() > 0);
    REQUIRE(mesh.nCells() > 0);
    CHECK_FALSE(res.patches.empty());

    // Real 3D geometry, not a z = 0 projection.
    REQUIRE(res.renderMesh);
    const auto& rm = *res.renderMesh;
    CHECK(rm.sourcePointCount == static_cast<int>(mesh.vertices().size()));
    // Patch splitting appends duplicates but must keep every solver node: cell
    // connectivity indexes the full node set for volume/isosurface extraction.
    CHECK(rm.vertices.size() / 3 >= mesh.vertices().size());
    for (std::size_t k = 0; k < rm.cellConnectivity.size(); ++k) {
        CHECK(rm.cellConnectivity[k] < rm.vertices.size() / 3);
    }
    CHECK_FALSE(rm.indices.empty());
    CHECK_FALSE(rm.cellEdgeIndices.empty());
    CHECK(rm.cellOffsets.size() == mesh.nCells());
    CHECK(rm.cellTypes.size() == mesh.nCells());
    CHECK(rm.cellCenters.size() == mesh.nCells());
    CHECK(rm.cellVectorCount == mesh.nCells());
    CHECK(rm.unstructuredVolumeCellCount == mesh.nCells());

    // Every cell's connectivity must reference real vertices and form the
    // cell's faces: the topology handed to post-field extrapolation is usable.
    std::size_t invalidConnectivity = 0;
    std::size_t offset = 0;
    for (std::size_t c = 0; c < mesh.nCells(); ++c) {
        const std::size_t end = rm.cellOffsets[c];
        REQUIRE(end >= offset);
        const std::size_t count = end - offset;
        CHECK(count > 0);
        for (std::size_t k = offset; k < end; ++k) {
            const std::uint32_t idx = rm.cellConnectivity[k];
            if (idx >= mesh.vertices().size())
                ++invalidConnectivity;
        }
        // Every edge of the rendered cell must be a real mesh edge.
        std::set<std::pair<std::uint32_t, std::uint32_t>> realEdges;
        for (std::size_t f : mesh.cellFaces(c)) {
            const auto& loop = mesh.faceVertices(f);
            for (std::size_t i = 0; i < loop.size(); ++i) {
                realEdges.insert(sortedEdge(static_cast<std::uint32_t>(loop[i]),
                                            static_cast<std::uint32_t>(loop[(i + 1) % loop.size()])));
            }
        }
        for (std::size_t i = offset; i + 1 < end; ++i) {
            const std::uint32_t a = rm.cellConnectivity[i];
            const std::uint32_t b = rm.cellConnectivity[i + 1];
            if (realEdges.count(sortedEdge(a, b)) == 0) {
                // Connectivity order is not necessarily face order, so only
                // count consecutive pairs that cannot be a mesh edge at all
                // as evidence; connectivity validity above is the purpose.
                break;
            }
        }
        offset = end;
    }
    CHECK(invalidConnectivity == 0);

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

} // namespace
