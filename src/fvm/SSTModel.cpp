#include "fvm/SSTModel.h"
#include <algorithm>
#include <cstdio>
#include <Eigen/IterativeLinearSolvers>
#include <queue>

namespace fvm {

namespace {

// Numeric guard on the cross-diffusion source. This is not a boundary value:
// it only stops a single bad cell from poisoning the whole field.
constexpr double crossDiffusionLimit = 1e3;

inline double clampPositive(double v, double lo) {
    return (v > lo) ? v : lo;
}

// SparseMatrix is Eigen::SparseMatrix<double>, i.e. column-major. Iterating
// InnerIterator(A, i) walks column i, not row i, so that idiom cannot be used
// to find the diagonal of cell i. Locate it with the raw compressed storage:
// scan column i for the inner index equal to i. A must be compressed.
inline double* diagonalPtr(SparseMatrix& A, std::size_t i) {
    const auto outer = A.outerIndexPtr();
    const auto inner = A.innerIndexPtr();
    const auto idx = static_cast<Eigen::Index>(i);
    double* values = A.valuePtr();
    for (auto k = outer[idx]; k < outer[idx + 1]; ++k) {
        if (inner[k] == idx) return values + k;
    }
    return nullptr;
}

} // namespace

template<std::size_t Dim>
SSTModel<Dim>::SSTModel(const Mesh<Dim>& mesh, Config config)
    : mesh_(mesh),
      config_(config),
      k_(mesh, config.kInf),
      omega_(mesh, config.omegaInf),
      nut_(mesh, 0.0),
      wallDist_(mesh, 0.0),
      S_(mesh, 0.0),
      F1_(mesh, 0.0),
      F2_(mesh, 0.0),
      poisson_(mesh),
      convection_(mesh) {
    for (auto& g : gradK_) g = Field<Dim>(mesh, 0.0);
    for (auto& g : gradOmega_) g = Field<Dim>(mesh, 0.0);
    for (auto& g : scalarGradBuffer_) g = Field<Dim>(mesh, 0.0);
    const double* volPtr = mesh_.cellVolumePtr();
    invVol_.resize(mesh_.nCells());
    for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
        double v = volPtr[i];
        invVol_[i] = (v > 1e-30) ? 1.0 / v : 0.0;
    }
    kSolver_.setMaxIterations(static_cast<int>(sstMaxIterations));
    kSolver_.setTolerance(sstTolerance);
    omegaSolver_.setMaxIterations(static_cast<int>(sstMaxIterations));
    omegaSolver_.setTolerance(sstTolerance);
}

template<std::size_t Dim>
void SSTModel<Dim>::initialize(double mu, double rho) {
    mu_ = mu;
    rho_ = rho;
    const std::size_t n = mesh_.nCells();
    const double nu = (rho_ > 1e-30) ? mu_ / rho_ : mu_;
    const double nutCap = config_.nutMaxFactor * nu;
    const double kk = clampPositive(config_.kInf, config_.kMin);
    const double ww = clampPositive(config_.omegaInf, config_.omegaMin);
    for (std::size_t i = 0; i < n; ++i) {
        k_[i] = kk;
        omega_[i] = ww;
        // Seed eddy viscosity from the initial k/omega so the first momentum
        // solve already feels the turbulent viscosity instead of running
        // laminar. The a1 limiter is inactive at S=0, so nut = k/omega; the
        // cap matters when omegaInf is very small (or zero).
        nut_[i] = std::min(kk / ww, nutCap);
    }
    maxYPlus_ = 0.0;
    maxWallDistance_ = 0.0;
}

template<std::size_t Dim>
VelocityBCType SSTModel<Dim>::faceBCType_(
    const std::vector<FaceVelocityBC>& faceBCs, std::size_t f) const {
    if (f < faceBCs.size()) return faceBCs[f].type;
    // No face entry: the momentum discretization falls back to a no-slip wall
    // on every boundary face it cannot classify, so mirror that here.
    return VelocityBCType::NoSlip;
}

