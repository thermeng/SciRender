#include "cfd/cfd_result_builder.h"

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/SSTModel.h"
#include "core/mesh_loader.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace cfd {

namespace {

// Extrapolate a per-cell value to per-point by averaging over all cells
// incident to each point (one-ring). Works straight from the RenderMesh CSRs
// (cellConnectivity/cellOffsets, VTK-style end offsets) so no cell->vertex
// list has to be rebuilt. Templated on the value type so scalar and vector
// fields share one implementation.
template<typename T>
std::vector<T> extrapolateToPoints(const RenderMesh& rm,
                                   const std::vector<T>& cellVals,
                                   int nCells, int nPts, const T& zero = T{}) {
    std::vector<T> out(static_cast<std::size_t>(nPts), zero);
    std::vector<int> cnt(static_cast<std::size_t>(nPts), 0);
    const auto& conn = rm.cellConnectivity;
    const auto& off = rm.cellOffsets;
    uint32_t start = 0;
    for (int c = 0; c < nCells && c < static_cast<int>(off.size()); ++c) {
        const uint32_t end = off[static_cast<std::size_t>(c)];
        if (end > conn.size()) { start = end; continue; }
        const T& v = cellVals[static_cast<std::size_t>(c)];
        for (uint32_t a = start; a < end; ++a) {
            const uint32_t pid = conn[a];
            if (pid < static_cast<uint32_t>(nPts)) { out[pid] += v; ++cnt[pid]; }
        }
        start = end;
    }
    for (int i = 0; i < nPts; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        if (cnt[ui] > 0) out[ui] /= static_cast<float>(cnt[ui]);
    }
    return out;
}

std::pair<float, float> rangeOf(const std::vector<float>& v) {
    if (v.empty()) return {0.0f, 1.0f};
    const auto mm = std::minmax_element(v.begin(), v.end());
    return {*mm.first, *mm.second};
}

} // namespace

