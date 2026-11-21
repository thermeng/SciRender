#include "fvm/PressureCorrection.h"
#include "fvm/FaceState.h"
#include "fvm/RhieChow.h"
#include "mesh/BoundaryUtils.h"

#include <array>
#include <chrono>
#include <iostream>
#include <cmath>
#include <limits>
#include <algorithm>
#include <tuple>

namespace fvm {

template<std::size_t Dim>
typename LinearSolverDispatcher<Dim>::Config
PressureCorrection<Dim>::solverConfig_(const Config& cfg) {
    typename LinearSolverDispatcher<Dim>::Config c;
    c.amgConfig = cfg.pressureAmg;
    // STAR parity: Acceleration = None means the AMG hierarchy is the solver
    // rather than a preconditioner.
    c.amgAsSolver = !cfg.useAMGPreconditioner ||
                    cfg.pressureAmg.acceleration ==
                    cfd::AmgSettings::Acceleration::None;
    c.amgCycles = cfg.pressureAmgPreconditionerCycles;
    c.amgMaxResidual = cfg.pressureAmg.tolerance;
    // The compact face-normal Laplacian is symmetric (SPD) by construction, so
    // CG is valid for it. The legacy composed least-squares operator
    // div(d * grad_LS) is NON-symmetric (the face-flux divergence is not the
    // exact adjoint of the least-squares gradient), so it must be solved with a
    // solver that tolerates non-symmetry: BiCGSTAB. The sign fix in solve()
    // makes its diagonal positive (better conditioned) but cannot make it
    // symmetric, so preferCG is restricted to the compact operator.
    c.preferCG = cfg.compactRhieChow;
    c.labels[0] = "pressure";
    return c;
}

template<std::size_t Dim>
PressureCorrection<Dim>::PressureCorrection(const Mesh<Dim>& mesh, const Config& config)
    : mesh_(mesh), config_(config),
      linearSolver_(mesh, solverConfig_(config)), divU_(mesh, 0.0) {}

template<std::size_t Dim>
PressureCorrection<Dim>::PressureCorrection(const Mesh<Dim>& mesh)
    : mesh_(mesh), config_{},
      linearSolver_(mesh, solverConfig_(config_)), divU_(mesh, 0.0) {}

template<std::size_t Dim>
void PressureCorrection<Dim>::refreshPressureGradient(const Field<Dim>& pressure) {
    if (pressureGradient_) {
        pressureGradient_->compute(pressure, pressureGrad_);
    } else {
        if (!gradientCalc_) {
            gradientCalc_ = std::make_unique<Gradient<Dim>>(mesh_, GradientScheme::LeastSquares);
            for (auto& g : pressureGrad_) g = Field<Dim>(mesh_, 0.0);
        }
        gradientCalc_->compute(pressure, pressureGrad_);
    }
}

template<std::size_t Dim>
void PressureCorrection<Dim>::solve(
    const VectorField<Dim>& uStar,
    const std::array<std::vector<double>, Dim>& dCoeff,
    const Field<Dim>& pressure,
    Field<Dim>& pPrime,
    const BCDiscretization& bc,
    const std::atomic<bool>* cancel) {

    const std::size_t n = mesh_.nCells();
    const std::size_t nF = mesh_.nFaces();
    if (n == 0) return;

    const auto tSolveStart = std::chrono::steady_clock::now();
    pressureStats_.solves++;

    if (!gradientCalc_) {
        gradientCalc_ = std::make_unique<Gradient<Dim>>(mesh_, GradientScheme::LeastSquares);
        for (auto& g : pressureGrad_) g = Field<Dim>(mesh_, 0.0);
    }

    const auto tGrad0 = std::chrono::steady_clock::now();
    if (config_.compactRhieChow) {
        // Least-squares pressure gradient: the operator the velocity
        // correction applies and the one the compact Rhie-Chow flux needs.
        refreshPressureGradient(pressure);
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
                : faceValueFromCentres(pressure[P], pressure[N], w);
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
    const double gradSec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - tGrad0).count();
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
    // Pressure-outlet pressure is enforced at the boundary FACES (a
    // prescribed static pressure), not by pinning outlet owner cells: the
    // outlet owner cells keep their pressure equations, and p' responds to
    // the outlet ring — the corrected outlet flux in the divergence RHS
    // (boundary branch of the loop below) and the boundary sensitivity added
    // to the compact/legacy operators. The absolute level of the field still
    // comes from the reference cell pinned above.
    for (std::size_t f = 0; f < mesh_.nFaces(); ++f) {
        if (mesh_.neighbor(f) != std::size_t(-1)) continue;
        if (f < bc.faceBCs().size() && bc.faceBCs()[f].type == VelocityBCType::PressureOutlet) {
            const std::size_t cell = mesh_.owner(f);
            if (cell < n) isPressureOutlet_[cell] = true;
        }
    }

    bool hasDirichletPressure = false;
    for (std::size_t i = 0; i < n; ++i) {
        if (pMask_[i]) { hasDirichletPressure = true; break; }
    }
    // A pressure outlet carries a symmetric positive-diagonal p' sensitivity
    // (the outlet ring), which already anchors the absolute pressure level.
    // Pinning a reference cell on top of that adds an identity row whose
    // off-diagonals are zero while the neighbours keep their -coeff entries,
    // i.e. A_ij != A_ji -- the only source of asymmetry in the compact
    // operator. Skip the reference pin when a pressure outlet is present.
    bool hasPressureOutlet = false;
    for (std::size_t i = 0; i < n; ++i) {
        if (isPressureOutlet_[i]) { hasPressureOutlet = true; break; }
    }
    if (!hasDirichletPressure && !hasPressureOutlet) {
        std::size_t refCell = config_.pressureRefCell;
        if (refCell >= n) {
            // Auto: pick a boundary-adjacent cell near the domain corner.
            // Sequential filter: smallest X, then smallest Y among X-min cells,
            // then smallest Z among Y-min cells.
            double bestX = std::numeric_limits<double>::max();
            for (std::size_t i = 0; i < n; ++i) {
                if (!mesh_.isBoundary(i)) continue;
                const double x = mesh_.cellCenter(i)[0];
                if (x < bestX) bestX = x;
            }

            double bestY = std::numeric_limits<double>::max();
            for (std::size_t i = 0; i < n; ++i) {
                if (!mesh_.isBoundary(i)) continue;
                const auto& c = mesh_.cellCenter(i);
                if (c[0] > bestX) continue;
                if (c[1] < bestY) bestY = c[1];
            }

            std::size_t bestCell = n;
            if constexpr (Dim >= 3) {
                double bestZ = std::numeric_limits<double>::max();
                for (std::size_t i = 0; i < n; ++i) {
                    if (!mesh_.isBoundary(i)) continue;
                    const auto& c = mesh_.cellCenter(i);
                    if (c[0] > bestX) continue;
                    if (c[1] > bestY) continue;
                    if (c[2] < bestZ) {
                        bestZ = c[2];
                        bestCell = i;
                    }
                }
            } else {
                for (std::size_t i = 0; i < n; ++i) {
                    if (!mesh_.isBoundary(i)) continue;
                    const auto& c = mesh_.cellCenter(i);
                    if (c[0] > bestX) continue;
                    if (c[1] > bestY) continue;
                    bestCell = i;
                    break;
                }
            }

            refCell = bestCell;
        }
        if (refCell < n && !pMask_[refCell]) { pMask_[refCell] = true; pVals_[refCell] = 0.0; }
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
    if (isolatedCellCount_ > 0 && !isolatedCellsReported_) {
        isolatedCellsReported_ = true;
        std::cerr << "  Pressure: " << isolatedCellCount_
                  << " isolated cell(s) have no pressure equation; their mass"
                     " balance cannot be corrected\n";
    }

    std::vector<double> divU(n, 0.0);
    #pragma omp parallel for schedule(dynamic)
    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = ownerPtr[f];
        const std::size_t N = neighborPtr[f];
        const double* fn = mesh_.faceNormalPtr(f);
        const double area = areaPtr[f];

        // --- Divergence: use face-based BC for Dirichlet walls (general FVM) ---
        // The face velocity comes from the shared policy layer so that the
        // divergence here, the momentum convection and the continuity residual
        // are all built from the SAME face value.
        double un = 0.0;
        const bool hasBC = bc.hasFaceBC(f);
        if (N == kNoNeighbor && hasBC) {
            un = boundaryFaceState<Dim>(uStar, P, fn, bc.faceBCs()[f]).normalVelocity;
            if (bc.isPressureDirichletFace(f)) {
                // Corrected outlet flux in the divergence RHS: the outlet ring
                // carries p' sensitivity (operator terms below), so this term
                // is the outlet face carrying the prescribed pressure relative
                // to the owner value — the boundary analog of the interior
                // (pN - pP)/dist gradient term.
                const double* xP = mesh_.cellCenterPtr(P);
                const double* xF = mesh_.faceCenterPtr(f);
                double dn = 0.0;
                for (std::size_t d = 0; d < Dim; ++d)
                    dn += fn[d] * (xF[d] - xP[d]);
                if (dn > 1e-30) {
                    double dFace = 0.0;
                    for (std::size_t d = 0; d < Dim; ++d)
                        dFace += dCoeff[d][P] * fn[d] * fn[d];
                    const double dist = mesh_.faceDist(f);
                    const double normalFactor = (dist > 1e-30) ? dn / (dist * dist) : 0.0;
                    // Same two-point normalFactor and sign as the compact outlet
                    // diagonal (rowAcc_[P] += dFace*area*normalFactor). This is
                    // the boundary analog of the interior (pN - pP)*normalFactor
                    // term, so the operator A and this RHS agree exactly.
                    // Sign convention: when pressure[P] > pressureValue we must
                    // increase outflow to drive pressure down, so the correction
                    // ADDS to the face flux (not subtracts). The old subtract
                    // sign drove pressure away from the outlet value and was
                    // previously masked by the cell-pressure overwrite in
                    // CFDSolver::applyPressureCorrection_ (now removed).
                    un += dFace * normalFactor * (pressure[P] - bc.faceBCs()[f].pressureValue);
                }
            }
        } else {
            const double w = (N == kNoNeighbor) ? 0.0 : mesh_.faceWeight(f);
            un = dotNormal(interpolateFaceVelocity(uStar, P, N, w,
                                                   &bc.dirichletMask(), &bc.dirichletVals()), fn);
        }

        // Rhie-Chow dissipation for the compact pressure equation: the
        // collocated two-point flux differs from the wide least-squares face
        // flux of the CURRENT pressure by exactly rhieChowCorrection(). Adding
        // it back is what damps the checkerboard mode and makes the compact
        // operator's RHS consistent with the compact matrix. The SAME function
        // (single source of truth) is used by the continuity residual, so the
        // operator and the residual can never drift apart.
        if (config_.compactRhieChow && N != std::size_t(-1)) {
            un += rhieChowCorrection<Dim>(mesh_, f, dCoeff, pressure, pressureGrad_, pMask_);
        }

        double flux = un * area;
        #pragma omp atomic update
        divU[P] += flux;
        if (N != std::size_t(-1)) {
            #pragma omp atomic update
            divU[N] -= flux;
        }
    }
    const double divSec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - tGrad0).count() - gradSec;
    for (std::size_t i = 0; i < n; ++i) divU_[i] = divU[i];

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
    // fixed flux and contributes nothing. A pressure-outlet boundary face
    // carries p' sensitivity at the owner row: the face pressure enters the
    // RHS through the corrected outlet flux (divergence loop above) and the
    // operator side adds the face-normal p' gradient response — compact mode
    // via the outlet-ring loop after the interior loop, legacy mode via the
    // extrapolated-owner branch.
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
                if (d < bc.dirichletMask().size() && !bc.dirichletMask()[d].empty()) {
                    dirP = bc.dirichletMask()[d][P];
                    if (N != std::size_t(-1)) dirN = bc.dirichletMask()[d][N];
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
    // Reuse the per-row accumulator to avoid per-solve allocations.
    if (rowAcc_.size() != n) {
        rowAcc_.assign(n, {});
    } else {
        for (auto& r : rowAcc_) r.clear();
    }
    for (auto& r : rowAcc_) r.reserve(2 * Dim + 2);

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
            rowAcc_[P].emplace_back(P, +coeff);
            rowAcc_[P].emplace_back(N, -coeff);
            rowAcc_[N].emplace_back(N, +coeff);
            rowAcc_[N].emplace_back(P, -coeff);
        }