template<std::size_t Dim>
void SSTModel<Dim>::computeWallDistance(
    const std::vector<FaceVelocityBC>& faceBCs) {
    const std::size_t n = mesh_.nCells();
    const double far = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i < n; ++i) wallDist_[i] = far;

    // Seed only from wall faces. Seeding from every boundary face (the old
    // behaviour) made outlets and inlets look like walls, so omega received a
    // wall value in the wrong cells and the blending functions were wrong
    // across the whole domain.
    std::size_t wallFaceCount = 0;
    for (std::size_t f = 0; f < mesh_.nFaces(); ++f) {
        if (mesh_.neighbor(f) != std::size_t(-1)) continue;
        if (!isWallType(faceBCType_(faceBCs, f))) continue;
        ++wallFaceCount;
        const std::size_t P = mesh_.owner(f);
        auto center = mesh_.cellCenter(P);
        const double* nrm = mesh_.faceNormalPtr(f);
        double d0 = 0.0;
        for (std::size_t d = 0; d < Dim; ++d) {
            double diff = center[d] - mesh_.faceCenter(f)[d];
            d0 += diff * nrm[d];
        }
        d0 = std::abs(d0);
        wallDist_[P] = std::min(wallDist_[P], d0);
    }

    if (wallFaceCount == 0) {
        // No wall anywhere: keep the distance finite instead of a huge value
        // that would drive every wall term to zero silently.
        for (std::size_t i = 0; i < n; ++i) wallDist_[i] = 0.0;
        maxWallDistance_ = 0.0;
        return;
    }

    std::vector<char> visited(n, false);
    using Entry = std::pair<double, std::size_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pq;

    for (std::size_t i = 0; i < n; ++i) {
        if (wallDist_[i] < far) pq.push({wallDist_[i], i});
    }

    while (!pq.empty()) {
        auto [dist, cell] = pq.top();
        pq.pop();
        if (visited[cell]) continue;
        visited[cell] = true;

        for (std::size_t neighbor : mesh_.cellNeighbors(cell)) {
            if (visited[neighbor]) continue;
            auto cP = mesh_.cellCenter(cell);
            auto cN = mesh_.cellCenter(neighbor);
            double d2 = 0.0;
            for (std::size_t d = 0; d < Dim; ++d) {
                double diff = cN[d] - cP[d];
                d2 += diff * diff;
            }
            double newDist = dist + std::sqrt(d2);
            if (newDist < wallDist_[neighbor]) {
                wallDist_[neighbor] = newDist;
                pq.push({newDist, neighbor});
            }
        }
    }

    maxWallDistance_ = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (wallDist_[i] == far) wallDist_[i] = 0.0;
        maxWallDistance_ = std::max(maxWallDistance_, wallDist_[i]);
    }
}

template<std::size_t Dim>
void SSTModel<Dim>::setBoundaryConditions(
    const std::vector<FaceVelocityBC>& faceBCs) {
    computeWallDistance(faceBCs);

    const std::size_t n = mesh_.nCells();
    const std::size_t nF = mesh_.nFaces();
    kFixedMask_.assign(n, 0);
    kFixedVals_.assign(n, 0.0);
    omegaFixedMask_.assign(n, 0);
    omegaFixedVals_.assign(n, 0.0);
    faceIsWall_.assign(nF, 0);
    faceIsInlet_.assign(nF, 0);
    faceIsOutlet_.assign(nF, 0);
    faceVelocity_.assign(nF, std::array<double, Dim>{});
    outletBackflowK_.assign(nF, 0.0);
    outletBackflowOmega_.assign(nF, 0.0);
    cellIsWall_.assign(n, 0);

    const double nu = (rho_ > 1e-30) ? mu_ / rho_ : mu_;
    const double beta1 = config_.constants.beta1;
    maxYPlus_ = 0.0;

    for (std::size_t f = 0; f < nF; ++f) {
        if (mesh_.neighbor(f) != std::size_t(-1)) continue;

        const VelocityBCType type = faceBCType_(faceBCs, f);
        const std::size_t cell = mesh_.owner(f);

        if (f < faceBCs.size()) {
            faceVelocity_[f][0] = faceBCs[f].u;
            if constexpr (Dim >= 2) faceVelocity_[f][1] = faceBCs[f].v;
            if constexpr (Dim >= 3) faceVelocity_[f][2] = faceBCs[f].w;
        }

        if (isWallType(type)) {
            faceIsWall_[f] = 1;
            cellIsWall_[cell] = 1;
            kFixedMask_[cell] = 1;
            kFixedVals_[cell] = 0.0;
            omegaFixedMask_[cell] = 1;
            const double y1 = std::max(wallDist_[cell], minFaceDist);
            double om = 6.0 * nu / (beta1 * y1 * y1);
            // This cap is part of the BC construction, not an interior clamp.
            // It is configurable and applied once, so the prescribed value is
            // exactly what the solver sees downstream.
            om = std::min(om, config_.omegaMax);
            omegaFixedVals_[cell] = om;
        } else if (isInletType(type)) {
            faceIsInlet_[f] = 1;
            // A corner cell that also touches a wall keeps the wall condition,
            // regardless of which face happens to be visited first.
            if (cellIsWall_[cell]) continue;
            kFixedMask_[cell] = 1;
            kFixedVals_[cell] = config_.kInf;
            omegaFixedMask_[cell] = 1;
            omegaFixedVals_[cell] = config_.omegaInf;
        } else if (type == VelocityBCType::Outlet ||
                   type == VelocityBCType::PressureOutlet) {
            // Zero-gradient while the flow leaves; the backflow values are
            // switched on per iteration when the face actually ingests.
            faceIsOutlet_[f] = 1;
            if (f < faceBCs.size()) {
                outletBackflowK_[f] = faceBCs[f].backflowK;
                outletBackflowOmega_[f] = faceBCs[f].backflowOmega;
            }
        }
        // Symmetry / SlipWall: zero-gradient (natural BC).
    }

    kBCMask_ = kFixedMask_;
    kBCVals_ = kFixedVals_;
    omegaBCMask_ = omegaFixedMask_;
    omegaBCVals_ = omegaFixedVals_;
}

