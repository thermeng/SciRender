#include "fvm/Momentum.h"
#include "fvm/Gradient.h"
#include "fvm/SSTConstants.h"
#include "mesh/BoundaryUtils.h"
#include <cmath>

namespace fvm {

template<std::size_t Dim>
Momentum<Dim>::Momentum(const Mesh<Dim>& mesh, double rho, double nu)
    : mesh_(mesh), rho_(rho), nu_(nu) {}

template<std::size_t Dim>
void Momentum<Dim>::ensureWorkspace_(std::size_t n) const {
    if (!workspaceInitialized_ || diagSum_.size() != n) {
        diagSum_.resize(n);
        diagInc_.resize(n);
        triplets_.reserve(n * (2 * Dim + 1));
        workspaceInitialized_ = true;
    }
}

template<std::size_t Dim>
void Momentum<Dim>::buildMomentumScatterMap_(SparseMatrix& A, std::size_t d) const {
    const std::size_t nt = triplets_.size();
    auto& pos = momTripletPos_[d];
    pos.resize(nt);
    const int* outer = A.outerIndexPtr();
    const int* inner = A.innerIndexPtr();
    for (std::size_t t = 0; t < nt; ++t) {
        int col = static_cast<int>(triplets_[t].col());
        int row = static_cast<int>(triplets_[t].row());
        int p = -1;
        for (int k = outer[col]; k < outer[col + 1]; ++k) {
            if (inner[k] == row) { p = k; break; }
        }
        pos[t] = p;
    }
    // Build diagonal position cache for O(n) dCoeff
    const std::size_t n = mesh_.nCells();
    auto& dpos = momDiagPos_[d];
    dpos.assign(n, -1);
    for (std::size_t i = 0; i < n; ++i) {
        int col = static_cast<int>(i);
        int row = static_cast<int>(i);
        for (int k = outer[col]; k < outer[col + 1]; ++k) {
            if (inner[k] == row) { dpos[i] = k; break; }
        }
    }
}

template<std::size_t Dim>
void Momentum<Dim>::updateMomentumValues_(SparseMatrix& A, std::size_t d) const {
    auto& pos = momTripletPos_[d];
    if (pos.size() != triplets_.size()) {
        buildMomentumScatterMap_(A, d);
        return;
    }
    double* vals = A.valuePtr();
    for (std::size_t t = 0; t < triplets_.size(); ++t) {
        int p = pos[t];
        if (p >= 0) vals[p] = triplets_[t].value();
    }
}

template<std::size_t Dim>
Field<Dim> Momentum<Dim>::assemble(
    std::size_t d,
    const VectorField<Dim>& velocity,
    const Field<Dim>& pressure,
    const Field<Dim>& source,
    const std::vector<char>& dirichletMask,
    const std::vector<double>& dirichletValues,
    SparseMatrix& A,
    Eigen::VectorXd& b,
    const Field<Dim>* nutField,
    const std::array<Field<Dim>, Dim>* velGrad) {

    const std::size_t n = mesh_.nCells();
    const std::size_t nF = mesh_.nFaces();
    const bool hasMask = !dirichletMask.empty();
    const bool hasGrad = (velGrad != nullptr);
    const bool hasSource = source.size() > 0;

    ensureWorkspace_(n);
    diagSum_.fill(0.0);
    triplets_.clear();

    A.resize(n, n);
    b.resize(n);
    b.setZero();

    Field<Dim> dpdx = pressureGradientField_(d, pressure);

    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = mesh_.owner(f);
        const std::size_t N = mesh_.neighbor(f);
        if (N == std::size_t(-1)) {
            if (!(hasMask && dirichletMask[P])) {
                const double dist = mesh_.faceDist(f);
                if (dist > minFaceDist) {
                    const double area = mesh_.faceArea(f);
                    const auto& fn = mesh_.faceNormal(f);
                    const auto& xP = mesh_.cellCenter(P);
                    const auto& xF = mesh_.faceCenter(f);
                    double dotSfd = 0.0;
                    for (std::size_t dd = 0; dd < Dim; ++dd)
                        dotSfd += fn[dd] * (xF[dd] - xP[dd]);
                    const double coeff = rho_ * nuEff_(P, P, nutField) * area * dotSfd / (dist * dist);
                    diagSum_[P] += coeff;
                }
            }
            continue;
        }

        const double dist = mesh_.faceDist(f);
        const double area = mesh_.faceArea(f);
        const auto& fn = mesh_.faceNormal(f);
        const auto& xP = mesh_.cellCenter(P);
        const auto& xN = mesh_.cellCenter(N);

        std::array<double, Dim> delta{};
        for (std::size_t dd = 0; dd < Dim; ++dd)
            delta[dd] = xN[dd] - xP[dd];

        double dotSfd = 0.0;
        for (std::size_t dd = 0; dd < Dim; ++dd)
            dotSfd += fn[dd] * delta[dd];

        const double nuEffPN = nuEff_(P, N, nutField);
        bool negativeDiffusion = (dotSfd < 0.0);
        double absDotSfd = negativeDiffusion ? -dotSfd : dotSfd;
        const double coeff = rho_ * nuEffPN * area * absDotSfd / (dist * dist);

        double correction = 0.0;
        if (hasGrad) {
            double dot_f_dhat = absDotSfd / dist;
            for (std::size_t dd = 0; dd < Dim; ++dd) {
                double Sf_nonorth_dd = area * fn[dd] - area * dot_f_dhat * delta[dd] / dist;
                correction += rho_ * nuEffPN * (*velGrad)[dd][P] * Sf_nonorth_dd;
            }
        }

        if (negativeDiffusion) {
            if (!(hasMask && dirichletMask[P])) {
                b(P) += coeff * (hasMask && dirichletMask[N] ? dirichletValues[N] : 0.0);
                if (hasGrad) b(P) -= correction;
            }
            if (!(hasMask && dirichletMask[N])) {
                b(N) += coeff * (hasMask && dirichletMask[P] ? dirichletValues[P] : 0.0);
                if (hasGrad) b(N) += correction;
            }
        } else {
            if (!(hasMask && dirichletMask[P])) {
                if (hasMask && dirichletMask[N]) {
                    b(P) += coeff * dirichletValues[N];
                } else {
                    triplets_.emplace_back(P, N, -coeff);
                }
                diagSum_[P] += coeff;
                if (hasGrad) b(P) -= correction;
            }

            if (!(hasMask && dirichletMask[N])) {
                if (hasMask && dirichletMask[P]) {
                    b(N) += coeff * dirichletValues[P];
                } else {
                    triplets_.emplace_back(N, P, -coeff);
                }
                diagSum_[N] += coeff;
                if (hasGrad) b(N) += correction;
            }
        }
    }

