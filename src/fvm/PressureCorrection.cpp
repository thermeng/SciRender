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
    c.amgAsSolver = !cfg.useAMGPreconditioner ||
                    cfg.pressureAmg.acceleration ==
                    cfd::AmgSettings::Acceleration::None;
    c.amgCycles = cfg.pressureAmgPreconditionerCycles;
    c.amgMaxResidual = cfg.pressureAmg.tolerance;
    c.preferCG = true;
    c.labels[0] = "Pressure";
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
            if constexpr (Dim == 3) gradientCalc_->setLimitGradients(mesh_.isUnstructured());
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
    const std::vector<double>* faceVolFlux,
    const std::atomic<bool>* cancel) {

    const std::size_t n = mesh_.nCells();
    const std::size_t nF = mesh_.nFaces();
    if (n == 0) return;

    const auto tSolveStart = std::chrono::steady_clock::now();
    pressureStats_.solves++;

    if (!gradientCalc_) {
        gradientCalc_ = std::make_unique<Gradient<Dim>>(mesh_, GradientScheme::LeastSquares);
        if constexpr (Dim == 3) gradientCalc_->setLimitGradients(mesh_.isUnstructured());
        for (auto& g : pressureGrad_) g = Field<Dim>(mesh_, 0.0);
    }

    const auto tGrad0 = std::chrono::steady_clock::now();
    refreshPressureGradient(pressure);

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
    bool hasPressureOutlet = false;
    for (std::size_t i = 0; i < n; ++i) {
        if (isPressureOutlet_[i]) { hasPressureOutlet = true; break; }
    }
    if (!hasDirichletPressure && !hasPressureOutlet) {
        std::size_t refCell = config_.pressureRefCell;
        if (refCell >= n) {
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

        double flux;
        if (faceVolFlux != nullptr) {
            // Provisional Rhie-Chow face volume flux: already includes the
            // Rhie-Chow term and the boundary face state. The outlet pressure
            // term is a pressure-correction boundary term and is added here.
            flux = (*faceVolFlux)[f];
            if (N == kNoNeighbor && bc.isPressureDirichletFace(f)) {
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
                    flux += area * dFace * normalFactor *
                            (pressure[P] - bc.faceBCs()[f].pressureValue);
                }
            }
        } else {
            double un = 0.0;
            const bool hasBC = bc.hasFaceBC(f);
            if (N == kNoNeighbor && hasBC) {
                un = boundaryFaceState<Dim>(uStar, P, fn, bc.faceBCs()[f]).normalVelocity;
                if (bc.isPressureDirichletFace(f)) {
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
            if (N != std::size_t(-1)) {
                un += rhieChowCorrection<Dim>(mesh_, f, dCoeff, pressure, pressureGrad_, pMask_);
            }
            flux = un * area;
        }
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
    if (rowAcc_.size() != n) {
        rowAcc_.assign(n, {});
    } else {
        for (auto& r : rowAcc_) r.clear();
    }
    for (auto& r : rowAcc_) r.reserve(2 * Dim + 2);

    // Compact Rhie-Chow operator: the two-point face-normal Laplacian.
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
        double rowB = -divU_[r];
        std::size_t emitted = 0;
        std::size_t last = 0;
        for (std::size_t idx = 1; idx <= entries.size(); ++idx) {
            if (idx == entries.size() || entries[idx].first != entries[last].first) {
                double val = 0.0;
                for (std::size_t k = last; k < idx; ++k) val += entries[k].second;

                const std::size_t col = entries[last].first;
                // For compact (SPD) operator: eliminate column entries to pinned cells
                // by shifting their known contribution to the RHS. This preserves
                // symmetry: A_ij = A_ji = 0 for pinned j, and b_i += val * p_j.
                if (col < pMask_.size() && pMask_[col]) {
                    rowB += val * pVals_[col];
                } else {
                    triplets_.emplace_back(r, col, val);
                    ++emitted;
                }
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

    std::size_t patternHash = 0;
    for (std::size_t i = 0; i < pMask_.size(); ++i) {
        if (pMask_[i]) patternHash ^= (i * 0x9e3779b97f4a7c15ULL + 1);
    }
    patternHash ^= (triplets_.size() * 0x85ebca6b);

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