template<std::size_t Dim>
void SSTModel<Dim>::setInletTurbulence(const std::vector<std::size_t>& faces,
                                       double k, double omega) {
    for (std::size_t f : faces) {
        if (f >= mesh_.nFaces()) continue;
        const std::size_t cell = mesh_.owner(f);
        if (cell >= kBCMask_.size()) continue;
        // An explicit inlet patch must not overwrite a wall cell. A cell that
        // owns both a wall face and an inlet face is rare, but silently
        // replacing its omega wall value would be a bad trade.
        if (cell < cellIsWall_.size() && cellIsWall_[cell]) continue;
        if (f < faceIsWall_.size() && faceIsWall_[f]) continue;
        kFixedMask_[cell] = 1;
        kFixedVals_[cell] = k;
        omegaFixedMask_[cell] = 1;
        omegaFixedVals_[cell] = omega;
        kBCMask_[cell] = 1;
        kBCVals_[cell] = k;
        omegaBCMask_[cell] = 1;
        omegaBCVals_[cell] = omega;
    }
}

template<std::size_t Dim>
void SSTModel<Dim>::updateBackflowBCs_(const VectorField<Dim>& velocity) {
    if (faceIsOutlet_.empty()) return;
    kBCMask_ = kFixedMask_;
    kBCVals_ = kFixedVals_;
    omegaBCMask_ = omegaFixedMask_;
    omegaBCVals_ = omegaFixedVals_;

    for (std::size_t f = 0; f < mesh_.nFaces(); ++f) {
        if (mesh_.neighbor(f) != std::size_t(-1)) continue;
        if (!faceIsOutlet_[f]) continue;
        const std::size_t P = mesh_.owner(f);
        const double* fn = mesh_.faceNormalPtr(f);
        double un = 0.0;
        for (std::size_t d = 0; d < Dim; ++d) un += velocity[d][P] * fn[d];
        if (un >= 0.0) continue; // outflow: zero-gradient, no Dirichlet

        // Only add a Dirichlet condition when the case actually specifies
        // backflow turbulence values. The default 0/0 means "not configured";
        // applying it would clamp k and omega to zero at a reversed outlet,
        // which is both unphysical and a strong artificial sink.
        if (!(outletBackflowK_[f] > 0.0) || !(outletBackflowOmega_[f] > 0.0))
            continue;

        if (kFixedMask_[P]) continue; // a wall/inlet condition wins
        kBCMask_[P] = 1;
        kBCVals_[P] = outletBackflowK_[f];
        omegaBCMask_[P] = 1;
        omegaBCVals_[P] = std::clamp(outletBackflowOmega_[f],
                                     config_.omegaMin, config_.omegaMax);
    }
}

template<std::size_t Dim>
void SSTModel<Dim>::clampFields() {
    limitField_(k_, config_.kMin, config_.kMax, kBCMask_, kBCVals_);
    limitField_(omega_, config_.omegaMin, config_.omegaMax,
                omegaBCMask_, omegaBCVals_);
    computeEddyViscosity();
}

template<std::size_t Dim>
void SSTModel<Dim>::limitField_(Field<Dim>& phi, double lo, double hi,
                                const std::vector<char>& mask,
                                const std::vector<double>& vals) const {
    const std::size_t n = mesh_.nCells();
    #pragma omp parallel for schedule(static)
    for (std::size_t i = 0; i < n; ++i) {
        if (i < mask.size() && mask[i]) {
            // Prescribed boundary value: keep it verbatim. A cap must not
            // silently rewrite an inlet or a wall condition.
            if (i < vals.size()) phi[i] = vals[i];
        } else {
            double v = phi[i];
            if (!std::isfinite(v)) v = lo;
            phi[i] = std::clamp(v, lo, hi);
        }
    }
}

template<std::size_t Dim>
void SSTModel<Dim>::solveIteration(const VectorField<Dim>& velocity,
                                   ConvectionScheme scheme) {
    computeStrainRate(velocity);
    updateBackflowBCs_(velocity);
    gradK_ = computeScalarGradients(k_, kBCMask_, kBCVals_);
    gradOmega_ = computeScalarGradients(omega_, omegaBCMask_, omegaBCVals_);
    computeBlendingFunctions();
    computeEddyViscosity();
    solveKTransport(velocity, scheme);
    solveOmegaTransport(velocity, scheme);
    computeEddyViscosity();
    computeWallDiagnostics(velocity);
    if (config_.wallFunction) applyWallFunctions(velocity);
}

template<std::size_t Dim>
const Field<Dim>& SSTModel<Dim>::k() const { return k_; }

template<std::size_t Dim>
const Field<Dim>& SSTModel<Dim>::omega() const { return omega_; }

template<std::size_t Dim>
const Field<Dim>& SSTModel<Dim>::nut() const { return nut_; }

template<std::size_t Dim>
Field<Dim>& SSTModel<Dim>::nut() { return nut_; }

template<std::size_t Dim>
const Field<Dim>& SSTModel<Dim>::wallDistance() const { return wallDist_; }

template<std::size_t Dim>
Field<Dim>& SSTModel<Dim>::k() { return k_; }

template<std::size_t Dim>
Field<Dim>& SSTModel<Dim>::omega() { return omega_; }

// --- Private method implementations ---

