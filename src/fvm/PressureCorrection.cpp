#include "fvm/PressureCorrection.h"
#include "fvm/SSTConstants.h"
#include "mesh/BoundaryUtils.h"

#include <array>
#include <iostream>
#include <cmath>
#include <limits>

namespace fvm {

template<std::size_t Dim>
PressureCorrection<Dim>::PressureCorrection(const Mesh<Dim>& mesh, const Config& config)
    : mesh_(mesh), config_(config), divU_(mesh, 0.0) {}

template<std::size_t Dim>
PressureCorrection<Dim>::PressureCorrection(const Mesh<Dim>& mesh)
    : mesh_(mesh), config_{}, divU_(mesh, 0.0) {
    config_.amgConfig.interpolationType = AMGHierarchy::InterpolationType::Direct;
}

template<std::size_t Dim>
void PressureCorrection<Dim>::solve(
    const VectorField<Dim>& uStar,
    const std::array<std::vector<double>, Dim>& dCoeff,
    const Field<Dim>& pressure,
    Field<Dim>& pPrime,
    const std::vector<std::pair<std::size_t, double>>& pressureOutletCells,
    const std::vector<char>& isPressureNeumann,
    const std::vector<char>* isPressureDirichletFace,
    const std::vector<std::pair<std::size_t, double>>* pressureOutletFaces,
    const std::vector<FaceVelocityBC>* faceBC,
    const std::vector<char>* faceHasBC,
    const std::vector<std::vector<char>>* velDirichletMask,
    const std::vector<std::vector<double>>* velDirichletVals) {

    const std::size_t n = mesh_.nCells();
    const std::size_t nF = mesh_.nFaces();

    if (!gradientCalc_) {
        gradientCalc_ = std::make_unique<Gradient<Dim>>(mesh_, GradientScheme::LeastSquares);
        for (auto& g : pressureGrad_) g = Field<Dim>(mesh_, 0.0);
    }

    if (config_.compactRhieChow) {
        // Least-squares pressure gradient: the operator the velocity
        // correction applies and the one the compact Rhie-Chow flux needs.
        gradientCalc_->compute(pressure, pressureGrad_);
    } else {
        // Green-Gauss gradient is the diagnostic the legacy residuals report
        // (the matrix and the velocity correction are built from the
        // least-squares gradient; see VelocityCorrection.h).
        for (std::size_t d = 0; d < Dim; ++d) pressureGrad_[d].assign(0.0);
        std::vector<double> invVol(n);
        const double* volPtr = mesh_.cellVolumePtr();
        for (std::size_t i = 0; i < n; ++i)
            invVol[i] = (volPtr[i] > 1e-30) ? 1.0 / volPtr[i] : 0.0;
        for (std::size_t f = 0; f < nF; ++f) {
            const std::size_t P = mesh_.owner(f);
            const std::size_t N = mesh_.neighbor(f);
            const double w = mesh_.faceWeight(f);
            const double pFace = (N == std::size_t(-1))
                ? pressure[P]
                : (1.0 - w) * pressure[P] + w * pressure[N];
            const auto& fn = mesh_.faceNormal(f);
            const double area = mesh_.faceArea(f);
            for (std::size_t d = 0; d < Dim; ++d) {
                const double contrib = pFace * fn[d] * area;
                pressureGrad_[d][P] += contrib * invVol[P];
                if (N != std::size_t(-1))
                    pressureGrad_[d][N] -= contrib * invVol[N];
            }
        }
    }
    const std::size_t* ownerPtr = mesh_.ownerPtr();
    const std::size_t* neighborPtr = mesh_.neighborPtr();
    const double* areaPtr = mesh_.faceAreaPtr();

    // Reuse pre-allocated buffers
    triplets_.clear();
    triplets_.reserve(n * (2 * Dim + 4));
    bVec_.resize(n);
    bVec_.setZero();

    if (pMask_.size() != n) pMask_.resize(n);
    if (pVals_.size() != n) pVals_.resize(n);
    if (isPressureOutlet_.size() != n) isPressureOutlet_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        pMask_[i] = false;
        pVals_[i] = 0.0;
        isPressureOutlet_[i] = false;
    }
    for (const auto& [cell, pRef] : pressureOutletCells) {
        isPressureOutlet_[cell] = true;
        pMask_[cell] = true;
        pVals_[cell] = 0.0;
    }
    // Face-based Dirichlet (exact per-face, fixes cell dedup loss)
    if (isPressureDirichletFace && pressureOutletFaces) {
        for (const auto& [face, pRef] : *pressureOutletFaces) {
            if (face < mesh_.nFaces() && mesh_.neighbor(face) == std::size_t(-1)) {
                std::size_t P = mesh_.owner(face);
                isPressureOutlet_[P] = true;
                pMask_[P] = true;
                pVals_[P] = 0.0;
            }
        }
        for (std::size_t f = 0; f < mesh_.nFaces() && f < isPressureDirichletFace->size(); ++f) {
            if ((*isPressureDirichletFace)[f] && mesh_.neighbor(f) == std::size_t(-1)) {
                std::size_t P = mesh_.owner(f);
                isPressureOutlet_[P] = true;
                pMask_[P] = true;
                pVals_[P] = 0.0;
            }
        }
    }