    for (std::size_t P = 0; P < n; ++P) {
        if (hasMask && dirichletMask[P]) {
            triplets_.emplace_back(P, P, 1.0);
            b(P) = dirichletValues[P];
        } else {
            triplets_.emplace_back(P, P, diagSum_[P]);
            if (hasSource) {
                b(P) += source[P] * mesh_.cellVolume(P);
            }
            b(P) -= mesh_.cellVolume(P) * dpdx[P];
        }
    }

    // Pattern-cache assembly: O(nnz log nnz) only on topology change
    {
        std::size_t patternHash = triplets_.size() * 0x85ebca6bULL + n * 31ULL;
        if (hasMask) {
            for (std::size_t i = 0; i < n; ++i) if (dirichletMask[i]) patternHash ^= (i * 0x9e3779b97f4a7c15ULL + 1);
        }
        // Distinguish diffusion-only vs convective pattern (extra flag)
        patternHash ^= 0x9e3779b97f4a7c15ULL;
        if (d >= momPatternHash_.size()) d = 0;
        if (!momPatternBuilt_[d] || patternHash != momPatternHash_[d]) {
            A.resize(n, n);
            A.setFromTriplets(triplets_.begin(), triplets_.end());
            A.makeCompressed();
            momPatternHash_[d] = patternHash;
            momPatternBuilt_[d] = true;
            buildMomentumScatterMap_(A, d);
        } else {
            // Ensure size matches; Eigen may have compressed with same nnz
            if (static_cast<std::size_t>(A.rows()) != n || static_cast<std::size_t>(A.cols()) != n || A.nonZeros() != static_cast<Eigen::Index>(triplets_.size())) {
                A.resize(n, n);
                A.setFromTriplets(triplets_.begin(), triplets_.end());
                A.makeCompressed();
                buildMomentumScatterMap_(A, d);
            } else {
                updateMomentumValues_(A, d);
            }
        }
    }

    Field<Dim> diagField(mesh_, 0.0);
    for (std::size_t P = 0; P < n; ++P) diagField[P] = diagSum_[P];
    return diagField;
}

template<std::size_t Dim>
void Momentum<Dim>::assembleWithConvection(
    std::size_t d,
    const VectorField<Dim>& velocity,
    const Field<Dim>& pressure,
    const Field<Dim>& source,
    const std::vector<char>& dirichletMask,
    const std::vector<double>& dirichletValues,
    SparseMatrix& A,
    Eigen::VectorXd& b,
    Field<Dim>& diag,
    ConvectionScheme scheme,
    const Field<Dim>* nutField,
    const std::array<Field<Dim>, Dim>* phiGrad,
    const std::vector<std::vector<char>>& dirichletMaskAll,
    const std::vector<std::vector<double>>& dirichletValsAll,
    const MomentumBCs& bcs) {

    Field<Dim> dpdx = pressureGradientField_(d, pressure);
    std::array<Field<Dim>, Dim> pressureGrad{};
    pressureGrad[d] = std::move(dpdx);
    MomentumBCs bcs2 = bcs;
    bcs2.dirichletMaskAll = &dirichletMaskAll;
    bcs2.dirichletValsAll = &dirichletValsAll;
    assembleWithConvection(d, velocity, pressureGrad, source, dirichletMask, dirichletValues,
                           A, b, diag, scheme, nutField, phiGrad, bcs2);
}