template<std::size_t Dim>
std::array<Field<Dim>, Dim> SSTModel<Dim>::computeScalarGradients(
    const Field<Dim>& phi,
    const std::vector<char>& mask,
    const std::vector<double>& vals) const {
    for (auto& g : scalarGradBuffer_) g.assign(0.0);

    const std::size_t n = mesh_.nCells();
    const std::size_t nF = mesh_.nFaces();
    // invVol_ is cached (mesh volumes are immutable for this model).
    const std::size_t* ownerPtr = mesh_.ownerPtr();
    const std::size_t* neighborPtr = mesh_.neighborPtr();
    const double* areaPtr = mesh_.faceAreaPtr();
    const double* weightPtr = mesh_.faceWeightPtr();
    const bool hasMask = !mask.empty();

    auto cellValue = [&](std::size_t c) -> double {
        if (hasMask && c < mask.size() && mask[c] && c < vals.size())
            return vals[c];
        return phi[c];
    };

    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = ownerPtr[f];
        const std::size_t N = neighborPtr[f];
        double phiFace;
        if (N == std::size_t(-1)) {
            // Boundary face: use the prescribed value where there is one
            // instead of the owner cell value (the old behaviour zeroed the
            // boundary contribution of a Dirichlet wall entirely).
            phiFace = cellValue(P);
        } else {
            // Exact linear interpolation between the cell centres, limited to
            // the adjacent cell values to avoid overshoots on stretched meshes.
            const double w = weightPtr[f];
            double phiLin = (1.0 - w) * cellValue(P) + w * cellValue(N);
            double lo = std::min(cellValue(P), cellValue(N));
            double hi = std::max(cellValue(P), cellValue(N));
            phiFace = std::clamp(phiLin, lo, hi);
        }
        const double* fn = mesh_.faceNormalPtr(f);
        double area = areaPtr[f];
        double phiArea = phiFace * area;
        for (std::size_t d = 0; d < Dim; ++d) {
            double contrib = phiArea * fn[d];
            scalarGradBuffer_[d][P] += contrib * invVol_[P];
            if (N != std::size_t(-1)) scalarGradBuffer_[d][N] -= contrib * invVol_[N];
        }
    }
    return scalarGradBuffer_;
}

template<std::size_t Dim>
void SSTModel<Dim>::computeStrainRate(const VectorField<Dim>& velocity) {
    const std::size_t n = mesh_.nCells();
    const std::size_t nF = mesh_.nFaces();

    S_.assign(0.0);

    // Reuse pre-allocated buffer to avoid O(Dim^2 * n) allocation per iteration
    auto& dUdx = dUdxBuffer_;
    if (!dUdxInitialized_) {
        for (std::size_t i = 0; i < Dim; ++i)
            for (std::size_t j = 0; j < Dim; ++j)
                dUdx[i][j] = Field<Dim>(mesh_, 0.0);
        dUdxInitialized_ = true;
    } else {
        for (std::size_t i = 0; i < Dim; ++i)
            for (std::size_t j = 0; j < Dim; ++j)
                dUdx[i][j].assign(0.0);
    }

    // invVol_ is cached (mesh volumes are immutable for this model).
    const std::size_t* ownerPtr = mesh_.ownerPtr();
    const std::size_t* neighborPtr = mesh_.neighborPtr();
    const double* areaPtr = mesh_.faceAreaPtr();
    const double* weightPtr = mesh_.faceWeightPtr();

    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = ownerPtr[f];
        const std::size_t N = neighborPtr[f];
        std::array<double, Dim> uFace{};
        if (N == std::size_t(-1)) {
            const bool prescribed = f < faceIsWall_.size() &&
                                    (faceIsWall_[f] || faceIsInlet_[f]);
            for (std::size_t dd = 0; dd < Dim; ++dd) {
                uFace[dd] = prescribed ? faceVelocity_[f][dd] : velocity[dd][P];
            }
        } else {
            const double w = weightPtr[f];
            for (std::size_t dd = 0; dd < Dim; ++dd) {
                uFace[dd] = (1.0 - w) * velocity[dd][P] + w * velocity[dd][N];
            }
        }
        const double* fn = mesh_.faceNormalPtr(f);
        double area = areaPtr[f];
        double invVolP = invVol_[P];
        double invVolN = (N == std::size_t(-1)) ? 0.0 : invVol_[N];
        for (std::size_t i = 0; i < Dim; ++i) {
            double uAi = uFace[i] * area;
            for (std::size_t j = 0; j < Dim; ++j) {
                double contrib = uAi * fn[j];
                dUdx[i][j][P] += contrib * invVolP;
                if (N != std::size_t(-1)) dUdx[i][j][N] -= contrib * invVolN;
            }
        }
    }

    #pragma omp parallel for schedule(static)
    for (std::size_t idx = 0; idx < n; ++idx) {
        double S2 = 0.0;
        for (std::size_t i = 0; i < Dim; ++i) {
            for (std::size_t j = 0; j < Dim; ++j) {
                double Sij = 0.5 * (dUdx[i][j][idx] + dUdx[j][i][idx]);
                S2 += Sij * Sij;
            }
        }
        S_[idx] = std::sqrt(2.0 * S2);
    }
}