        // Pressure-outlet boundary (compact mode): a fixed-pressure outlet is a
        // Dirichlet boundary for p' (p' = 0 at the face). The face normal
        // gradient of p' is (p'_P - 0)/dn, which responds ONLY to the owner
        // cell -- a symmetric POSITIVE diagonal contribution, exactly like the
        // interior two-point terms. Using the least-squares gradient here (the
        // old ring) added the wide, signed LS stencil, which is both asymmetric
        // (it broke asymMax = 0) and gave a negative self-weight (negative
        // diagonals at the outlet cells), destroying the SPD property. The
        // prescribed pressure itself enters the RHS through the corrected outlet
        // flux in the divergence loop.
        for (std::size_t f = 0; f < nF; ++f) {
            if (mesh_.neighbor(f) != std::size_t(-1)) continue;
            if (!bc.isPressureDirichletFace(f)) continue;
            const std::size_t P = ownerPtr[f];
            const double* fn = mesh_.faceNormalPtr(f);
            const double area = areaPtr[f];
            double dFace = 0.0;
            for (std::size_t d = 0; d < Dim; ++d)
                dFace += dCoeff[d][P] * fn[d] * fn[d];
            if (dFace == 0.0) continue;
            const auto& xP = mesh_.cellCenter(P);
            const auto& xF = mesh_.faceCenterPtr(f);
            double dn = 0.0;
            for (std::size_t d = 0; d < Dim; ++d)
                dn += fn[d] * (xF[d] - xP[d]);
            const double dist = mesh_.faceDist(f);
            if (dist <= 1e-30) continue;
            const double normalFactor = dn / (dist * dist);
            // Positive diagonal: the Dirichlet face stiffness.
            rowAcc_[P].emplace_back(P, dFace * area * normalFactor);
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
                // A pressure-outlet face now has its owner cell unpinned, so
                // this branch adds the boundary sensitivity to the outlet-ring
                // (the value enters through the corrected outlet flux in the
                // divergence RHS, like the compact mode).
                if (pMask_[P])
                    continue;
                const bool hasBCEntry = bc.hasFaceBC(f);
                bool ownerExtrapolated = !hasBCEntry;
                if (hasBCEntry) {
                    ownerExtrapolated = isOutflowVelocityBC(bc.faceBCs()[f].type);
                }
                if (!ownerExtrapolated)
                    continue;
                for (std::size_t d = 0; d < Dim; ++d) {
                    bool dirP = false;
                if (d < bc.dirichletMask().size()) {
                    const auto& m = bc.dirichletMask()[d];
                    if (P < m.size()) dirP = m[P];
                    }
                    if (dirP) continue; // corrected owner value = prescribed: fixed
                    const double local = -dCoeff[d][P] * fn[d] * area;
                    if (local != 0.0) {
                        for (const auto& [col, k] : gradientRows_[d][P])
                            rowAcc_[P].emplace_back(col, local * k);
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
                if (d < bc.dirichletMask().size()) {
                    const auto& m = bc.dirichletMask()[d];
                    if (N < m.size()) dirN = m[N];
                    if (P < m.size()) dirP = m[P];
                }
                if (dirN || dirP) continue;

                const double faceP = (1.0 - w) * dCoeff[d][P] * fn[d] * area;
                const double faceN = w * dCoeff[d][N] * fn[d] * area;
                // Face velocity = faceP * (G p')[d][P] + faceN * (G p')[d][N]:
                // +flux into the owner's row, -flux into the neighbour's row,
                // the same bookkeeping the divergence of u* uses above.
                if (faceP != 0.0) {
                    for (const auto& [col, k] : gradientRows_[d][P]) {
                        rowAcc_[P].emplace_back(col, faceP * k);
                        rowAcc_[N].emplace_back(col, -faceP * k);
                    }
                }
                if (faceN != 0.0) {
                    for (const auto& [col, k] : gradientRows_[d][N]) {
                        rowAcc_[P].emplace_back(col, faceN * k);
                        rowAcc_[N].emplace_back(col, -faceN * k);
                    }
                }
            }
        }
    }