template<std::size_t Dim>
void Momentum<Dim>::computeDCoeffAndRelax(SparseMatrix& A, Eigen::VectorXd& b,
                                          const std::vector<char>& dirichletMask,
                                          const Field<Dim>& velocityOld,
                                          double alpha,
                                          std::vector<double>& dCoeff,
                                          bool simplec) const {
    const std::size_t n = mesh_.nCells();
    dCoeff.resize(n);
    A.makeCompressed();
    double* vals = A.valuePtr();
    const int* outer = A.outerIndexPtr();
    const int* inner = A.innerIndexPtr();

    // O(deg) diagonal lookup via ColMajor outer/inner (deg≈5–7, cheaper than coeff binary search)
    auto getDiag = [&](std::size_t i, int& posOut) -> double {
        for (int k = outer[i]; k < outer[i + 1]; ++k) {
            if (inner[k] == static_cast<int>(i)) {
                posOut = k;
                return vals[k];
            }
        }
        posOut = -1;
        return A.coeff(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(i));
    };

    // First pass: compute dCoeff
    // Precompute per-row off-diagonal magnitude sums. A is column-major, so
    // iterating InnerIterator(A, i) would visit column i, not row i.
    std::vector<double> rowOffSum(n, 0.0);
    {
        const double* vals = A.valuePtr();
        const int* outer = A.outerIndexPtr();
        const int* inner = A.innerIndexPtr();
        for (Eigen::Index j = 0; j < static_cast<Eigen::Index>(n); ++j) {
            for (int k = outer[j]; k < outer[j + 1]; ++k) {
                int r = inner[k];
                if (r != j) rowOffSum[static_cast<std::size_t>(r)] += std::abs(vals[k]);
            }
        }
    }
    // We also cache diagonal positions for the relaxation pass to avoid re-scan
    std::vector<int> diagPos(n, -1);
    const double* volPtr = mesh_.cellVolumePtr();
    for (std::size_t i = 0; i < n; ++i) {
        int p = -1;
        double aP = getDiag(i, p);
        diagPos[i] = p;
        double vol = volPtr[i];
        if (simplec) {
            double sumOff = (i < rowOffSum.size()) ? rowOffSum[i] : 0.0;
            double denom = aP - sumOff;
            if (denom < simplexMinDenomRatio * aP) denom = simplexMinDenomRatio * aP;
            // The relaxation below rescales the diagonal to aP/alpha, so the
            // consistent pressure-correction coefficient carries the same
            // factor (Patankar's SIMPLEC with under-relaxation).
            dCoeff[i] = (denom > minFaceDist) ? vol * alpha / denom : 0.0;
        } else {
            dCoeff[i] = (aP > minFaceDist) ? vol * alpha / aP : 0.0;
        }
        if (!dirichletMask.empty() && dirichletMask[i]) dCoeff[i] = 0.0;
    }

    // NOTE: this used to `return` early for simplec, which silently skipped
    // velocity under-relaxation entirely — `alphaU` had no effect at all with
    // simplec enabled. Under-relaxation is required for SIMPLEC stability, and
    // dCoeff above is now written consistently with it.
    const double relax = (1.0 - alpha) / alpha;
    const double invAlpha = 1.0 / alpha;
    if (relax <= 1e-14) return;   // alpha == 1: nothing to do
    for (std::size_t i = 0; i < n; ++i) {
        if (!dirichletMask.empty() && dirichletMask[i]) continue;
        int p = diagPos[i];
        double aP;
        if (p >= 0) aP = vals[p];
        else aP = A.coeff(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(i));
        if (aP > 1e-15) {
            b(static_cast<Eigen::Index>(i)) += relax * aP * velocityOld[i];
            if (p >= 0) vals[p] = aP / alpha;
            else A.coeffRef(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(i)) = aP / alpha;
        }
    }
}

template<std::size_t Dim>
const Mesh<Dim>& Momentum<Dim>::mesh() const { return mesh_; }

template<std::size_t Dim>
double Momentum<Dim>::nuEff_(std::size_t P, std::size_t N,
                             const Field<Dim>* nutField) const {
    double nuEff = nu_;
    if (nutField) {
        nuEff += 0.5 * ((*nutField)[P] + (*nutField)[N]);
    }
    return nuEff;
}

template<std::size_t Dim>
Field<Dim> Momentum<Dim>::pressureGradientField_(std::size_t d,
                                                 const Field<Dim>& pressure) const {
    const std::size_t n = mesh_.nCells();
    const std::size_t nF = mesh_.nFaces();
    Field<Dim> grad(mesh_, 0.0);

    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = mesh_.owner(f);
        const std::size_t N = mesh_.neighbor(f);

        double pFace;
        if (N == std::size_t(-1)) {
            pFace = pressure[P];
        } else {
            double w = mesh_.faceWeight(f);
            pFace = (1.0 - w) * pressure[P] + w * pressure[N];
        }

        double normalD = mesh_.faceNormal(f)[d];
        double contrib = pFace * normalD * mesh_.faceArea(f);

        grad[P] += contrib / mesh_.cellVolume(P);
        if (N != std::size_t(-1)) {
            grad[N] -= contrib / mesh_.cellVolume(N);
        }
    }

    return grad;
}

template<std::size_t Dim>
void Momentum<Dim>::computePressureGradientFused_(const Field<Dim>& pressure,
                                                 std::array<Field<Dim>, Dim>& grad) const {
    const std::size_t n = mesh_.nCells();
    const std::size_t nF = mesh_.nFaces();
    for (auto& g : grad) g = Field<Dim>(mesh_, 0.0);

    // Precompute 1/vol to replace divisions in inner loop
    std::vector<double> invVol(n);
    const double* volPtr = mesh_.cellVolumePtr();
    for (std::size_t i = 0; i < n; ++i) {
        double v = volPtr[i];
        invVol[i] = (v > 1e-30) ? 1.0 / v : 0.0;
    }
    const std::size_t* ownerPtr = mesh_.ownerPtr();
    const std::size_t* neighborPtr = mesh_.neighborPtr();
    const double* areaPtr = mesh_.faceAreaPtr();
    const double* weightPtr = mesh_.faceWeightPtr();

    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = ownerPtr[f];
        const std::size_t N = neighborPtr[f];
        double w = weightPtr[f];
        double pFace = (N == std::size_t(-1)) ? pressure[P] : (1.0 - w) * pressure[P] + w * pressure[N];
        const double* fn = mesh_.faceNormalPtr(f);
        double area = areaPtr[f];
        for (std::size_t d = 0; d < Dim; ++d) {
            double contrib = pFace * fn[d] * area;
            grad[d][P] += contrib * invVol[P];
            if (N != std::size_t(-1)) grad[d][N] -= contrib * invVol[N];
        }
    }
}

template<std::size_t Dim>
void Momentum<Dim>::computePressureGradient(const Field<Dim>& pressure,
                                            std::array<Field<Dim>, Dim>& grad) const {
    computePressureGradientFused_(pressure, grad);
}