template<std::size_t Dim>
void SSTModel<Dim>::applyWallFunctions(const VectorField<Dim>& velocity) {
    const double nu = (rho_ > 1e-30) ? mu_ / rho_ : mu_;
    const double kappa = config_.constants.kappa;
    const double B = 5.0;
    const double Cmu = 0.09;
    const std::size_t n = mesh_.nCells();

    for (std::size_t f = 0; f < mesh_.nFaces(); ++f) {
        if (mesh_.neighbor(f) != std::size_t(-1)) continue;
        if (f >= faceIsWall_.size() || !faceIsWall_[f]) continue;
        const std::size_t P = mesh_.owner(f);
        const double y = std::max(wallDist_[P], minFaceDist);
        if (y < 1e-12) continue;

        double uMag = 0.0;
        const double* wallVel = faceVelocity_[f].data();
        for (std::size_t d = 0; d < Dim; ++d) {
            double diff = velocity[d][P] - wallVel[d];
            uMag += diff * diff;
        }
        uMag = std::sqrt(uMag);
        if (uMag < 1e-12) continue;

        // Iterative solution of u+ = (1/kappa) ln(y+) + B
        double uTau = std::max(uMag / 20.0, 1e-8);
        for (int it = 0; it < 20; ++it) {
            double yp = y * uTau / std::max(nu, 1e-30);
            double up = (yp < 11.225) ? yp : (1.0 / kappa) * std::log(std::max(yp, 1e-12)) + B;
            if (up > 1e-12) uTau = uMag / up;
        }
        double yp = y * uTau / std::max(nu, 1e-30);

        double kVal, omegaVal, nutVal;
        if (yp < 11.225) {
            // Viscous sublayer
            kVal = 0.0; // k=0 at wall
            omegaVal = 6.0 * nu / (config_.constants.beta1 * y * y);
            nutVal = 0.0;
        } else {
            kVal = uTau * uTau / std::sqrt(Cmu);
            omegaVal = uTau / (std::sqrt(config_.constants.beta_star) * kappa * y);
            nutVal = std::max(uTau * kappa * y - nu, 0.0);
        }
        k_[P] = std::clamp(kVal, config_.kMin, config_.kMax);
        omega_[P] = std::clamp(omegaVal, config_.omegaMin, config_.omegaMax);
        nut_[P] = std::max(nutVal, 0.0);
    }
}

template<std::size_t Dim>
void SSTModel<Dim>::computeWallDiagnostics(const VectorField<Dim>& velocity) {
    maxYPlus_ = 0.0;
    const double nu = (rho_ > 1e-30) ? mu_ / rho_ : mu_;
    if (!(nu > 0.0)) return;

    for (std::size_t f = 0; f < mesh_.nFaces(); ++f) {
        if (mesh_.neighbor(f) != std::size_t(-1)) continue;
        if (f >= faceIsWall_.size() || !faceIsWall_[f]) continue;
        const std::size_t P = mesh_.owner(f);
        const double y = std::max(wallDist_[P], minFaceDist);

        // Wall-adjacent shear estimate from the viscous sublayer. With no
        // wall function this is the best available diagnostic, and it is
        // exactly the quantity that tells the caller whether the first cell
        // is inside the sublayer (y+ ~ 1) or not.
        double du2 = 0.0;
        for (std::size_t d = 0; d < Dim; ++d) {
            const double wallVel = faceVelocity_[f][d];
            const double diff = velocity[d][P] - wallVel;
            du2 += diff * diff;
        }
        const double magU = std::sqrt(du2);
        const double tau = mu_ * magU / y;
        const double uTau = std::sqrt(std::max(tau / rho_, 0.0));
        const double yPlus = y * uTau / nu;
        if (std::isfinite(yPlus)) maxYPlus_ = std::max(maxYPlus_, yPlus);
    }
    // Auto-switch to wall functions when the mesh is too coarse for low-Re.
    if (!config_.wallFunction && maxYPlus_ > 30.0) {
        config_.wallFunction = true;
    }
}

template<std::size_t Dim>
void SSTModel<Dim>::computeBlendingFunctions() {
    const auto& c = config_.constants;
    const double eps = 1e-30;
    const double nu = (rho_ > 1e-30) ? mu_ / rho_ : mu_;

    #pragma omp parallel for schedule(static)
    for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
        double y  = wallDist_[i];
        double k  = std::max(k_[i], config_.kMin);
        double w  = std::max(omega_[i], config_.omegaMin);
        double y2 = y * y;

        double arg1a = std::sqrt(k) / (c.beta_star * w * y + eps);
        double arg1b = wallDistFactor * nu / (w * y2 + eps);

        double dotProd = 0.0;
        for (std::size_t d = 0; d < Dim; ++d)
            dotProd += gradK_[d][i] * gradOmega_[d][i];
        double CDomega = std::max(
            2.0 * c.sigma_w2 * dotProd / (w + eps), 1e-10);

        // Density-free: the transport equations are kinematic and CDomega is
        // kinematic too. The old 4*rho*sigma_w2*k/(CDomega*y^2) made F1 depend
        // on density, so for water (rho=1000) F1 was pushed towards the
        // k-epsilon branch across the entire boundary layer.
        double arg1c = 4.0 * c.sigma_w2 * k / (CDomega * y2 + eps);

        double arg1 = std::min(std::max(arg1a, arg1b), arg1c);
        F1_[i] = std::tanh(arg1 * arg1 * arg1 * arg1);

        double arg2a = 2.0 * std::sqrt(k) / (c.beta_star * w * y + eps);
        double arg2b = wallDistFactor * nu / (w * y2 + eps);
        double arg2 = std::max(arg2a, arg2b);
        F2_[i] = std::tanh(arg2 * arg2);
    }
}