    for (std::size_t r = 0; r < n; ++r) {
        if (pMask_[r]) {
            // Identity row: p' is prescribed here (reference cell, isolated
            // or degenerate cell).
            triplets_.emplace_back(r, r, 1.0);
            bVec_(r) = pVals_[r];
            rowAcc_[r].clear();
            continue;
        }
        auto& entries = rowAcc_[r];
        std::sort(entries.begin(), entries.end(),
                  [](const std::pair<std::size_t, double>& lhs, const std::pair<std::size_t, double>& rhs) {
                      return lhs.first < rhs.first;
                  });
        // b = -div(u*) in raw flux-sum units (divU_ is divided per volume
        // only after the solve, where it is read back as a diagnostic).
        // BOTH operators are assembled as -div(Gamma grad p') so the matrix is
        // SPD / an M-matrix: the compact branch builds +diag/-offdiag directly,
        // while the legacy composed operator builds its negative (+div), so the
        // legacy matrix is negated at emit below and the RHS is negated for both.
        const double rowB = -divU_[r];
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
                // Legacy composes +div(d G) (negative definite, positive
                // off-diagonals); negate it to the SPD form -div(d G) so CG/AMG
                // operate on a positive-definite operator, matching compact
                // mode. The pinned/identity rows above are emitted separately
                // and keep their +1 diagonal.
                const double emitVal = config_.compactRhieChow ? val : -val;
                triplets_.emplace_back(r, entries[last].first, emitVal);
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
    // Recorded, not printed: every corrector runs this solve, so printing here
    // repeated the same line once per corrector. The caller reports it once per
    // time step, where it describes the state of the flow rather than one solve.
    lastReverseFlowFaces_ = nReverseFlow;

    for (std::size_t P = 0; P < n; ++P) {
        divU_[P] /= mesh_.cellVolume(P);
    }

    // Warm start the pressure correction from the previous p' value:
    // the SIMPLE outer iterations change the operator only slightly, so
    // the previous correction is a much better initial guess than zero and
    // the Krylov iteration converges in fewer steps.
    Eigen::VectorXd x(n);
    for (std::size_t i = 0; i < n; ++i)
        x(static_cast<Eigen::Index>(i)) = pPrime[i];
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
            const int* lb = std::lower_bound(inner + outer[col], inner + outer[col + 1], row);
            if (lb < inner + outer[col + 1] && *lb == row) pos = static_cast<int>(lb - inner);
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
                const int* lb = std::lower_bound(inner + outer[col], inner + outer[col + 1], row);
                if (lb < inner + outer[col + 1] && *lb == row) pos = static_cast<int>(lb - inner);
                aTripletToValuePos_[t] = pos;
            }
        }
        double* vals = A_.valuePtr();
        for (std::size_t t = 0; t < triplets_.size(); ++t) {
            int pos = aTripletToValuePos_[t];
            if (pos >= 0) vals[pos] = triplets_[t].value();
        }
    }


    { // SPD / M-matrix probe of the assembled pressure operator.
        // Reports once on first assembly (cheap sanity check) and on every
        // solve when pressureSolverStats is enabled. A_ is column-major, so
        // outerIndexPtr() ranges over columns; per-row quantities are gathered
        // by scattering each stored (row,col,value) into its row.
        static bool firstReport = true;
        if (config_.pressureSolverStats || firstReport) {
            firstReport = false;
            const Eigen::Index nn = static_cast<Eigen::Index>(n);
            const int*    outer = A_.outerIndexPtr();
            const int*    inner = A_.innerIndexPtr();
            const double* vptr  = A_.valuePtr();

            std::vector<double> rowSum(n, 0.0), rowOffSum(n, 0.0), rowDiag(n, 0.0);
            std::vector<char>   hasDiag(n, 0);
            double diagMin = std::numeric_limits<double>::max();
            double diagMax = 0.0;
            std::size_t negDiag = 0, posOffDiag = 0;
            for (Eigen::Index c = 0; c < nn; ++c) {                     // column c
                for (int k = outer[c]; k < outer[c + 1]; ++k) {
                    const std::size_t r = static_cast<std::size_t>(inner[k]); // row r
                    const double a = vptr[k];
                    rowSum[r] += a;
                    if (r == static_cast<std::size_t>(c)) {
                        rowDiag[r] = a; hasDiag[r] = 1;
                        diagMin = std::min(diagMin, a);
                        diagMax = std::max(diagMax, std::abs(a));
                        if (a < 0.0) ++negDiag;
                    } else {
                        rowOffSum[r] += std::abs(a);
                        if (a > 1e-12) ++posOffDiag;
                    }
                }
            }
            double minDominance = std::numeric_limits<double>::max();   // min_i (A_ii - sum_{j!=i}|A_ij|)
            double maxInteriorRowSum = 0.0;                             // max_i |sum_j A_ij| (Neumann rows ~ 0)
            for (std::size_t i = 0; i < n; ++i) {
                if (hasDiag[i]) minDominance = std::min(minDominance, rowDiag[i] - rowOffSum[i]);
                if (!pMask_[i]) maxInteriorRowSum = std::max(maxInteriorRowSum, std::abs(rowSum[i]));
            }
            double asymMax = 0.0;                                       // max_{i!=j} |A_ij - A_ji|
            for (Eigen::Index c = 0; c < nn; ++c) {                     // column c
                for (int k = outer[c]; k < outer[c + 1]; ++k) {
                    const Eigen::Index r = inner[k];
                    if (r == c) continue;
                    // stored entry is A(r,c); its transpose is A(c,r)
                    asymMax = std::max(asymMax, std::abs(vptr[k] - A_.coeff(c, r)));
                }
            }
            std::cerr << "  [pressure op] n=" << n << " diagMin=" << diagMin
                      << " diagMax=" << diagMax << " negDiag=" << negDiag
                      << " posOffDiag=" << posOffDiag << " minDominance=" << minDominance
                      << " maxInteriorRowSum=" << maxInteriorRowSum
                      << " asymMax=" << asymMax << "\n";
        }
    }
    // --- Unified linear-solver dispatch (replaces inline AMG + Krylov) ---
    Eigen::VectorXd warmStart(n);
    for (std::size_t i = 0; i < n; ++i)
        warmStart(static_cast<Eigen::Index>(i)) = pMask_[i] ? pVals_[i] : x(static_cast<Eigen::Index>(i));
    auto result = linearSolver_.solve(0, A_, bVec_, warmStart, cancel);
    converged = !result.failed && result.x.allFinite();
    x = std::move(result.x);
    lastSolveIterations_ = result.krylovIters;
    lastSolveResidual_ = (bVec_.norm() > 0.0) ? (bVec_ - A_ * x).norm() / bVec_.norm() : 0.0;
    if (!converged) pressureStats_.amgRejected++;
    amgBuilt_ = linearSolver_.amgBuilt(0);
    pressureStats_.amgSolves++;
    // Wire stats from the dispatcher (owns AMG build/solve/krylov counters).
    pressureStats_.amgBuilds = linearSolver_.amgBuilds();
    pressureStats_.amgKrylovIters = linearSolver_.krylovIters();
    pressureStats_.amgSeconds = linearSolver_.amgSeconds();
    pressureStats_.buildSeconds = linearSolver_.buildSeconds();

    bool nonFinite = false;
    #pragma omp parallel for schedule(static) reduction(||:nonFinite)
    for (std::size_t i = 0; i < n; ++i) {
        const double val = x(static_cast<Eigen::Index>(i));
        if (!std::isfinite(val)) {
            pPrime[i] = 0.0;
            nonFinite = true;
        } else {
            pPrime[i] = val;
        }
    }
    // Eigen can report a breakdown while returning an all-zero vector, so a
    // finite result on its own is not proof of success. Any solver chain that
    // ended without convergence *and* with a non-finite residual is a hard
    // failure; a finite but inexact result keeps the historical behaviour.
    if (!converged && (!std::isfinite(lastSolveResidual_) || !x.allFinite()))
        nonFinite = true;
    lastSolveProducedNonFinite_ = nonFinite;
}

template class PressureCorrection<2>;
template class PressureCorrection<3>;

} // namespace fvm