template<std::size_t Dim>
void Momentum<Dim>::assembleWithConvection(
    std::size_t d,
    const VectorField<Dim>& velocity,
    const std::array<Field<Dim>, Dim>& pressureGrad,
    const Field<Dim>& source,
    const std::vector<char>& dirichletMask,
    const std::vector<double>& dirichletValues,
    SparseMatrix& A,
    Eigen::VectorXd& b,
    Field<Dim>& diag,
    ConvectionScheme scheme,
    const Field<Dim>* nutField,
    const std::array<Field<Dim>, Dim>* phiGrad,
    const MomentumBCs& bcs) {

    const std::size_t n = mesh_.nCells();
    const std::size_t nF = mesh_.nFaces();
    const bool hasMask = !dirichletMask.empty();
    const bool hasMaskAll = bcs.dirichletMaskAll && !bcs.dirichletMaskAll->empty();
    const bool hasSource = source.size() > 0;

    ensureWorkspace_(n);
    diagSum_.fill(0.0);
    diagInc_.fill(0.0);
    triplets_.clear();

    A.resize(n, n);
    b.resize(n);
    b.setZero();
    diag = Field<Dim>(mesh_, 0.0);

    const Field<Dim>& dpdx = pressureGrad[d];
    const double* areaPtr = mesh_.faceAreaPtr();
    const double* distPtr = mesh_.faceDistPtr();
    const double* volPtr = mesh_.cellVolumePtr();
    const std::size_t* ownerPtr = mesh_.ownerPtr();
    const std::size_t* neighborPtr = mesh_.neighborPtr();

    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = ownerPtr[f];
        const std::size_t N = neighborPtr[f];
        if (N == std::size_t(-1)) {
            const double dist = distPtr[f];
            if (dist > minFaceDist) {
                const double area = areaPtr[f];
                const double* fn = mesh_.faceNormalPtr(f);
                const double* xP = mesh_.cellCenterPtr(P);
                const double* xF = mesh_.faceCenterPtr(f);
                double dotSfd = 0.0;
                for (std::size_t dd = 0; dd < Dim; ++dd)
                    dotSfd += fn[dd] * (xF[dd] - xP[dd]);
                // Face-based slip handling (exact normal) — supersedes cell-based bcs.slipWallMask shim
                bool isSlipFace = false;
                bool isOutletFace = false;
                if (bcs.faceBC && bcs.faceHasBC && f < bcs.faceBC->size() && f < bcs.faceHasBC->size() && (*bcs.faceHasBC)[f]) {
                    auto t = (*bcs.faceBC)[f].type;
                    if (t == VelocityBCType::SlipWall || t == VelocityBCType::Symmetry) {
                        std::size_t maxD = 0;
                        double maxAbs = 0.0;
                        for (std::size_t dd = 0; dd < Dim; ++dd) {
                            double a = std::abs(fn[dd]);
                            if (a > maxAbs) { maxAbs = a; maxD = dd; }
                        }
                        // Only the dominant normal component is treated as Dirichlet
                        // (u.n = 0); all other components are tangential (slip).
                        isSlipFace = (d != maxD);
                    } else if (t == VelocityBCType::PressureOutlet || t == VelocityBCType::Outlet) {
                        isOutletFace = true;
                    }
                } else {
                    isSlipFace = (bcs.slipWallMask && !bcs.slipWallMask->empty() && (*bcs.slipWallMask)[P]);
                }
                // Diffusion at boundary: Dirichlet (NoSlip/MovingWall/Inlet) => flux = D*(phi_cell-phi_face)/dist
                // Neumann (Outlet/Symmetry/Slip tangential) => flux 0
                if (!isSlipFace && !isOutletFace) {
                    const double diffCoeff = rho_ * nuEff_(P, P, nutField) * area * dotSfd / (dist * dist);
                    diagSum_[P] += diffCoeff;
                    // General Dirichlet handling for face-based BCs (unstructured)
                    if (bcs.faceBC && bcs.faceHasBC && f < bcs.faceBC->size() && f < bcs.faceHasBC->size() && (*bcs.faceHasBC)[f]) {
                        auto t2 = (*bcs.faceBC)[f].type;
                        if (t2 == VelocityBCType::NoSlip || t2 == VelocityBCType::MovingWall || t2 == VelocityBCType::Inlet) {
                            double phiFace = 0.0;
                            if (t2 == VelocityBCType::MovingWall || t2 == VelocityBCType::Inlet) {
                                const auto &fbc2 = (*bcs.faceBC)[f];
                                phiFace = (d == 0 ? fbc2.u : (d == 1 ? fbc2.v : fbc2.w));
                            }
                            if (!(hasMask && dirichletMask[P])) {
                                b(P) += diffCoeff * phiFace;
                            }
                        }
                    }
                }

                if (!(hasMask && dirichletMask[P])) {
                    // Determine if this is an outlet/pressure-outlet face for Fluent-like handling
                    bool isOutlet = false;
                    bool suppressBackflow = false;
                    double backflowU[3] = {0,0,0};
                    if (bcs.faceBC && bcs.faceHasBC && f < bcs.faceBC->size() && f < bcs.faceHasBC->size() && (*bcs.faceHasBC)[f]) {
                        auto t = (*bcs.faceBC)[f].type;
                        if (t == VelocityBCType::PressureOutlet || t == VelocityBCType::Outlet) {
                            isOutlet = true;
                            suppressBackflow = (*bcs.faceBC)[f].suppressBackflow;
                            backflowU[0] = (*bcs.faceBC)[f].backflowU;
                            backflowU[1] = (*bcs.faceBC)[f].backflowV;
                            backflowU[2] = (*bcs.faceBC)[f].backflowW;
                        }
                    }
                    double un = 0.0;
                    // First compute un with appropriate face velocity (for outlet: interior for outflow)
                    for (std::size_t dd = 0; dd < Dim; ++dd) {
                        double uFace;
                        if (bcs.faceBC && bcs.faceHasBC && f < bcs.faceBC->size() && f < bcs.faceHasBC->size() && (*bcs.faceHasBC)[f]) {
                            auto t = (*bcs.faceBC)[f].type;
                            if (t == VelocityBCType::Inlet || t == VelocityBCType::MovingWall) {
                                const auto &fbc = (*bcs.faceBC)[f];
                                uFace = (dd == 0 ? fbc.u : (dd == 1 ? fbc.v : fbc.w));
                            } else if (t == VelocityBCType::NoSlip) {
                                uFace = 0.0;
                            } else if (t == VelocityBCType::Symmetry || t == VelocityBCType::SlipWall) {
                                double vn = 0.0;
                                for (std::size_t k = 0; k < Dim; ++k) vn += velocity[k][P] * fn[k];
                                double interior = velocity[dd][P];
                                uFace = interior - vn * fn[dd];
                            } else if (t == VelocityBCType::PressureOutlet || t == VelocityBCType::Outlet) {
                                // Fluent outlet: zero-gradient for outflow, backflow Dirichlet for inflow
                                // Use interior for now, corrected below if backflow
                                uFace = velocity[dd][P];
                            } else {
                                if (bcs.dirichletMaskAll && !bcs.dirichletMaskAll->empty() && !(*bcs.dirichletMaskAll)[dd].empty() && (*bcs.dirichletMaskAll)[dd][P]) {
                                    uFace = (*bcs.dirichletValsAll)[dd][P];
                                } else {
                                    uFace = velocity[dd][P];
                                }
                            }
                        } else {
                            if (bcs.dirichletMaskAll && !bcs.dirichletMaskAll->empty() && !(*bcs.dirichletMaskAll)[dd].empty() && (*bcs.dirichletMaskAll)[dd][P]) {
                                uFace = (*bcs.dirichletValsAll)[dd][P];
                            } else {
                                uFace = velocity[dd][P];
                            }
                        }
                        un += uFace * fn[dd];
                    }
                    // Fluent backflow handling for outlet - use shared helper
                    if (bcs.faceBC && bcs.faceHasBC && f < bcs.faceBC->size() && f < bcs.faceHasBC->size() && (*bcs.faceHasBC)[f]) {
                        const auto& fbc = (*bcs.faceBC)[f];
                        std::array<double, Dim> fnArr{};
                        for (std::size_t dd = 0; dd < Dim; ++dd) fnArr[dd] = fn[dd];
                        un = applyBackflowCorrection<Dim>(un, fnArr, fbc);
                    }
                    double F = rho_ * un * area;
                    // For outlet with suppressed backflow, F==0, no contribution
                    if (isOutlet && un == 0.0 && F == 0.0) {
                        // No flux, nothing to add
                    } else if (F >= 0.0) {
                        diagInc_[P] += F;
                        // SOU outflow boundary: second-order extrapolation via gradient.
                        //
                        // The reconstructed face value is phi_f = phi_up + corr, so the
                        // face contributes F*(phi_up + corr) to the owner row. The base
                        // upwind assembly already carries F*phi_up, therefore the known
                        // part F*corr moves to the right-hand side with a sign flip
                        // (b(P) -= F*corr). Adding it with the opposite sign turns the
                        // bounded second-order reconstruction into a downwind one.
                        if (scheme == ConvectionScheme::SecondOrderUpwind && phiGrad != nullptr) {
                            const double* xUp = mesh_.cellCenterPtr(P);
                            double gradDotR = 0.0;
                            for (std::size_t dd = 0; dd < Dim; ++dd) {
                                double r = xF[dd] - xUp[dd];
                                gradDotR += (*phiGrad)[dd][P] * r;
                            }
                            double phiUp = velocity[d][P];
                            double phiMin = phiUp, phiMax = phiUp;
                            for (auto nb : mesh_.cellNeighbors(P)) {
                                double v = (hasMask && dirichletMask[nb]) ? dirichletValues[nb] : velocity[d][nb];
                                if (v < phiMin) phiMin = v;
                                if (v > phiMax) phiMax = v;
                            }
                            double phiF = phiUp + gradDotR;
                            if (phiF > phiMax) phiF = phiMax;
                            else if (phiF < phiMin) phiF = phiMin;
                            b(P) -= F * (phiF - phiUp);
                        }
                    } else {
                        double phiUpwind;
                        if (bcs.faceBC && bcs.faceHasBC && f < bcs.faceBC->size() && f < bcs.faceHasBC->size() && (*bcs.faceHasBC)[f]) {
                            auto t = (*bcs.faceBC)[f].type;
                            if (t == VelocityBCType::Inlet || t == VelocityBCType::MovingWall) {
                                const auto &fbc = (*bcs.faceBC)[f];
                                phiUpwind = (d == 0 ? fbc.u : (d == 1 ? fbc.v : fbc.w));
                            } else if (t == VelocityBCType::NoSlip) {
                                phiUpwind = 0.0;
                            } else if (t == VelocityBCType::Symmetry || t == VelocityBCType::SlipWall) {
                                double vn = 0.0;
                                for (std::size_t k = 0; k < Dim; ++k) vn += velocity[k][P] * fn[k];
                                phiUpwind = velocity[d][P] - vn * fn[d];
                            } else if (t == VelocityBCType::PressureOutlet || t == VelocityBCType::Outlet) {
                                // Backflow Dirichlet - get from FaceVelocityBC
                                const auto &fbc = (*bcs.faceBC)[f];
                                phiUpwind = (d == 0 ? fbc.backflowU : (d == 1 ? fbc.backflowV : fbc.backflowW));
                            } else {
                                phiUpwind = (hasMaskAll && bcs.dirichletMaskAll && !bcs.dirichletMaskAll->empty() && !(*bcs.dirichletMaskAll)[d].empty() && (*bcs.dirichletMaskAll)[d][P])
                                           ? (*bcs.dirichletValsAll)[d][P] : velocity[d][P];
                            }
                        } else {
                            phiUpwind = (hasMaskAll && bcs.dirichletMaskAll && !bcs.dirichletMaskAll->empty() && !(*bcs.dirichletMaskAll)[d].empty() && (*bcs.dirichletMaskAll)[d][P])
                                       ? (*bcs.dirichletValsAll)[d][P] : velocity[d][P];
                        }
                        b(P) -= F * phiUpwind;
                    }
                }
            }
            continue;
        }

        const double dist = distPtr[f];
        const double area = areaPtr[f];
        const double* fn = mesh_.faceNormalPtr(f);
        const double* xP = mesh_.cellCenterPtr(P);
        const double* xN = mesh_.cellCenterPtr(N);

        double delta[Dim];
        for (std::size_t dd = 0; dd < Dim; ++dd)
            delta[dd] = xN[dd] - xP[dd];

        double dotSfd = 0.0;
        for (std::size_t dd = 0; dd < Dim; ++dd)
            dotSfd += fn[dd] * delta[dd];

        const double nuEffPN = nuEff_(P, N, nutField);
        bool negativeDiffusion = (dotSfd < 0.0);
        double absDotSfd = negativeDiffusion ? -dotSfd : dotSfd;
        double diffCoeff = rho_ * nuEffPN * area * absDotSfd / (dist * dist);

        double correction = 0.0;
        if (phiGrad != nullptr) {
            double dot_f_dhat = absDotSfd / dist;
            for (std::size_t dd = 0; dd < Dim; ++dd) {
                double Sf_nonorth_dd = area * fn[dd] - area * dot_f_dhat * delta[dd] / dist;
                correction += rho_ * nuEffPN * (*phiGrad)[dd][P] * Sf_nonorth_dd;
            }
            correction *= nonOrthCorrectionFactor;
        }

        if (negativeDiffusion) {
            diagSum_[P] += diffCoeff;
            diagSum_[N] += diffCoeff;
            if (!(hasMask && dirichletMask[P])) {
                b(P) += diffCoeff * (hasMask && dirichletMask[N] ? dirichletValues[N] : 0.0);
                if (phiGrad != nullptr) b(P) -= correction;
            }
            if (!(hasMask && dirichletMask[N])) {
                b(N) += diffCoeff * (hasMask && dirichletMask[P] ? dirichletValues[P] : 0.0);
                if (phiGrad != nullptr) b(N) += correction;
            }
        } else {
            diagSum_[P] += diffCoeff;
            diagSum_[N] += diffCoeff;

            if (!(hasMask && dirichletMask[P])) {
                if (hasMask && dirichletMask[N]) {
                    b(P) += diffCoeff * dirichletValues[N];
                } else {
                    triplets_.emplace_back(P, N, -diffCoeff);
                }
                if (phiGrad != nullptr) b(P) -= correction;
            }

            if (!(hasMask && dirichletMask[N])) {
                if (hasMask && dirichletMask[P]) {
                    b(N) += diffCoeff * dirichletValues[P];
                } else {
                    triplets_.emplace_back(N, P, -diffCoeff);
                }
                if (phiGrad != nullptr) b(N) += correction;
            }
        }

        {
            double w = mesh_.faceWeight(f);
            double un = 0.0;
            for (std::size_t dd = 0; dd < Dim; ++dd) {
                double uFace;
                if (hasMaskAll && bcs.dirichletMaskAll && !bcs.dirichletMaskAll->empty() && !(*bcs.dirichletMaskAll)[dd].empty() && (*bcs.dirichletMaskAll)[dd][N]) uFace = (*bcs.dirichletValsAll)[dd][N];
                else if (hasMaskAll && bcs.dirichletMaskAll && !bcs.dirichletMaskAll->empty() && !(*bcs.dirichletMaskAll)[dd].empty() && (*bcs.dirichletMaskAll)[dd][P]) uFace = (*bcs.dirichletValsAll)[dd][P];
                else uFace = (1.0 - w) * velocity[dd][P] + w * velocity[dd][N];
                un += uFace * fn[dd];
            }
            double F = rho_ * un * area;
            // The base assembly always carries the UPWIND face value. The
            // second-order reconstruction (SOU/QUICK) enters afterwards as a
            // deferred correction, so the base must stay plain upwind for those
            // schemes too. QUICK shares the same base.
            const bool isUpwindBase = (scheme == ConvectionScheme::Upwind ||
                                       scheme == ConvectionScheme::SecondOrderUpwind ||
                                       scheme == ConvectionScheme::QUICK);
            auto addConvFluxUnified = [&](double flux, std::size_t from, std::size_t to) {
                if (isUpwindBase) {
                    if (flux >= 0.0) diagInc_[from] += flux;
                    else {
                        if (hasMask && dirichletMask[to]) b(from) -= flux * dirichletValues[to];
                        else if (!(hasMask && dirichletMask[from])) triplets_.emplace_back(from, to, flux);
                    }
                } else {
                    // Central
                    diagInc_[from] += 0.5 * flux;
                    if (hasMask && dirichletMask[to]) b(from) -= 0.5 * flux * dirichletValues[to];
                    else if (!(hasMask && dirichletMask[from])) triplets_.emplace_back(from, to, 0.5 * flux);
                }
            };
            addConvFluxUnified(F, P, N);
            addConvFluxUnified(-F, N, P);

            // ---- deferred reconstruction (second order upwind / QUICK) ----
            //
            // The reconstructed face value is phi_f = phi_up + corr. The base
            // upwind assembly already carries phi_up on the two rows, so only
            // the increment is left: the upwind (donor) cell loses F*corr from
            // its left-hand side and the downwind cell gains it.
            //
            // BOTH shares are required. Dropping the receiver's share, or
            // flipping the sign, makes the convective operator
            // non-conservative and turns the bounded second-order scheme into
            // an unbounded downwind one. This mirrors the scalar Convection
            // operator (src/fvm/Convection.cpp), which is the tested
            // reference for this contract.
            if (scheme == ConvectionScheme::SecondOrderUpwind ||
                scheme == ConvectionScheme::QUICK) {
                const bool upIsP = (F >= 0.0);
                const std::size_t upCell = upIsP ? P : N;
                const std::size_t downCell = upIsP ? N : P;
                const bool upPrescribed = hasMask && dirichletMask[upCell];
                const bool downPrescribed = hasMask && dirichletMask[downCell];
                const double fluxMag = std::abs(F);

                double corr = 0.0;
                bool haveCorr = false;

                if (scheme == ConvectionScheme::QUICK) {
                    // Three-point quadratic reconstruction (unbounded by design -
                    // the same reconstruction the scalar Convection operator
                    // uses).
                    std::array<double, Dim> fnArr{};
                    for (std::size_t dd = 0; dd < Dim; ++dd) fnArr[dd] = fn[dd];
                    auto upPair = secondOrderUpwindCells(mesh_, P, N, un, fnArr);
                    const std::size_t up = upPair.first;
                    const std::size_t up2 = upPair.second;
                    if (up != up2 && up != downCell && up2 != downCell) {
                        const double* xUp2 = mesh_.cellCenterPtr(up2);
                        const double* xUp = mesh_.cellCenterPtr(up);
                        const double* xDown = mesh_.cellCenterPtr(downCell);
                        const double* xFp = mesh_.faceCenterPtr(f);
                        auto coord = [&](const double* x) {
                            double s = 0.0;
                            for (std::size_t dd = 0; dd < Dim; ++dd) s += x[dd] * fnArr[dd];
                            return s;
                        };
                        const double s2 = coord(xUp2), sUp = coord(xUp), sDown = coord(xDown), sF = coord(xFp);
                        const double denomA = (s2 - sUp) * (s2 - sDown);
                        const double denomB = (sUp - s2) * (sUp - sDown);
                        const double denomC = (sDown - s2) * (sDown - sUp);
                        if (std::abs(denomA) > 1e-30 && std::abs(denomB) > 1e-30 && std::abs(denomC) > 1e-30) {
                            const double wa = (sF - sUp) * (sF - sDown) / denomA;
                            const double wb = (sF - s2) * (sF - sDown) / denomB;
                            const double wc = (sF - s2) * (sF - sUp) / denomC;
                            auto cellVal = [&](std::size_t c) {
                                return (hasMask && dirichletMask[c]) ? dirichletValues[c] : velocity[d][c];
                            };
                            const double phiF = wa * cellVal(up2) + wb * cellVal(up) + wc * cellVal(downCell);
                            corr = phiF - cellVal(up);
                            haveCorr = true;
                        }
                    }
                } else if (phiGrad != nullptr) {
                    // Gradient reconstruction, clamped to the bounds of the
                    // upwind cell's neighbours (Barth-Jespersen style).
                    const double* xF = mesh_.faceCenterPtr(f);
                    const double* xUp = mesh_.cellCenterPtr(upCell);
                    double gradDotR = 0.0;
                    for (std::size_t dd = 0; dd < Dim; ++dd) {
                        double r = xF[dd] - xUp[dd];
                        gradDotR += (*phiGrad)[dd][upCell] * r;
                    }
                    double phiUp = upPrescribed ? dirichletValues[upCell] : velocity[d][upCell];
                    double phiMin = phiUp, phiMax = phiUp;
                    for (auto nb : mesh_.cellNeighbors(upCell)) {
                        double v = (hasMask && dirichletMask[nb]) ? dirichletValues[nb] : velocity[d][nb];
                        if (v < phiMin) phiMin = v;
                        if (v > phiMax) phiMax = v;
                    }
                    double phiF = phiUp + gradDotR;
                    if (phiF > phiMax) phiF = phiMax;
                    else if (phiF < phiMin) phiF = phiMin;
                    corr = phiF - phiUp;
                    haveCorr = true;
                } else {
                    // Second order upwind without a cell gradient: fall back to
                    // the distance-weighted two-cell extrapolation.
                    std::array<double, Dim> fnArr{};
                    for (std::size_t dd = 0; dd < Dim; ++dd) fnArr[dd] = fn[dd];
                    auto upPair = secondOrderUpwindCells(mesh_, P, N, un, fnArr);
                    const std::size_t up = upPair.first;
                    const std::size_t up2 = upPair.second;
                    if (up != up2) {
                        const double phiUp = (hasMask && dirichletMask[up]) ? dirichletValues[up] : velocity[d][up];
                        const double phiUp2 = velocity[d][up2];
                        const double* xF2 = mesh_.faceCenterPtr(f);
                        const double* xUp = mesh_.cellCenterPtr(up);
                        const double* xUp2 = mesh_.cellCenterPtr(up2);
                        double distF = 0, distUp = 0;
                        for (std::size_t dd = 0; dd < Dim; ++dd) { double df = xF2[dd]-xUp[dd]; double du = xUp[dd]-xUp2[dd]; distF += df*df; distUp += du*du; }
                        distF = std::sqrt(distF); distUp = std::sqrt(distUp);
                        double wgt = (distUp > 1e-12 ? distF/distUp : 0.5); if (wgt > 2.0) wgt = 2.0;
                        double phiF = phiUp + wgt * (phiUp - phiUp2);
                        double phiMin = std::min(phiUp, phiUp2), phiMax = std::max(phiUp, phiUp2);
                        for (auto nb : mesh_.cellNeighbors(up)) { double v = velocity[d][nb]; if (v < phiMin) phiMin = v; if (v > phiMax) phiMax = v; }
                        if (phiF > phiMax) phiF = phiMax; else if (phiF < phiMin) phiF = phiMin;
                        corr = phiF - phiUp;
                        haveCorr = true;
                    }
                }

                if (haveCorr && corr != 0.0 && fluxMag > 0.0) {
                    if (!upPrescribed) b(upCell) -= fluxMag * corr;
                    if (!downPrescribed) b(downCell) += fluxMag * corr;
                }
            }
        }
    }

    for (std::size_t P = 0; P < n; ++P) {
        diagSum_[P] += diagInc_[P];
        if (hasMask && dirichletMask[P]) {
            triplets_.emplace_back(P, P, diagSum_[P]);
            b(P) = diagSum_[P] * dirichletValues[P];
            diag[P] = diagSum_[P];
        } else {
            triplets_.emplace_back(P, P, diagSum_[P]);
            diag[P] = diagSum_[P];
            if (hasSource) {
                b(P) += source[P] * volPtr[P];
            }
            b(P) -= volPtr[P] * dpdx[P];
        }
    }

    // Pattern-cache assembly: O(nnz log nnz) only on topology change
    {
        std::size_t patternHash = triplets_.size() * 0x85ebca6bULL + n * 31ULL;
        patternHash ^= static_cast<std::size_t>(scheme) * 0x9e3779b97f4a7c55ULL;
        if (hasMask) {
            for (std::size_t i = 0; i < n; ++i) if (dirichletMask[i]) patternHash ^= (i * 0x9e3779b97f4a7c15ULL + 1);
        }
        // Include face BC presence in hash
        if (bcs.faceBC && bcs.faceHasBC) {
            for (std::size_t f = 0; f < bcs.faceBC->size() && f < bcs.faceHasBC->size(); ++f) {
                if ((*bcs.faceHasBC)[f]) patternHash ^= (f * 0x9e3779b97f4a7c15ULL + 2);
            }
        }
        // Include phiGrad presence
        if (phiGrad) patternHash ^= 0x85ebca6bULL;

        if (d < momPatternHash_.size() && momPatternBuilt_[d] && patternHash == momPatternHash_[d]) {
            // Pattern unchanged: O(nnz) scatter update
            if (A.rows() == static_cast<Eigen::Index>(n) && A.cols() == static_cast<Eigen::Index>(n) &&
                A.nonZeros() == static_cast<Eigen::Index>(triplets_.size()) &&
                momTripletPos_[d].size() == triplets_.size()) {
                updateMomentumValues_(A, d);
            } else {
                // Size mismatch (should be rare): fall back to full rebuild
                A.resize(n, n);
                A.setFromTriplets(triplets_.begin(), triplets_.end());
                A.makeCompressed();
                buildMomentumScatterMap_(A, d);
            }
        } else {
            // Pattern changed or first time: full rebuild
            A.resize(n, n);
            A.setFromTriplets(triplets_.begin(), triplets_.end());
            A.makeCompressed();
            if (d < momPatternHash_.size()) {
                momPatternHash_[d] = patternHash;
                momPatternBuilt_[d] = true;
                buildMomentumScatterMap_(A, d);
            }
        }
    }
}