template<std::size_t Dim>
void SSTModel<Dim>::computeEddyViscosity() {
    const auto& c = config_.constants;
    const double nu = (rho_ > 1e-30) ? mu_ / rho_ : mu_;
    const double nutCap = config_.nutMaxFactor * nu;
    #pragma omp parallel for schedule(static)
    for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
        double k = std::max(k_[i], config_.kMin);
        double w = std::max(omega_[i], config_.omegaMin);
        double denom = std::max(c.a1 * w, S_[i] * F2_[i]);
        nut_[i] = c.a1 * k / std::max(denom, 1e-20);
        if (nut_[i] > nutCap) nut_[i] = nutCap;
        if (nut_[i] < 0.0) nut_[i] = 0.0;
    }
}

template<std::size_t Dim>
void SSTModel<Dim>::computeKDiffusionSource_(Field<Dim>& diff,
                                             Field<Dim>& src) const {
    const std::size_t n = mesh_.nCells();
    const auto& c = config_.constants;
    for (std::size_t i = 0; i < n; ++i) {
        const double sigmaK = F1_[i] * c.sigma_k1 + (1.0 - F1_[i]) * c.sigma_k2;
        diff[i] = (mu_ / rho_) + sigmaK * std::max(nut_[i], 0.0);

        const double k = std::max(k_[i], config_.kMin);
        const double w = std::max(omega_[i], config_.omegaMin);
        const double Pk = std::max(nut_[i], 0.0) * S_[i] * S_[i];
        src[i] = std::min(Pk, sstProductionLimitFactor * c.beta_star * w * k);
    }
}

template<std::size_t Dim>
void SSTModel<Dim>::computeOmegaDiffusionSource_(Field<Dim>& diff,
                                                 Field<Dim>& src) const {
    const std::size_t n = mesh_.nCells();
    const auto& c = config_.constants;
    for (std::size_t i = 0; i < n; ++i) {
        const double sigmaW = F1_[i] * c.sigma_w1 + (1.0 - F1_[i]) * c.sigma_w2;
        diff[i] = (mu_ / rho_) + sigmaW * std::max(nut_[i], 0.0);

        const double w = std::max(omega_[i], config_.omegaMin);
        const double k = std::max(k_[i], config_.kMin);
        const double alpha = F1_[i] * c.alpha1 + (1.0 - F1_[i]) * c.alpha2;

        // Production with the same limiter used by the k equation, written in
        // the bounded form alpha * min(S^2, lim/(nut)) so it stays finite as
        // nut -> 0 and never needs the branch-dependent 1e3 clamp the old
        // steady path used.
        const double prodLimit = sstProductionLimitFactor * c.beta_star * w * k;
        const double nutSafe = std::max(nut_[i], 1e-20);
        const double S2 = S_[i] * S_[i];
        const double prodTerm = alpha * std::min(S2, prodLimit / nutSafe);

        double dotProd = 0.0;
        for (std::size_t d = 0; d < Dim; ++d)
            dotProd += gradK_[d][i] * gradOmega_[d][i];
        double crossDiff = 2.0 * (1.0 - F1_[i]) * c.sigma_w2 * dotProd / w;
        crossDiff = std::clamp(crossDiff, -crossDiffusionLimit, crossDiffusionLimit);

        src[i] = prodTerm + crossDiff;
    }
}

template<std::size_t Dim>
void SSTModel<Dim>::applyUnderRelaxation_(SparseMatrix& A, Eigen::VectorXd& b,
                                          const Field<Dim>& phi, double alpha,
                                          const std::vector<char>& mask) const {
    const double a = std::clamp(alpha, 1e-3, 1.0);
    const double relax = (1.0 - a) / a;
    if (relax <= 1e-14) return;

    A.makeCompressed();
    const std::size_t n = mesh_.nCells();
    for (std::size_t i = 0; i < n; ++i) {
        if (i < mask.size() && mask[i]) continue;
        double* diag = diagonalPtr(A, i);
        if (diag == nullptr) continue;
        const double aP = *diag;
        if (aP > 1e-15) {
            b(static_cast<Eigen::Index>(i)) += relax * aP * phi[i];
            *diag = aP / a;
        }
    }
}

template<std::size_t Dim>
void SSTModel<Dim>::solveKTransport(const VectorField<Dim>& velocity,
                                    ConvectionScheme scheme) {
    solveKTransportImpl(velocity, scheme, 0.0, 0.0, TimeScheme::Euler,
                        nullptr, nullptr);
}

template<std::size_t Dim>
void SSTModel<Dim>::solveOmegaTransport(const VectorField<Dim>& velocity,
                                        ConvectionScheme scheme) {
    solveOmegaTransportImpl(velocity, scheme, 0.0, 0.0, TimeScheme::Euler,
                            nullptr, nullptr);
}

template<std::size_t Dim>
void SSTModel<Dim>::solveIterationTransient(const VectorField<Dim>& velocity,
                                            ConvectionScheme scheme,
                                            double dt,
                                            double dtPrev,
                                            TimeScheme timeScheme,
                                            const Field<Dim>& kOld,
                                            const Field<Dim>& omegaOld,
                                            const Field<Dim>* kOldPrev,
                                            const Field<Dim>* omegaOldPrev) {
    computeStrainRate(velocity);
    updateBackflowBCs_(velocity);
    gradK_ = computeScalarGradients(k_, kBCMask_, kBCVals_);
    gradOmega_ = computeScalarGradients(omega_, omegaBCMask_, omegaBCVals_);
    computeBlendingFunctions();
    computeEddyViscosity();
    solveKTransportTransient(velocity, scheme, dt, dtPrev, timeScheme, kOld, kOldPrev);
    solveOmegaTransportTransient(velocity, scheme, dt, dtPrev, timeScheme, omegaOld, omegaOldPrev);
    computeEddyViscosity();
    computeWallDiagnostics(velocity);
    if (config_.wallFunction) applyWallFunctions(velocity);
}