    // A boundary cell the pressure does not prescribe and an outlet does not
    // reach is a Dirichlet cell of the pressure correction (p' = 0 there).
    for (std::size_t i = 0; i < n; ++i) {
        if (mesh_.isBoundary(i) && !isPressureNeumann[i] && !isPressureOutlet_[i]) {
            pMask_[i] = true;
            pVals_[i] = 0.0;
        }
    }

    // A pure Neumann system (no Dirichlet pressure anywhere) is singular: it
    // needs ONE pinned reference cell (p' = 0 there) to fix the constant
    // mode. When the system already carries a Dirichlet row - an outlet, a
    // prescribed pressure boundary - nothing has to be pinned: pinning a
    // second cell would force a spurious local feature into p' exactly where
    // the mass balance must stay correct.
    bool hasDirichletPressure = false;
    for (std::size_t i = 0; i < n; ++i) {
        if (pMask_[i]) { hasDirichletPressure = true; break; }
    }
    if (!hasDirichletPressure) {
        std::size_t refCell = config_.pressureRefCell;
        if (refCell >= n) {
            // The pinned cell is the one cell whose continuity error the
            // pressure correction cannot remove, so it belongs in a smooth
            // interior region - not at the first interior cell, which in a
            // cavity is a corner: the largest local imbalance, and exactly
            // where the corner vortices form.
            std::array<double, Dim> centroid{};
            for (std::size_t i = 0; i < n; ++i) {
                const auto& c = mesh_.cellCenter(i);
                for (std::size_t d = 0; d < Dim; ++d) centroid[d] += c[d];
            }
            for (std::size_t d = 0; d < Dim; ++d) centroid[d] /= static_cast<double>(n);
            double best = std::numeric_limits<double>::max();
            for (std::size_t i = 0; i < n; ++i) {
                if (mesh_.isBoundary(i) || isPressureOutlet_[i] || pMask_[i]) continue;
                if (!isPressureNeumann[i] || mesh_.isBoundary(i)) continue;
                const auto& c = mesh_.cellCenter(i);
                double d2 = 0.0;
                for (std::size_t d = 0; d < Dim; ++d) {
                    const double dd = c[d] - centroid[d];
                    d2 += dd * dd;
                }
                if (d2 < best) { best = d2; refCell = i; }
            }
            if (refCell >= n) {
                for (std::size_t i = 0; i < n; ++i) {
                    if (mesh_.isBoundary(i) || isPressureOutlet_[i]) continue;
                    refCell = i;
                    break;
                }
            }
            if (refCell >= n) refCell = n - 1;
        }
        if (!pMask_[refCell]) { pMask_[refCell] = true; pVals_[refCell] = 0.0; }
    }

    // A cell without a single interior face has no pressure equation at all:
    // its mass balance can never be corrected. It is excluded from the
    // unknowns (carries p' = 0) and counted, so the run reports it instead of
    // silently never converging. The deliberately pinned reference cell - and
    // any other cell whose pressure is already prescribed - is a CHOICE, not a
    // discovery, so it is not reported as isolated.
    isolatedCellCount_ = 0;
    #pragma omp parallel for reduction(+:isolatedCellCount_) schedule(static)
    for (std::size_t i = 0; i < n; ++i) {
        if (pMask_[i])
            continue;
        bool hasInteriorFace = false;
        for (std::size_t f : mesh_.cellFaces(i)) {
            if (mesh_.neighbor(f) != std::size_t(-1)) { hasInteriorFace = true; break; }
        }
        if (!hasInteriorFace) {
            ++isolatedCellCount_;
            pMask_[i] = true;
            pVals_[i] = 0.0;
        }
    }