template<std::size_t Dim>
void Momentum<Dim>::assembleWithConvectionTransient(
    std::size_t d,
    const VectorField<Dim>& velocity,
    const std::array<Field<Dim>, Dim>& pressureGrad,
    const Field<Dim>& source,
    const std::vector<char>& dirichletMask,
    const std::vector<double>& dirichletValues,
    SparseMatrix& A,
    Eigen::VectorXd& b,
    Field<Dim>& diag,
    double dt,
    double dtPrev,
    const Field<Dim>* oldTime,
    const Field<Dim>* oldTimePrev,
    TimeScheme timeScheme,
    ConvectionScheme convScheme,
    const Field<Dim>* nutField,
    const std::array<Field<Dim>, Dim>* phiGrad,
    const MomentumBCs& bcs) {

    assembleWithConvection(d, velocity, pressureGrad, source, dirichletMask, dirichletValues,
                           A, b, diag, convScheme, nutField, phiGrad, bcs);

    if (dt <= 0.0 || dt > 1e14 || !oldTime) return;

    const std::size_t n = mesh_.nCells();

    // BDF2 coefficients follow dt and dt/dtPrev. Passing dtPrev == 0 (first
    // step, or no usable history) degrades cleanly to explicit Euler rather
    // than applying constant-step coefficients to a varying step.
    ImplicitTimeCoeffs tc;
    if (timeScheme == TimeScheme::BDF2 && oldTimePrev != nullptr) {
        tc = implicitTimeCoeffs(dt, dtPrev);
    }
    const bool useBDF2 = tc.secondOrder;
    if (!useBDF2 && dt > 0.0) {
        tc.c0 = 1.0 / dt;
    }

    A.makeCompressed();
    // Use diagonal position cache to avoid O(n log nnz) coeffRef scans
    double* vals = A.valuePtr();
    const int* outer = A.outerIndexPtr();
    const int* inner = A.innerIndexPtr();
    // Ensure diag cache available for this d
    if (d < momDiagPos_.size() && momDiagPos_[d].size() == n) {
        auto& dpos = momDiagPos_[d];
        for (std::size_t i = 0; i < n; ++i) {
            if (!dirichletMask.empty() && dirichletMask[i]) continue;
            double vol = mesh_.cellVolume(i);
            const double coeff = tc.c0 * rho_ * vol;
            const double rhs = useBDF2
                ? rho_ * vol * (tc.cOld * (*oldTime)[i] + tc.cOldPrev * (*oldTimePrev)[i])
                : coeff * (*oldTime)[i];
            int p = dpos[i];
            if (p >= 0) vals[p] += coeff;
            b(static_cast<Eigen::Index>(i)) += rhs;
            diag[i] += coeff;
        }
    } else {
        for (std::size_t i = 0; i < n; ++i) {
            if (!dirichletMask.empty() && dirichletMask[i]) continue;
            double vol = mesh_.cellVolume(i);
            const double coeff = tc.c0 * rho_ * vol;
            const double rhs = useBDF2
                ? rho_ * vol * (tc.cOld * (*oldTime)[i] + tc.cOldPrev * (*oldTimePrev)[i])
                : coeff * (*oldTime)[i];
            // Fallback binary search via outer/inner
            int col = static_cast<int>(i);
            for (int k = outer[col]; k < outer[col+1]; ++k) {
                if (inner[k] == col) { vals[k] += coeff; break; }
            }
            b(static_cast<Eigen::Index>(i)) += rhs;
            diag[i] += coeff;
        }
    }
}

template class Momentum<2>;
template class Momentum<3>;

} // namespace fvm