template<std::size_t Dim>
void SSTModel<Dim>::solveKTransportImpl(const VectorField<Dim>& velocity,
                                        ConvectionScheme scheme,
                                        double dt,
                                        double dtPrev,
                                        TimeScheme timeScheme,
                                        const Field<Dim>* kOld,
                                        const Field<Dim>* kOldPrev) {
    const std::size_t n = mesh_.nCells();
    const auto& c = config_.constants;

    Field<Dim> diffCoeff(mesh_, 0.0);
    Field<Dim> sourceK(mesh_, 0.0);
    computeKDiffusionSource_(diffCoeff, sourceK);

    SparseMatrix A;
    Eigen::VectorXd b;
    poisson_.discretize(A, b, diffCoeff, sourceK, kBCMask_, kBCVals_, &gradK_);

    std::array<const Field<Dim>*, Dim> velPtrs{};
    for (std::size_t d = 0; d < Dim; ++d) velPtrs[d] = &velocity[d];
    convection_.setScheme(scheme);
    convection_.discretize(A, b, k_, velPtrs,
                           ConvectionBCs{kBCMask_, kBCVals_, {}, {}}, &gradK_);

    A.makeCompressed();
    for (std::size_t i = 0; i < n; ++i) {
        if (kBCMask_[i]) continue;
        double destroyCoeff = c.beta_star
                            * std::max(omega_[i], config_.omegaMin)
                            * mesh_.cellVolume(i);
        if (double* diag = diagonalPtr(A, i)) *diag += destroyCoeff;
    }

    // Transient term. A non-positive dt means the caller did not provide a
    // usable step, which is an error: silently solving the steady equation
    // (the old fallback) hides a broken time configuration.
    ImplicitTimeCoeffs tc{};
    const bool useTime = (kOld != nullptr && dt > 0.0 && dt < 1e14);
    if (useTime) {
        if (timeScheme == TimeScheme::BDF2 && kOldPrev != nullptr) {
            tc = implicitTimeCoeffs(dt, dtPrev);
        } else {
            // Explicit Euler (or BDF2 without previous step): (phi^{n+1} - phi^n)/dt
            tc.c0 = 1.0 / dt;
            tc.cOld = 1.0 / dt;
            tc.secondOrder = false;
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (kBCMask_[i]) continue;
            double vol = mesh_.cellVolume(i);
            const double coeff = tc.c0 * vol;
            const double rhs = tc.secondOrder
                ? vol * (tc.cOld * (*kOld)[i] + tc.cOldPrev * (*kOldPrev)[i])
                : coeff * (*kOld)[i];
            if (double* diag = diagonalPtr(A, i)) *diag += coeff;
            b(static_cast<Eigen::Index>(i)) += rhs;
        }
    }

    // Same under-relaxation as the steady path; the transient path used to
    // blend the solution after the solve, which let boundary cells approach
    // their prescribed value asymptotically instead of enforcing it.
    applyUnderRelaxation_(A, b, k_, config_.alphaK, kBCMask_);

    Eigen::VectorXd x = solveKSystem_(A, b);

    const double bnorm = b.lpNorm<2>();
    kResidual_ = (bnorm > 1e-30)
        ? (b - A * x).lpNorm<2>() / bnorm
        : (b - A * x).lpNorm<2>();

    for (std::size_t i = 0; i < n; ++i) k_[i] = x(static_cast<Eigen::Index>(i));
    limitField_(k_, config_.kMin, config_.kMax, kBCMask_, kBCVals_);
}

template<std::size_t Dim>
void SSTModel<Dim>::solveKTransportTransient(const VectorField<Dim>& velocity,
                                            ConvectionScheme scheme,
                                            double dt,
                                            double dtPrev,
                                            TimeScheme timeScheme,
                                            const Field<Dim>& kOld,
                                            const Field<Dim>* kOldPrev) {
    solveKTransportImpl(velocity, scheme, dt, dtPrev, timeScheme, &kOld, kOldPrev);
}