    divU_.assign(0.0);

    #pragma omp parallel for schedule(static)
    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = ownerPtr[f];
        const std::size_t N = neighborPtr[f];
        const double* fn = mesh_.faceNormalPtr(f);
        const double area = areaPtr[f];

        // --- Divergence: use face-based BC for Dirichlet walls (general FVM) ---
        double un = 0.0;
        if (N == std::size_t(-1) && faceBC && faceHasBC && f < faceBC->size() && (*faceHasBC)[f]) {
            const auto &fbc = (*faceBC)[f];
            auto t = fbc.type;
            if (t == VelocityBCType::NoSlip || t == VelocityBCType::Symmetry || t == VelocityBCType::SlipWall) {
                un = 0.0; // no penetration
            } else if (t == VelocityBCType::MovingWall || t == VelocityBCType::Inlet) {
                un = fbc.u * fn[0] + fbc.v * fn[1];
                if constexpr (Dim==3) un += fbc.w * fn[2];
            } else if (t == VelocityBCType::PressureOutlet || t == VelocityBCType::Outlet) {
                for (std::size_t d = 0; d < Dim; ++d) un += uStar[d][P] * fn[d];
                std::array<double, Dim> fnArr{};
                for (std::size_t d = 0; d < Dim; ++d) fnArr[d] = fn[d];
                un = applyBackflowCorrection<Dim>(un, fnArr, fbc);
            } else {
                for (std::size_t d = 0; d < Dim; ++d) un += uStar[d][P] * fn[d];
            }
        } else {
            double w = mesh_.faceWeight(f);
            for (std::size_t d = 0; d < Dim; ++d) {
                // Face velocity must match the momentum assembly exactly
                // (Momentum.cpp interior-face rule): a cell with a prescribed
                // velocity contributes that prescribed value to the WHOLE face,
                // and the neighbour takes priority. Using a plain interpolation
                // here would balance a mass imbalance the momentum equation does
                // not have, which corrupts the pressure field near the walls.
                double uFace;
                bool dirN = false, dirP = false;
                if (velDirichletMask && velDirichletVals &&
                    d < velDirichletMask->size() && d < velDirichletVals->size()) {
                    const auto& m = (*velDirichletMask)[d];
                    const auto& v = (*velDirichletVals)[d];
                    if (!m.empty() && !v.empty()) {
                        if (N != std::size_t(-1) && m[N]) dirN = true;
                        if (m[P]) dirP = true;
                    }
                }
                if (dirN) {
                    uFace = (*velDirichletVals)[d][N];
                } else if (dirP) {
                    uFace = (*velDirichletVals)[d][P];
                } else if (N == std::size_t(-1)) {
                    uFace = uStar[d][P];
                } else {
                    uFace = (1.0 - w) * uStar[d][P] + w * uStar[d][N];
                }
                un += uFace * fn[d];
            }
        }

        // Rhie-Chow momentum interpolation for the compact pressure equation.
        // The interpolated cell velocity carries the wide cell gradient of the
        // pressure; the compact face flux must instead use the two-point
        // normal gradient. Adding the difference back is the collocated-grid
        // correction that damps the checkerboard mode. The same face
        // diffusivity is used by the compact matrix below, so the equation is
        // self-consistent.
        if (config_.compactRhieChow && N != std::size_t(-1)) {
            const double wRC = mesh_.faceWeight(f);
            double dFace = 0.0;
            for (std::size_t d = 0; d < Dim; ++d) {
                const double dP = dCoeff[d][P];
                const double dN = dCoeff[d][N];
                dFace += ((1.0 - wRC) * dP + wRC * dN) * fn[d] * fn[d];
            }
            if (dFace != 0.0) {
                const auto& xP = mesh_.cellCenter(P);
                const auto& xN = mesh_.cellCenter(N);
                double dotSfd = 0.0;
                for (std::size_t d = 0; d < Dim; ++d)
                    dotSfd += fn[d] * (xN[d] - xP[d]);
                const double dist = mesh_.faceDist(f);
                const double normalFactor =
                    (dist > 1e-30) ? dotSfd / (dist * dist) : 0.0;
                double gradInterpN = 0.0;
                for (std::size_t d = 0; d < Dim; ++d) {
                    gradInterpN += ((1.0 - wRC) * pressureGrad_[d][P] +
                                    wRC * pressureGrad_[d][N]) * fn[d];
                }
                const double gradFaceN = (pressure[N] - pressure[P]) * normalFactor;
                un += dFace * (gradInterpN - gradFaceN);
            }
        }

