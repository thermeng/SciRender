#include "fvm/PressureCorrection.h"
#include "fvm/SSTConstants.h"
#include "mesh/BoundaryUtils.h"

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
    const std::vector<char>* faceHasBC) {

    const std::size_t n = mesh_.nCells();
    const std::size_t nF = mesh_.nFaces();

    if (!gradientCalc_) {
        gradientCalc_ = std::make_unique<Gradient<Dim>>(mesh_);
        for (auto& g : pressureGrad_) g = Field<Dim>(mesh_, 0.0);
        for (auto& g : pressureGradPrev_) g = Field<Dim>(mesh_, 0.0);
    }

    // Green-Gauss (face-based) gradient for consistency with Momentum
    for (std::size_t d = 0; d < Dim; ++d) pressureGrad_[d].assign(0.0);
    const std::size_t* ownerPtr = mesh_.ownerPtr();
    const std::size_t* neighborPtr = mesh_.neighborPtr();
    const double* areaPtr = mesh_.faceAreaPtr();
    const double* weightPtr = mesh_.faceWeightPtr();
    std::vector<double> invVol(n);
    const double* volPtr = mesh_.cellVolumePtr();
    for (std::size_t i = 0; i < n; ++i) invVol[i] = (volPtr[i] > 1e-30) ? 1.0/volPtr[i] : 0.0;
    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = ownerPtr[f];
        const std::size_t N = neighborPtr[f];
        double w = weightPtr[f];
        double pFace = (N == std::size_t(-1)) ? pressure[P] : (1.0 - w) * pressure[P] + w * pressure[N];
        const double* fn = mesh_.faceNormalPtr(f);
        double area = areaPtr[f];
        for (std::size_t d = 0; d < Dim; ++d) {
            double contrib = pFace * fn[d] * area;
            pressureGrad_[d][P] += contrib * invVol[P];
            if (N != std::size_t(-1)) pressureGrad_[d][N] -= contrib * invVol[N];
        }
    }

    // Reuse pre-allocated buffers - fused single-pass init (was 4*O(n) fills)
    triplets_.clear();
    triplets_.reserve(n * (2 * Dim + 1));
    bVec_.resize(n);
    bVec_.setZero();

    if (pMask_.size() != n) pMask_.resize(n);
    if (pVals_.size() != n) pVals_.resize(n);
    if (isPressureOutlet_.size() != n) isPressureOutlet_.resize(n);
    if (diagSum_.size() != n) diagSum_.resize(n);
    // Single fused O(n) init instead of 4 separate fills
    for (std::size_t i = 0; i < n; ++i) {
        pMask_[i] = false;
        pVals_[i] = 0.0;
        isPressureOutlet_[i] = false;
        diagSum_[i] = 0.0;
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

    std::size_t refCell = config_.pressureRefCell;
    if (refCell >= n) {
        refCell = n;
        for (std::size_t i = 0; i < n; ++i) if (!mesh_.isBoundary(i) && !isPressureOutlet_[i]) { refCell = i; break; }
        if (refCell == n) for (std::size_t i = 0; i < n; ++i) if (!isPressureOutlet_[i]) { refCell = i; break; }
        if (refCell == n) refCell = n - 1;
    }
    if (!pMask_[refCell]) { pMask_[refCell]=true; pVals_[refCell]=0.0; }

    for (std::size_t i = 0; i < n; ++i) {
        if (mesh_.isBoundary(i) && !isPressureNeumann[i] && !isPressureOutlet_[i]) {
            pMask_[i] = true;
            pVals_[i] = 0.0;
        }
    }

    divU_.assign(0.0);
    // diagSum already zeroed in fused init above (was separate O(n) fill)

    const double* distPtr = mesh_.faceDistPtr();

    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = ownerPtr[f];
        const std::size_t N = neighborPtr[f];
        const double* fn = mesh_.faceNormalPtr(f);
        const double area = areaPtr[f];
        const double dist = distPtr[f];

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
                double uFace = (N == std::size_t(-1)) ? uStar[d][P] : (1.0 - w) * uStar[d][P] + w * uStar[d][N];
                un += uFace * fn[d];
            }
        }

        if (config_.rhieChow && N != std::size_t(-1)) {
            un += rhieChowCorrection(mesh_, f, dCoeff, pressure, pressureGrad_, pMask_);
        }

        double flux = un * area;
        divU_[P] += flux;
        if (N != std::size_t(-1)) {
            divU_[N] -= flux;
        }

        // --- Matrix assembly ---
        if (N == std::size_t(-1)) {
        if (!pMask_[P]) {
                const double* xPptr = mesh_.cellCenterPtr(P);
                const double* xFptr = mesh_.faceCenterPtr(f);
                double dotSfd = 0.0;
                for (std::size_t d = 0; d < Dim; ++d) dotSfd += fn[d] * (xFptr[d] - xPptr[d]);
                double absDotSfd = std::abs(dotSfd);
                double dFaceN = 0.0;
                for (std::size_t d = 0; d < Dim; ++d) dFaceN += dCoeff[d][P] * fn[d] * fn[d];
                if (dFaceN > minFaceDist && dist > minFaceDist && absDotSfd > minFaceDist) {
                    diagSum_[P] += dFaceN * area * absDotSfd / (dist * dist);
                }
            }
            continue;
        }

        if (pMask_[P] && pMask_[N]) continue;

        const double* xPptr = mesh_.cellCenterPtr(P);
        const double* xNptr = mesh_.cellCenterPtr(N);

        std::array<double, Dim> dVec{};
        for (std::size_t dd = 0; dd < Dim; ++dd) dVec[dd] = xNptr[dd] - xPptr[dd];

        double dotSfd = 0.0;
        for (std::size_t dd = 0; dd < Dim; ++dd) dotSfd += fn[dd] * dVec[dd];
        double absDotSfd = std::abs(dotSfd);
        double w = mesh_.faceWeight(f);
        double dFaceN = 0.0;
        for (std::size_t dd = 0; dd < Dim; ++dd) {
            double nd = fn[dd];
            double dP_eff = pMask_[P] ? 0.0 : dCoeff[dd][P];
            double dN_eff = pMask_[N] ? 0.0 : dCoeff[dd][N];
            dFaceN += ((1.0 - w) * dP_eff + w * dN_eff) * nd * nd;
        }
        double coeff = (dist > minFaceDist && dFaceN > minFaceDist) ? dFaceN * area * absDotSfd / (dist * dist) : 0.0;

        double correction = 0.0;
        double dot_f_dhat = dotSfd / dist;
        // Non-orthogonal (cross-diffusion) correction is deferred: use the
        // previous outer iteration's pressure gradient so it does not
        // over-correct on skewed cells.
        const auto& pgNonOrth = hasPrevGrad_ ? pressureGradPrev_ : pressureGrad_;
        for (std::size_t dd = 0; dd < Dim; ++dd) {
            double Sf_nonorth_dd = area * fn[dd] - area * dot_f_dhat * dVec[dd] / dist;
            correction += ((1.0 - w) * pgNonOrth[dd][P] + w * pgNonOrth[dd][N]) * Sf_nonorth_dd;
        }
        correction *= dFaceN;
        if (!pMask_[P]) {
            if (pMask_[N]) {
                bVec_(P) += coeff * pVals_[N];
            } else {
                triplets_.emplace_back(P, N, -coeff);
            }
            diagSum_[P] += coeff;
            bVec_(P) -= correction;
        }

        if (!pMask_[N]) {
            if (pMask_[P]) {
                bVec_(N) += coeff * pVals_[P];
            } else {
                triplets_.emplace_back(N, P, -coeff);
            }
            diagSum_[N] += coeff;
            bVec_(N) += correction;
        }
    }

    for (std::size_t P = 0; P < n; ++P) {
        divU_[P] /= mesh_.cellVolume(P);
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
        if (pMask_[P]) {
            triplets_.emplace_back(P, P, 1.0);
            bVec_(P) = pVals_[P];
        } else if (diagSum_[P] <= 0.0) {
            pMask_[P] = true;
            pVals_[P] = 0.0;
            triplets_.emplace_back(P, P, 1.0);
            bVec_(P) = 0.0;
        } else {
            triplets_.emplace_back(P, P, diagSum_[P]);
            bVec_(P) = -divU_[P] * mesh_.cellVolume(P);
        }
    }

    Eigen::VectorXd x;
    bool converged = false;

    std::size_t patternHash = 0;
    for (std::size_t i = 0; i < pMask_.size(); ++i) {
        if (pMask_[i]) patternHash ^= (i * 0x9e3779b97f4a7c15ULL + 1);
    }
    patternHash ^= (triplets_.size() * 0x85ebca6b);

    bool maskChanged = (prevPMask_.size() != pMask_.size());
    if (!maskChanged) {
        for (std::size_t i = 0; i < pMask_.size(); ++i) if (prevPMask_[i] != pMask_[i]) { maskChanged = true; break; }
    }

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
            amg_.setConfig(config_.amgConfig);
            amg_.build(A_);
            amgPrec_.setAMG(&amg_);
            amgBuilt_ = true;
            amgPatternHash_ = patternHash;
            prevPMask_ = pMask_;
            prevNonZeroPattern_ = triplets_.size();
        }

        if (amg_.isBuilt()) {
            if (!pPatternAnalyzed_) {
                bicgSolver_.setMaxIterations(1000);
                bicgSolver_.setTolerance(1e-4);
                bicgSolver_.analyzePattern(A_);
                pPatternAnalyzed_ = true;
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
            amg_.setConfig(config_.amgConfig);
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
        if (!pPatternAnalyzed_ || maskChanged || nz != prevNonZeroPattern_) {
            cgSolver_.setMaxIterations(10000);
            cgSolver_.setTolerance(1e-8);
            cgSolver_.analyzePattern(A_);
            bicgSolver_.setMaxIterations(1000);
            bicgSolver_.setTolerance(1e-4);
            bicgSolver_.analyzePattern(A_);
            bicgFallbackSolver_.setMaxIterations(10000);
            bicgFallbackSolver_.setTolerance(1e-8);
            bicgFallbackSolver_.analyzePattern(A_);
            pPatternAnalyzed_ = true;
            prevPMask_ = pMask_;
            prevNonZeroPattern_ = nz;
        }
        cgSolver_.factorize(A_);
        x = cgSolver_.solve(bVec_);
        converged = (cgSolver_.info() == Eigen::Success);
        if (!converged) {
            std::cerr << "  WARNING: pressure CG failed (iters=" << cgSolver_.iterations()
                      << ", resid=" << cgSolver_.error() << "), falling back to BiCGSTAB\n";
        }
        if (!converged) {
            bicgFallbackSolver_.factorize(A_);
            x = bicgFallbackSolver_.solve(bVec_);

            if (bicgFallbackSolver_.info() != Eigen::Success) {
                std::cerr << "  WARNING: pressure BiCGSTAB fallback also failed (iters=" << bicgFallbackSolver_.iterations()
                          << ", resid=" << bicgFallbackSolver_.error() << ")\n";
            }
        }
    }

    // Defer the non-orthogonal correction gradient for the next outer iteration.
    pressureGradPrev_ = pressureGrad_;
    hasPrevGrad_ = true;

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