std::shared_ptr<const RenderMesh> buildResultMesh(
    const std::shared_ptr<const RenderMesh>& baseMesh,
    const fvm::Mesh<2>& mesh,
    const fvm::VectorField<2>& velocity,
    const fvm::Field<2>& pressure,
    const fvm::SSTModel<2>* turbulence,
    const CaseSettings& settings,
    const std::vector<PatchBC>& patchBCs) {

    RenderMesh rm = *baseMesh; // copy geometry + cell topology CSRs
    auto& attrs = mesh_utils::ensureAttributes(rm);

    const int nCells = static_cast<int>(mesh.nCells());
    if (nCells <= 0) return nullptr;
    const int nSrc = (rm.sourcePointCount > 0)
        ? rm.sourcePointCount
        : static_cast<int>(rm.vertices.size() / 3);
    const int nFinal = static_cast<int>(rm.vertices.size() / 3);
    if (nSrc <= 0 || nFinal <= 0) return nullptr;

    auto expandToFinal = [&](const std::vector<float>& src) {
        std::vector<float> out(static_cast<std::size_t>(nFinal), 0.0f);
        if (static_cast<int>(src.size()) == nFinal) return src;
        if (rm.vertexSourceIndex.size() == static_cast<std::size_t>(nFinal)) {
            for (int i = 0; i < nFinal; ++i) {
                const uint32_t s = rm.vertexSourceIndex[static_cast<std::size_t>(i)];
                if (s < src.size()) out[static_cast<std::size_t>(i)] = src[s];
            }
        } else {
            for (int i = 0; i < nFinal && i < static_cast<int>(src.size()); ++i)
                out[static_cast<std::size_t>(i)] = src[static_cast<std::size_t>(i)];
        }
        return out;
    };

    // ── Pressure ────────────────────────────────────────────────────────────
    // Store BOTH cell and point data. The renderer resolves the active surface
    // field per-vertex (FieldStore::resolveScalar(Vertex)); without a
    // pointScalars entry it falls back to the cell-sized array and uploads it
    // 1:1 against the larger vertex buffer, which scrambles the surface. The
    // extrapolated point copy is what the GPU actually shades.
    std::vector<float> cellPres(static_cast<std::size_t>(nCells));
    for (int c = 0; c < nCells; ++c)
        cellPres[static_cast<std::size_t>(c)] = static_cast<float>(pressure[static_cast<std::size_t>(c)]);

    attrs.cellScalars["pressure"] = cellPres;
    attrs.cellScalarRanges["pressure"] = rangeOf(cellPres);

    std::vector<float> pointPresSrc = extrapolateToPoints<float>(rm, cellPres, nCells, nSrc);
    std::vector<float> pointPres = expandToFinal(pointPresSrc);

    attrs.pointScalars["pressure"] = pointPres;
    attrs.pointScalarRanges["pressure"] = rangeOf(pointPresSrc);

    rm.scalars = pointPres;
    rm.scalarName = "pressure";

    // ── Velocity ────────────────────────────────────────────────────────────
    std::vector<glm::vec3> cellVel(static_cast<std::size_t>(nCells));
    for (int c = 0; c < nCells; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        cellVel[uc] = glm::vec3(static_cast<float>(velocity[0][uc]),
                                static_cast<float>(velocity[1][uc]),
                                0.0f);
    }

    rm.cellVectorsData = cellVel;
    rm.cellVectorOffset.clear();
    rm.cellVectorOffset["velocity"] = 0;
    rm.availableCellVectorNames = {"velocity"};
    rm.cellVectorName = "velocity";
    rm.cellVectorCount = static_cast<std::size_t>(nCells);

    // Point-located velocity (glyphs, streamlines, LIC read this contiguous run).
    std::vector<glm::vec3> pointVel = extrapolateToPoints<glm::vec3>(rm, cellVel, nCells, nSrc);

    // Preserve prescribed boundary velocities at boundary vertices instead of
    // leaving them as the average of adjacent cell centers. This keeps the
    // visualized field consistent with the applied boundary conditions (e.g. a
    // lid moving at 1 m/s shows up as 1 m/s on the lid).
    for (const auto& pbc : patchBCs) {
        auto git = mesh.boundaryGroups().find(pbc.patchName);
        if (git == mesh.boundaryGroups().end()) continue;
        glm::vec3 bvel(0.0f);
        bool apply = false;
        switch (pbc.type) {
            case BCType::VelocityInlet:
            case BCType::MovingWall:
                bvel = glm::vec3(static_cast<float>(pbc.u),
                                 static_cast<float>(pbc.v),
                                 static_cast<float>(pbc.w));
                apply = true;
                break;
            case BCType::WallNoSlip:
                apply = true;
                break;
            default:
                break;
        }
        if (!apply) continue;
        for (std::size_t f : git->second) {
            for (std::size_t vid : mesh.faceVertices(f)) {
                if (vid < pointVel.size()) pointVel[vid] = bvel;
            }
        }
    }

    rm.pointVectorsData = pointVel;
    rm.pointVectorOffset.clear();
    rm.pointVectorOffset["velocity"] = 0;
    rm.availableVectorNames = {"velocity"};
    rm.vectorName = "velocity";
    rm.pointVectorCount = static_cast<std::size_t>(nSrc);

    // ── Turbulence ──────────────────────────────────────────────────────────
    if (settings.turbulence && turbulence) {
        const auto& tk = turbulence->k();
        const auto& tw = turbulence->omega();
        const auto& tn = turbulence->nut();
        std::vector<float> k(static_cast<std::size_t>(nCells));
        std::vector<float> om(static_cast<std::size_t>(nCells));
        std::vector<float> nt(static_cast<std::size_t>(nCells));
        for (int c = 0; c < nCells; ++c) {
            const auto uc = static_cast<std::size_t>(c);
            k[uc] = static_cast<float>(tk[uc]);
            om[uc] = static_cast<float>(tw[uc]);
            nt[uc] = static_cast<float>(tn[uc]);
        }
        const std::pair<const char*, const std::vector<float>*> fields[] = {
            {"k", &k}, {"omega", &om}, {"nut", &nt}
        };
        for (const auto& [name, cellVals] : fields) {
            attrs.cellScalars[name] = *cellVals;
            attrs.cellScalarRanges[name] = rangeOf(*cellVals);
            std::vector<float> ptsSrc = extrapolateToPoints<float>(rm, *cellVals, nCells, nSrc);
            std::vector<float> pts = expandToFinal(ptsSrc);
            attrs.pointScalars[name] = pts;
            attrs.pointScalarRanges[name] = rangeOf(ptsSrc);
        }
    }

    // ── Scalar metadata ─────────────────────────────────────────────────────
    // Legacy global range = union of cell + point extents (wide enough for
    // both the cell-center and the extrapolated per-vertex coloring paths).
    attrs.scalarMin = std::min(attrs.cellScalarRanges["pressure"].first,
                               attrs.pointScalarRanges["pressure"].first);
    attrs.scalarMax = std::max(attrs.cellScalarRanges["pressure"].second,
                               attrs.pointScalarRanges["pressure"].second);

    rm.availableScalarNames.clear();
    rm.availableScalarNames.push_back("pressure");
    if (settings.turbulence) {
        rm.availableScalarNames.push_back("k");
        rm.availableScalarNames.push_back("omega");
        rm.availableScalarNames.push_back("nut");
    }

    mesh_utils::finalizeGeometrySignatures(rm);
    return std::make_shared<const RenderMesh>(std::move(rm));
}

} // namespace cfd