template<std::size_t Dim>
void SSTModel<Dim>::solveOmegaTransportImpl(const VectorField<Dim>& velocity,
                                            ConvectionScheme scheme,
                                            double dt,
                                            double dtPrev,
                                            TimeScheme timeScheme,
                                            const Field<Dim>* omegaOld,
                                            const Field<Dim>* omegaOldPrev) {
    const std::size_t n = mesh_.nCells();
    const auto& c = config_.constants;

    Field<Dim> diffCoeff(mesh_, 0.0);
    Field<Dim> sourceOmega(mesh_, 0.0);
    computeOmegaDiffusionSource_(diffCoeff, sourceOmega);

    SparseMatrix A;
    Eigen::VectorXd b;
    poisson_.discretize(A, b, diffCoeff, sourceOmega,
                        omegaBCMask_, omegaBCVals_, &gradOmega_);

    std::array<const Field<Dim>*, Dim> velPtrs{};
    for (std::size_t d = 0; d < Dim; ++d) velPtrs[d] = &velocity[d];
    convection_.setScheme(scheme);
    convection_.discretize(A, b, omega_, velPtrs,
                           ConvectionBCs{omegaBCMask_, omegaBCVals_, {}, {}}, &gradOmega_);

    A.makeCompressed();
    for (std::size_t i = 0; i < n; ++i) {
        if (omegaBCMask_[i]) continue;
        double beta = F1_[i] * c.beta1 + (1.0 - F1_[i]) * c.beta2;
        double w = std::max(omega_[i], config_.omegaMin);
        double diagInc = beta * w * mesh_.cellVolume(i);
        if (double* diag = diagonalPtr(A, i)) *diag += diagInc;
    }

    ImplicitTimeCoeffs tc{};
    const bool useTime = (omegaOld != nullptr && dt > 0.0 && dt < 1e14);
    if (useTime) {
        if (timeScheme == TimeScheme::BDF2 && omegaOldPrev != nullptr) {
            tc = implicitTimeCoeffs(dt, dtPrev);
        } else {
            // Explicit Euler (or BDF2 without previous step): (phi^{n+1} - phi^n)/dt
            tc.c0 = 1.0 / dt;
            tc.cOld = 1.0 / dt;
            tc.secondOrder = false;
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (omegaBCMask_[i]) continue;
            double vol = mesh_.cellVolume(i);
            const double coeff = tc.c0 * vol;
            const double rhs = tc.secondOrder
                ? vol * (tc.cOld * (*omegaOld)[i] + tc.cOldPrev * (*omegaOldPrev)[i])
                : coeff * (*omegaOld)[i];
            if (double* diag = diagonalPtr(A, i)) *diag += coeff;
            b(static_cast<Eigen::Index>(i)) += rhs;
        }
    }

    applyUnderRelaxation_(A, b, omega_, config_.alphaOmega, omegaBCMask_);

    Eigen::VectorXd x = solveOmegaSystem_(A, b);

    const double bnorm = b.lpNorm<2>();
    omegaResidual_ = (bnorm > 1e-30)
        ? (b - A * x).lpNorm<2>() / bnorm
        : (b - A * x).lpNorm<2>();

    for (std::size_t i = 0; i < n; ++i) omega_[i] = x(static_cast<Eigen::Index>(i));
    limitField_(omega_, config_.omegaMin, config_.omegaMax,
                omegaBCMask_, omegaBCVals_);
}

template<std::size_t Dim>
void SSTModel<Dim>::solveOmegaTransportTransient(const VectorField<Dim>& velocity,
                                                ConvectionScheme scheme,
                                                double dt,
                                                double dtPrev,
                                                TimeScheme timeScheme,
                                                const Field<Dim>& omegaOld,
                                                const Field<Dim>* omegaOldPrev) {
    solveOmegaTransportImpl(velocity, scheme, dt, dtPrev, timeScheme, &omegaOld,
                            omegaOldPrev);
}

template<std::size_t Dim>
Eigen::VectorXd SSTModel<Dim>::solveKSystem_(const SparseMatrix& A,
                                             const Eigen::VectorXd& b) {
    kSolver_.compute(A);
    Eigen::VectorXd x = kSolver_.solve(b);
    if (kSolver_.info() != Eigen::Success) {
        // Fallback: solve without the ILU preconditioner.
        Eigen::BiCGSTAB<SparseMatrix> fallback;
        fallback.setMaxIterations(static_cast<int>(sstMaxIterations));
        fallback.setTolerance(sstTolerance);
        fallback.compute(A);
        x = fallback.solve(b);
        if (fallback.info() != Eigen::Success) {
            std::fprintf(stderr,
                "  WARNING: SST k solve did not converge (iters=%d, resid=%g)\n",
                fallback.iterations(), fallback.error());
        }
    }
    sanitizeSolution_(x);
    return x;
}

template<std::size_t Dim>
Eigen::VectorXd SSTModel<Dim>::solveOmegaSystem_(const SparseMatrix& A,
                                                 const Eigen::VectorXd& b) {
    omegaSolver_.compute(A);
    Eigen::VectorXd x = omegaSolver_.solve(b);
    if (omegaSolver_.info() != Eigen::Success) {
        Eigen::BiCGSTAB<SparseMatrix> fallback;
        fallback.setMaxIterations(static_cast<int>(sstMaxIterations));
        fallback.setTolerance(sstTolerance);
        fallback.compute(A);
        x = fallback.solve(b);
        if (fallback.info() != Eigen::Success) {
            std::fprintf(stderr,
                "  WARNING: SST omega solve did not converge (iters=%d, resid=%g)\n",
                fallback.iterations(), fallback.error());
        }
    }
    sanitizeSolution_(x);
    return x;
}

template<std::size_t Dim>
void SSTModel<Dim>::sanitizeSolution_(Eigen::VectorXd& x) const {
    std::size_t bad = 0;
    for (Eigen::Index i = 0; i < x.size(); ++i) {
        if (!std::isfinite(x(i))) {
            x(i) = 0.0;
            ++bad;
        }
    }
    if (bad > 0) {
        std::fprintf(stderr,
            "  WARNING: SST solve produced %zu non-finite value(s); replaced with the floor\n",
            bad);
    }
}

template class SSTModel<2>;
template class SSTModel<3>;

} // namespace fvm
