#include "cfd/cfd_result_builder.h"

#include "field/Field.h"
#include "field/VectorField.h"
#include "mesh/Mesh.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace cfd {

namespace {

std::pair<float, float> rangeOf(const std::vector<float>& v) {
    if (v.empty()) return {0.0f, 1.0f};
    const auto mm = std::minmax_element(v.begin(), v.end());
    return {*mm.first, *mm.second};
}

} // namespace

template<std::size_t Dim>
std::shared_ptr<const RenderMesh> buildResultMesh(
    const std::shared_ptr<const RenderMesh>& baseMesh,
    const fvm::Mesh<Dim>& mesh,
    const SolutionSnapshot& state,
    const CaseSettings& settings,
    const std::vector<PatchBC>& patchBCs) {

    if (!baseMesh || !state.hasFields() || state.cells != mesh.nCells()) return nullptr;

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
        cellPres[static_cast<std::size_t>(c)] = static_cast<float>(state.pressure[static_cast<std::size_t>(c)]);

    attrs.cellScalars["pressure"] = cellPres;
    attrs.cellScalarRanges["pressure"] = rangeOf(cellPres);

    std::vector<float> pointPresSrc = mesh_utils::extrapolateCellToPoints<float>(rm, cellPres, static_cast<std::size_t>(nCells), static_cast<std::size_t>(nSrc));
    std::vector<float> pointPres = expandToFinal(pointPresSrc);

    attrs.pointScalars["pressure"] = pointPres;
    attrs.pointScalarRanges["pressure"] = rangeOf(pointPresSrc);

    rm.scalars = pointPres;
    rm.scalarName = "pressure";

    // ── Velocity ────────────────────────────────────────────────────────────
    std::vector<glm::vec3> cellVel(static_cast<std::size_t>(nCells));
    for (int c = 0; c < nCells; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        float z = 0.0f;
        if constexpr (Dim >= 3) z = static_cast<float>(state.velocity[2][uc]);
        cellVel[uc] = glm::vec3(static_cast<float>(state.velocity[0][uc]),
                                static_cast<float>(state.velocity[1][uc]),
                                z);
    }

    rm.cellVectorsData = cellVel;
    rm.cellVectorOffset.clear();
    rm.cellVectorOffset["velocity"] = 0;
    rm.availableCellVectorNames = {"velocity"};
    rm.cellVectorName = "velocity";
    rm.cellVectorCount = static_cast<std::size_t>(nCells);

    // Point-located velocity (glyphs, streamlines, LIC read this contiguous run).
    std::vector<glm::vec3> pointVel = mesh_utils::extrapolateCellToPoints<glm::vec3>(rm, cellVel, static_cast<std::size_t>(nCells), static_cast<std::size_t>(nSrc));

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
    if (settings.turbulence && state.hasTurbulence()) {
        std::vector<float> k(static_cast<std::size_t>(nCells));
        std::vector<float> om(static_cast<std::size_t>(nCells));
        std::vector<float> nt(static_cast<std::size_t>(nCells));
        for (int c = 0; c < nCells; ++c) {
            const auto uc = static_cast<std::size_t>(c);
            k[uc] = static_cast<float>(state.turbulence.k[uc]);
            om[uc] = static_cast<float>(state.turbulence.omega[uc]);
            nt[uc] = static_cast<float>(state.turbulence.nut[uc]);
        }
        const std::pair<const char*, const std::vector<float>*> fields[] = {
            {"k", &k}, {"omega", &om}, {"nut", &nt}
        };
        for (const auto& [name, cellVals] : fields) {
            attrs.cellScalars[name] = *cellVals;
            attrs.cellScalarRanges[name] = rangeOf(*cellVals);
            std::vector<float> ptsSrc = mesh_utils::extrapolateCellToPoints<float>(rm, *cellVals, static_cast<std::size_t>(nCells), static_cast<std::size_t>(nSrc));
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
    if (settings.turbulence && state.hasTurbulence()) {
        rm.availableScalarNames.push_back("k");
        rm.availableScalarNames.push_back("omega");
        rm.availableScalarNames.push_back("nut");
    }

    mesh_utils::finalizeGeometrySignatures(rm);
    return std::make_shared<const RenderMesh>(std::move(rm));
}

template std::shared_ptr<const RenderMesh> buildResultMesh<2>(
    const std::shared_ptr<const RenderMesh>&,
    const fvm::Mesh<2>&,
    const SolutionSnapshot&,
    const CaseSettings&,
    const std::vector<PatchBC>&);

template std::shared_ptr<const RenderMesh> buildResultMesh<3>(
    const std::shared_ptr<const RenderMesh>&,
    const fvm::Mesh<3>&,
    const SolutionSnapshot&,
    const CaseSettings&,
    const std::vector<PatchBC>&);

} // namespace cfd