        double flux = un * area;
        #pragma omp atomic
        divU_[P] += flux;
        if (N != std::size_t(-1)) {
            #pragma omp atomic
            divU_[N] -= flux;
        }
    }

    // ------------------------------------------------------------------
    // Two pressure operators share the divergence above.
    //
    // compactRhieChow (PISO, collocated grid):
    //   A = div_f(d_f * (p_N - p_P)/d), the two-point face-normal Laplacian,
    //   with the Rhie-Chow face flux already on the right-hand side. With
    //   b = div(u*) the solved field satisfies div(u* - d grad p') = 0 in the
    //   FACE-FLUX sense; the Rhie-Chow term removes the checkerboard mode the
    //   central interpolation would otherwise admit.
    //
    // legacy composed operator (compactRhieChow = false):
    //   A = div(d * G) with the SAME per-cell least-squares gradient
    //   (gradientRows_, from Gradient::buildMatrices) the velocity correction
    //   applies. With b = div(u*) in raw flux-sum units the solved field then
    //   satisfies, cell by cell,
    //       div(u* - d * grad(p')) = 0
    //   exactly - not only in total. The composition is a wider stencil - a
    //   face sees the gradient of both of its cells and each gradient reaches
    //   that cell's own neighbours - which also makes the cross-diffusion part
    //   of the diffusion exact.
    //
    // In both modes a prescribed-velocity face (wall, inlet, moving lid) has a
    // fixed flux and contributes nothing. A pressure-outlet cell carries
    // p' = 0 and is an identity row.
    // ------------------------------------------------------------------
    // A cell whose velocity is prescribed in EVERY component of EVERY
    // incident face has no p' sensitivity at all: its equation would be
    // empty, so it becomes a Dirichlet cell of the pressure system (p'
    // pinned to p' = 0), which keeps the matrix invertible.
    {
        std::vector<char> rowVoid(n, 1);
        for (std::size_t f = 0; f < nF; ++f) {
            const std::size_t P = ownerPtr[f];
            const std::size_t N = neighborPtr[f];
            for (std::size_t d = 0; d < Dim; ++d) {
                bool dirN = false, dirP = false;
                if (velDirichletMask && d < velDirichletMask->size() && !(*velDirichletMask)[d].empty()) {
                    dirP = (*velDirichletMask)[d][P];
                    if (N != std::size_t(-1)) dirN = (*velDirichletMask)[d][N];
                }
                if (!dirN && !dirP) {
                    rowVoid[P] = 0;
                    if (N != std::size_t(-1)) rowVoid[N] = 0;
                }
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (rowVoid[i] && !pMask_[i]) {
                pMask_[i] = true;
                pVals_[i] = 0.0;
            }
        }
    }

    // Assemble: the per-face flux sensitivity accumulated into per-row
    // column lists. Entries are summed on emit below so every
    // (row, column) pair lands in triplets_ exactly once and the A_ value
    // scatter map stays a bijection.
    std::vector<std::vector<std::pair<std::size_t, double>>> rowAcc(n);

    if (config_.compactRhieChow) {
        // Collocated-grid PISO operator: the two-point face-normal Laplacian.
        // The right-hand side above already carries the Rhie-Chow correction,
        // so this matrix and that face flux use the same d_f. Boundary faces
        // are deliberately absent: a prescribed-velocity face has a fixed
        // flux, and a pressure boundary carries p' = 0 (its cell is a
        // Dirichlet row), so neither has a p' sensitivity. That is the
        // standard zero-normal-gradient treatment of an open boundary.
        for (std::size_t f = 0; f < nF; ++f) {
            const std::size_t P = ownerPtr[f];
            const std::size_t N = neighborPtr[f];
            if (N == std::size_t(-1)) continue;

            const double* fn = mesh_.faceNormalPtr(f);
            const double area = areaPtr[f];
            const double w = mesh_.faceWeight(f);
            double dFace = 0.0;
            for (std::size_t d = 0; d < Dim; ++d) {
                const double dP = dCoeff[d][P];
                const double dN = dCoeff[d][N];
                dFace += ((1.0 - w) * dP + w * dN) * fn[d] * fn[d];
            }
            if (dFace == 0.0) continue;

            const auto& xP = mesh_.cellCenter(P);
            const auto& xN = mesh_.cellCenter(N);
            double dotSfd = 0.0;
            for (std::size_t d = 0; d < Dim; ++d)
                dotSfd += fn[d] * (xN[d] - xP[d]);
            const double dist = mesh_.faceDist(f);
            if (dist <= 1e-30) continue;
            const double normalFactor = dotSfd / (dist * dist);
            const double coeff = dFace * area * normalFactor;
            if (coeff == 0.0) continue;

            // d_f = sum_d d_d n_d^2. The assembled matrix is -div(d_f grad),
            // i.e. positive diagonal / negative off-diagonal (an M-matrix);
            // the right-hand side is negated to match. Solving -A p' = -b
            // gives the same p' as A p' = b but lets CG/AMG operate on a
            // positive-definite system.
            rowAcc[P].emplace_back(P, +coeff);
            rowAcc[P].emplace_back(N, -coeff);
            rowAcc[N].emplace_back(N, +coeff);
            rowAcc[N].emplace_back(P, -coeff);
        }
    } else {
        if (!gradientRowsBuilt_) {
            gradientCalc_->buildMatrices(gradientRows_);
            gradientRowsBuilt_ = true;
        }

        for (std::size_t f = 0; f < nF; ++f) {
            const std::size_t P = ownerPtr[f];
            const std::size_t N = neighborPtr[f];
            const double* fn = mesh_.faceNormalPtr(f);
            const double area = areaPtr[f];

            if (N == std::size_t(-1)) {
                // A boundary face whose velocity the boundary condition FIXES
                // (walls, inlets, moving lids) does not respond to p' and
                // contributes nothing to the matrix; its prescribed flux is
                // already in b. Without a BC - and at pressure outlets, where
                // both the b-side and the divergence diagnostics use the
                // extrapolated owner value - the face velocity IS the corrected
                // owner value, so its p' sensitivity completes the composed
                // operator on the boundary ring: without it the continuity the
                // solver enforces and the divergence the run measures would
                // disagree exactly at the wall cells, and whatever the walls
                // leak would have nowhere to go but the reference cell.
                //
                // (The owner of a BC'd outlet face carries prescribed p' - its
                // row is replaced by the identity below - so its terms would be
                // discarded anyway.)
                if (pMask_[P])
                    continue;
                const bool hasBCEntry = faceBC && faceHasBC && f < faceBC->size() && (*faceHasBC)[f];
                bool ownerExtrapolated = !hasBCEntry;
                if (hasBCEntry) {
                    const auto t = (*faceBC)[f].type;
                    ownerExtrapolated = (t == VelocityBCType::PressureOutlet || t == VelocityBCType::Outlet);
                }
                if (!ownerExtrapolated)
                    continue;
                for (std::size_t d = 0; d < Dim; ++d) {
                    bool dirP = false;
                    if (velDirichletMask && d < velDirichletMask->size() && !(*velDirichletMask)[d].empty())
                        dirP = (*velDirichletMask)[d][P];
                    if (dirP) continue; // corrected owner value = prescribed: fixed
                    const double local = -dCoeff[d][P] * fn[d] * area;
                    if (local != 0.0) {
                        for (const auto& [col, k] : gradientRows_[d][P])
                            rowAcc[P].emplace_back(col, local * k);
                    }
                }
                continue;
            }

            const double w = mesh_.faceWeight(f);

            for (std::size_t d = 0; d < Dim; ++d) {
                // A cell with a prescribed velocity contributes the prescribed
                // value to the WHOLE face (the neighbour takes priority - the
                // same rule the divergence above uses), so the face velocity
                // does not respond to p' in that component.
                bool dirN = false, dirP = false;
                if (velDirichletMask && d < velDirichletMask->size() && !(*velDirichletMask)[d].empty()) {
                    dirN = (*velDirichletMask)[d][N];
                    dirP = (*velDirichletMask)[d][P];
                }
                if (dirN || dirP) continue;

                const double faceP = (1.0 - w) * dCoeff[d][P] * fn[d] * area;
                const double faceN = w * dCoeff[d][N] * fn[d] * area;
                // Face velocity = faceP * (G p')[d][P] + faceN * (G p')[d][N]:
                // +flux into the owner's row, -flux into the neighbour's row,
                // the same bookkeeping the divergence of u* uses above.
                if (faceP != 0.0) {
                    for (const auto& [col, k] : gradientRows_[d][P]) {
                        rowAcc[P].emplace_back(col, faceP * k);
                        rowAcc[N].emplace_back(col, -faceP * k);
                    }
                }
                if (faceN != 0.0) {
                    for (const auto& [col, k] : gradientRows_[d][N]) {
                        rowAcc[P].emplace_back(col, faceN * k);
                        rowAcc[N].emplace_back(col, -faceN * k);
                    }
                }
            }
        }
    }

    for (std::size_t r = 0; r < n; ++r) {
        if (pMask_[r]) {
            // Identity row: p' is prescribed here (reference cell, outlet,
            // isolated or degenerate cell).
            triplets_.emplace_back(r, r, 1.0);
            bVec_(r) = pVals_[r];
            rowAcc[r].clear();
            continue;
        }
        auto& entries = rowAcc[r];
        std::sort(entries.begin(), entries.end(),
                  [](const std::pair<std::size_t, double>& lhs, const std::pair<std::size_t, double>& rhs) {
                      return lhs.first < rhs.first;
                  });
        // b = div(u*) in raw flux-sum units (divU_ is divided per volume
        // only after the solve, where it is read back as a diagnostic). The
        // compact matrix carries the opposite sign, so its right-hand side is
        // negated with it.
        double rowB = divU_[r];
        if (config_.compactRhieChow) rowB = -rowB;
        std::size_t emitted = 0;
        std::size_t last = 0;
        for (std::size_t idx = 1; idx <= entries.size(); ++idx) {
            if (idx == entries.size() || entries[idx].first != entries[last].first) {
                double val = 0.0;
                for (std::size_t k = last; k < idx; ++k) val += entries[k].second;
                // Every column stays in the matrix, including those of pinned
                // / Dirichlet cells: their identity rows prescribe the value,
                // so the rows that reference them remain EXACT rows of the
                // composed operator. (Shifting prescribed columns into the
                // rhs instead would make the STORED matrix a truncated
                // operator whose action no longer matches the divergence of
                // the gradient it was built from.)
                triplets_.emplace_back(r, entries[last].first, val);
                ++emitted;
                last = idx;
            }
        }
        bVec_(r) = rowB;
        if (emitted == 0) {
            // Degenerate row (e.g. all incident dCoeff are zero): pin it so
            // the system stays invertible.
            pMask_[r] = true;
            pVals_[r] = 0.0;
            triplets_.emplace_back(r, r, 1.0);
            bVec_(r) = 0.0;
        }
    }

    // --- Reverse flow diagnostic (no topology change) ---
    std::size_t nReverseFlow = 0;
    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = mesh_.owner(f);
        const std::size_t N = mesh_.neighbor(f);
        if (N != std::size_t(-1)) continue;
        if (!isPressureOutlet_[P]) continue;
        double un = 0.0;
        for (std::size_t d = 0; d < Dim; ++d) un += uStar[d][P] * mesh_.faceNormal(f)[d];
        if (un < 0.0) ++nReverseFlow;
    }
    if (nReverseFlow > 0) std::cerr << "  WARNING: " << nReverseFlow << " pressure outlet face(s) with reverse flow\n";

    for (std::size_t P = 0; P < n; ++P) {
        divU_[P] /= mesh_.cellVolume(P);
    }

    Eigen::VectorXd x;
    bool converged = false;

    // The stopping criterion of the linear solve has to follow the mesh: the
    // pressure equation drives a FLUX-SUM residual to zero, and the quantity
    // that must vanish is |r|/V - the divergence of the corrected velocity.
    // A fixed relative tolerance keeps |r| ~ tol*|b| independent of h, which
    // leaves a per-cell divergence floor of tol/V that GROWS as the mesh is
    // refined. Scaling the relative tolerance by the mean cell volume keeps
    // the resolved divergence ~ tol*h on average - strictly shrinking with
    // refinement - which is what makes the coupled velocity/pressure pair
    // converge at the same rate the truncation error does.
    double volSum = 0.0;
    for (std::size_t i = 0; i < n; ++i) volSum += mesh_.cellVolume(i);
    const double volScale = (n > 0) ? volSum / static_cast<double>(n) : 1.0;

    std::size_t patternHash = 0;
    for (std::size_t i = 0; i < pMask_.size(); ++i) {
        if (pMask_[i]) patternHash ^= (i * 0x9e3779b97f4a7c15ULL + 1);
    }
    patternHash ^= (triplets_.size() * 0x85ebca6b);
    // The compact and legacy operators have different sparsity patterns. Make
    // a mode switch invalidate the matrix/preconditioner caches even when the
    // triplet count happens to match.
    if (config_.compactRhieChow) patternHash ^= 0x27d4eb2d;

    bool maskChanged = (prevPMask_.size() != pMask_.size());
    if (!maskChanged) {
        for (std::size_t i = 0; i < pMask_.size(); ++i) if (prevPMask_[i] != pMask_[i]) { maskChanged = true; break; }
    }
    const bool compactChanged = (config_.compactRhieChow != prevCompact_);

    if (!aPatternBuilt_ || patternHash != aPatternHash_) {
        A_.resize(n, n);
        A_.setFromTriplets(triplets_.begin(), triplets_.end());
        A_.makeCompressed();
        aPatternBuilt_ = true;
        aPatternHash_ = patternHash;
        // Build O(nnz) scatter map: triplet idx -> valuePtr idx (ColMajor: outer=col)
        aTripletToValuePos_.resize(triplets_.size());
        const int* outer = A_.outerIndexPtr();
        const int* inner = A_.innerIndexPtr();
        for (std::size_t t = 0; t < triplets_.size(); ++t) {
            int col = static_cast<int>(triplets_[t].col());
            int row = static_cast<int>(triplets_[t].row());
            int pos = -1;
            for (int k = outer[col]; k < outer[col + 1]; ++k) {
                if (inner[k] == row) { pos = k; break; }
            }
            aTripletToValuePos_[t] = pos;
        }
    } else {
        // O(nnz) direct scatter — no sort, no hash, no allocation
        // Rebuild map if size mismatch (defensive)
        if (aTripletToValuePos_.size() != triplets_.size()) {
            const int* outer = A_.outerIndexPtr();
            const int* inner = A_.innerIndexPtr();
            aTripletToValuePos_.resize(triplets_.size());
            for (std::size_t t = 0; t < triplets_.size(); ++t) {
                int col = static_cast<int>(triplets_[t].col());
                int row = static_cast<int>(triplets_[t].row());
                int pos = -1;
                for (int k = outer[col]; k < outer[col + 1]; ++k) {
                    if (inner[k] == row) { pos = k; break; }
                }
                aTripletToValuePos_[t] = pos;
            }
        }
        double* vals = A_.valuePtr();
        for (std::size_t t = 0; t < triplets_.size(); ++t) {
            int pos = aTripletToValuePos_[t];
            if (pos >= 0) vals[pos] = triplets_[t].value();
        }
        // Values changed but pattern same -> coarse hierarchy is stale (Ac = P^T A P depends on A).
        // Updating only fine level is insufficient; force rebuild of full hierarchy next use.
        if (amgBuilt_) {
            amg_.invalidate();
            amgBuilt_ = false;
        }
    }

    // Adaptive threshold: use AMG only for larger systems where setup cost is amortized.
    // For pressure Poisson, CG+IC is faster for n < ~10k; AMG overhead dominates and
    // pure Neumann (cavity) is difficult for AMG due to pinned reference cell.
    // Use AMG only for n >= the configured threshold where it actually pays off.
    const std::size_t amgThreshold = config_.amgThreshold;
    // For pure Neumann cavity, prefer CG even for larger sizes until AMG is hardened
    bool isPureNeumann = true;
    for (char v : isPressureOutlet_) if (v) { isPureNeumann = false; break; }
    if (isPureNeumann) {
        for (std::size_t i = 0; i < n; ++i) if (mesh_.isBoundary(i) && !isPressureNeumann[i]) { isPureNeumann = false; break; }
    }
    const std::size_t effectiveThreshold = isPureNeumann ? 20000 : amgThreshold;
    if (config_.useAMG && config_.useAMGPreconditioner && n >= effectiveThreshold) {
        if (!amgBuilt_ || patternHash != amgPatternHash_) {
            amg_.invalidate();
            auto amgCfg = config_.amgConfig;
            amgCfg.tolerance *= volScale;
            amg_.setConfig(amgCfg);
            amg_.build(A_);
            amgPrec_.setAMG(&amg_);
            amgBuilt_ = true;
            amgPatternHash_ = patternHash;
            prevPMask_ = pMask_;
            prevNonZeroPattern_ = triplets_.size();
        }

        if (amg_.isBuilt()) {
            if (!pPatternAnalyzed_ || compactChanged) {
                bicgSolver_.setMaxIterations(1000);
                bicgSolver_.setTolerance(1e-4 * volScale);
                bicgSolver_.analyzePattern(A_);
                pPatternAnalyzed_ = true;
                prevCompact_ = config_.compactRhieChow;
            }
            bicgSolver_.factorize(A_);
            bicgSolver_.preconditioner().setAMG(&amg_);
            x = bicgSolver_.solve(bVec_);
            converged = (bicgSolver_.info() == Eigen::Success);
            if (!converged) {
                std::cerr << "  WARNING: AMG-preconditioned BiCGSTAB did not converge (iters="
                          << bicgSolver_.iterations() << ", resid=" << bicgSolver_.error()
                          << "), falling back to CG+IC\n";
            }
        }
    } else if (config_.useAMG && n >= effectiveThreshold) {
        if (!amgBuilt_ || patternHash != amgPatternHash_) {
            amg_.invalidate();
            auto amgCfg = config_.amgConfig;
            amgCfg.tolerance *= volScale;
            amg_.setConfig(amgCfg);
            amg_.build(A_);
            amgBuilt_ = true;
            amgPatternHash_ = patternHash;
            prevPMask_ = pMask_;
            prevNonZeroPattern_ = triplets_.size();
        }

        if (amg_.isBuilt()) {
            converged = amg_.solve(bVec_, x);
            if (!converged) {
                std::cerr << "  WARNING: AMG solve did not converge, falling back to CG+IC\n";
            }
        }
    }

    if (!converged) {
        std::size_t nz = triplets_.size();
        if (!pPatternAnalyzed_ || maskChanged || compactChanged || nz != prevNonZeroPattern_) {
            bicgFallbackSolver_.setMaxIterations(10000);
            bicgFallbackSolver_.setTolerance(1e-8 * volScale);
            bicgFallbackSolver_.analyzePattern(A_);
            bicgSolver_.setMaxIterations(1000);
            bicgSolver_.setTolerance(1e-4 * volScale);
            bicgSolver_.analyzePattern(A_);
            cgSolver_.setMaxIterations(10000);
            cgSolver_.setTolerance(1e-8 * volScale);
            cgSolver_.analyzePattern(A_);
            pPatternAnalyzed_ = true;
            prevPMask_ = pMask_;
            prevNonZeroPattern_ = nz;
            prevCompact_ = config_.compactRhieChow;
        }
        // The legacy composed pressure matrix is NOT symmetric: its rows are
        // built from one-sided least-squares gradient fits, so A = A^T fails
        // almost everywhere. CG + Incomplete Cholesky is only meaningful for
        // SPD systems and either stalls or diverges there, so the nonsymmetric
        // Krylov solve (BiCGSTAB + ILU) goes first and CG stays as a fallback.
        // The compact Rhie-Chow matrix is the two-point Laplacian stored with
        // the opposite sign (positive diagonal, negative off-diagonal), i.e.
        // SPD; BiCGSTAB still solves it, and CG remains the fallback.
        bicgFallbackSolver_.factorize(A_);
        x = bicgFallbackSolver_.solve(bVec_);
        converged = (bicgFallbackSolver_.info() == Eigen::Success);
        lastSolveIterations_ = bicgFallbackSolver_.iterations();
        lastSolveResidual_ = bicgFallbackSolver_.error();
        if (!converged) {
            std::cerr << "  WARNING: pressure BiCGSTAB+ILU failed (iters=" << bicgFallbackSolver_.iterations()
                      << ", resid=" << bicgFallbackSolver_.error() << "), falling back to CG+IC\n";
        }
        if (!converged) {
            cgSolver_.factorize(A_);
            x = cgSolver_.solve(bVec_);
            converged = (cgSolver_.info() == Eigen::Success);
            lastSolveIterations_ = cgSolver_.iterations();
            lastSolveResidual_ = cgSolver_.error();

            if (!converged) {
                std::cerr << "  WARNING: pressure CG fallback also failed (iters=" << cgSolver_.iterations()
                          << ", resid=" << cgSolver_.error() << ")\n";
            }
        }
    }

    #pragma omp parallel for schedule(static)
    for (std::size_t i = 0; i < n; ++i) {
        if (std::isnan(x(static_cast<Eigen::Index>(i))) || std::isinf(x(static_cast<Eigen::Index>(i)))) {
            pPrime[i] = 0.0;
        } else {
            pPrime[i] = x(static_cast<Eigen::Index>(i));
        }
    }
}

template class PressureCorrection<2>;
template class PressureCorrection<3>;

} // namespace fvm
