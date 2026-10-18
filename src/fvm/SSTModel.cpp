#include "fvm/SSTModel.h"
#include <cstdio>
#include <Eigen/IterativeLinearSolvers>
#include <queue>

namespace fvm {

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
      convection_(mesh),
      gradient_(mesh) {
    for (auto& g : gradK_) g = Field<Dim>(mesh, 0.0);
    for (auto& g : gradOmega_) g = Field<Dim>(mesh, 0.0);
    for (auto& g : scalarGradBuffer_) g = Field<Dim>(mesh, 0.0);
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
    for (std::size_t i = 0; i < n; ++i) {
        k_[i] = config_.kInf;
        omega_[i] = config_.omegaInf;
        // Seed eddy viscosity from the initial k/omega so the first momentum
        // solve already feels the turbulent viscosity instead of running laminar.
        nut_[i] = config_.kInf / std::max(config_.omegaInf, config_.omegaMin);
    }
}

template<std::size_t Dim>
void SSTModel<Dim>::computeWallDistance() {
    const std::size_t n = mesh_.nCells();
    for (std::size_t i = 0; i < n; ++i)
        wallDist_[i] = std::numeric_limits<double>::max();

    for (std::size_t f = 0; f < mesh_.nFaces(); ++f) {
        if (mesh_.neighbor(f) != std::size_t(-1)) continue;
        const std::size_t P = mesh_.owner(f);
        auto center = mesh_.cellCenter(P);
        double dist2 = 0.0;
        for (std::size_t d = 0; d < Dim; ++d) {
            double diff = mesh_.faceCenter(f)[d] - center[d];
            dist2 += diff * diff;
        }
        wallDist_[P] = std::min(wallDist_[P], std::sqrt(dist2));
    }

    std::vector<bool> visited(n, false);
    using Entry = std::pair<double, std::size_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pq;

    for (std::size_t i = 0; i < n; ++i) {
        if (wallDist_[i] < std::numeric_limits<double>::max()) {
            pq.push({wallDist_[i], i});
        }
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

    for (std::size_t i = 0; i < n; ++i) {
        if (wallDist_[i] == std::numeric_limits<double>::max())
            wallDist_[i] = 0.0;
    }
}

template<std::size_t Dim>
void SSTModel<Dim>::solveIteration(const VectorField<Dim>& velocity,
                                   ConvectionScheme scheme) {
     computeStrainRate(velocity);
     gradK_ = computeScalarGradients(k_);
     gradOmega_ = computeScalarGradients(omega_);
     computeBlendingFunctions();
     computeEddyViscosity();
    solveKTransport(velocity, scheme);
    solveOmegaTransport(velocity, scheme);
    computeEddyViscosity();
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
std::array<Field<Dim>, Dim> SSTModel<Dim>::computeScalarGradients(const Field<Dim>& phi) const {
    for (auto& g : scalarGradBuffer_) g.assign(0.0);

    const std::size_t n = mesh_.nCells();
    const std::size_t nF = mesh_.nFaces();
    // Precompute 1/vol to replace division in inner loop: O(n) once vs O(nF*Dim) divisions
    std::vector<double> invVol(n);
    const double* volPtr = mesh_.cellVolumePtr();
    for (std::size_t i = 0; i < n; ++i) {
        double v = volPtr[i];
        invVol[i] = (v > 1e-30) ? 1.0 / v : 0.0;
    }
    const std::size_t* ownerPtr = mesh_.ownerPtr();
    const std::size_t* neighborPtr = mesh_.neighborPtr();
    const double* areaPtr = mesh_.faceAreaPtr();

    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = ownerPtr[f];
        const std::size_t N = neighborPtr[f];
        double phiFace = (N == std::size_t(-1)) ? phi[P] : 0.5 * (phi[P] + phi[N]);
        const double* fn = mesh_.faceNormalPtr(f);
        double area = areaPtr[f];
        double phiArea = phiFace * area;
        for (std::size_t d = 0; d < Dim; ++d) {
            double contrib = phiArea * fn[d];
            scalarGradBuffer_[d][P] += contrib * invVol[P];
            if (N != std::size_t(-1)) scalarGradBuffer_[d][N] -= contrib * invVol[N];
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

    // Precompute 1/vol for inner loop
    std::vector<double> invVol(n);
    const double* volPtr = mesh_.cellVolumePtr();
    for (std::size_t i = 0; i < n; ++i) {
        double v = volPtr[i];
        invVol[i] = (v > 1e-30) ? 1.0 / v : 0.0;
    }
    const std::size_t* ownerPtr = mesh_.ownerPtr();
    const std::size_t* neighborPtr = mesh_.neighborPtr();
    const double* areaPtr = mesh_.faceAreaPtr();

    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = ownerPtr[f];
        const std::size_t N = neighborPtr[f];
        std::array<double, Dim> uFace{};
        for (std::size_t dd = 0; dd < Dim; ++dd) {
            uFace[dd] = (N == std::size_t(-1)) ? velocity[dd][P] : 0.5 * (velocity[dd][P] + velocity[dd][N]);
        }
        const double* fn = mesh_.faceNormalPtr(f);
        double area = areaPtr[f];
        double invVolP = invVol[P];
        double invVolN = (N == std::size_t(-1)) ? 0.0 : invVol[N];
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
void SSTModel<Dim>::computeBlendingFunctions() {
    const auto& c = config_.constants;
    const double eps = 1e-30;

    #pragma omp parallel for schedule(static)
    for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
        double y  = wallDist_[i];
        double k  = std::max(k_[i], config_.kMin);
        double w  = std::max(omega_[i], config_.omegaMin);
        double y2 = y * y;

        double arg1a = std::sqrt(k) / (c.beta_star * w * y + eps);
        double arg1b = wallDistFactor * mu_ / (rho_ * w * y2 + eps);

        double dotProd = 0.0;
        for (std::size_t d = 0; d < Dim; ++d)
            dotProd += gradK_[d][i] * gradOmega_[d][i];
        double CDomega = std::max(
            2.0 * c.sigma_w2 * dotProd / (w + eps), 1e-10);

        double arg1c = 4.0 * rho_ * c.sigma_w2 * k
                     / (CDomega * y2 + eps);

        double arg1 = std::min(std::max(arg1a, arg1b), arg1c);
        F1_[i] = std::tanh(arg1 * arg1 * arg1 * arg1);

        double arg2a = 2.0 * std::sqrt(k) / (c.beta_star * w * y + eps);
        double arg2b = wallDistFactor * mu_ / (rho_ * w * y2 + eps);
        double arg2 = std::max(arg2a, arg2b);
        F2_[i] = std::tanh(arg2 * arg2);
    }
}

template<std::size_t Dim>
void SSTModel<Dim>::computeEddyViscosity() {
    const auto& c = config_.constants;
    #pragma omp parallel for schedule(static)
    for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
        double k = std::max(k_[i], config_.kMin);
        double w = std::max(omega_[i], config_.omegaMin);
        double denom = std::max(c.a1 * w, S_[i] * F2_[i]);
        nut_[i] = c.a1 * k / std::max(denom, 1e-20);
        if (nut_[i] > 1e5 * (mu_ / rho_)) nut_[i] = 1e5 * (mu_ / rho_);
        if (nut_[i] < 0.0) nut_[i] = 0.0;
    }
}

template<std::size_t Dim>
void SSTModel<Dim>::solveKTransport(const VectorField<Dim>& velocity,
                                    ConvectionScheme scheme) {
    const std::size_t n = mesh_.nCells();
    const auto& c = config_.constants;

    Field<Dim> diffCoeff(mesh_, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        double sigmaK = F1_[i] * c.sigma_k1 + (1.0 - F1_[i]) * c.sigma_k2;
        diffCoeff[i] = (mu_ / rho_) + sigmaK * nut_[i];
    }

    Field<Dim> sourceK(mesh_, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        double Pk = nut_[i] * S_[i] * S_[i];
        double PkLim = std::min(Pk, sstProductionLimitFactor * c.beta_star
                                    * std::max(omega_[i], config_.omegaMin)
                                    * std::max(k_[i], config_.kMin));
        sourceK[i] = PkLim;
    }

    SparseMatrix A;
    Eigen::VectorXd b;
    poisson_.discretize(A, b, diffCoeff, sourceK, kBCMask_, kBCVals_);

    std::array<const Field<Dim>*, Dim> velPtrs{};
    for (std::size_t d = 0; d < Dim; ++d)
        velPtrs[d] = &velocity[d];
    convection_.setScheme(scheme);

    std::array<Field<Dim>, Dim> gradK;
    for (auto& g : gradK) g = Field<Dim>(mesh_, 0.0);

    convection_.discretize(A, b, k_, velPtrs, kBCMask_, kBCVals_,
                           nullptr);

    for (std::size_t i = 0; i < n; ++i) {
        if (kBCMask_[i]) continue;
        double destroyCoeff = c.beta_star
                            * std::max(omega_[i], config_.omegaMin)
                            * mesh_.cellVolume(i);
        for (SparseMatrix::InnerIterator it(A, static_cast<Eigen::Index>(i)); it; ++it) {
            if (it.col() == static_cast<Eigen::Index>(i)) {
                it.valueRef() += destroyCoeff;
                break;
            }
        }
    }

    // Under-relaxation of k equation
    {
        double alpha = config_.alphaK;
        double relax = (1.0 - alpha) / alpha;
        if (relax > 1e-14) {
            A.makeCompressed();
            for (std::size_t i = 0; i < n; ++i) {
                if (kBCMask_[i]) continue;
                for (SparseMatrix::InnerIterator it(A, static_cast<Eigen::Index>(i)); it; ++it) {
                    if (it.col() == static_cast<Eigen::Index>(i)) {
                        double aP = it.value();
                        if (aP > 1e-15) {
                            b(static_cast<Eigen::Index>(i)) += relax * aP * k_[i];
                            it.valueRef() = aP / alpha;
                        }
                        break;
                    }
                }
            }
        }
    }

    Eigen::VectorXd x = solveKSystem_(A, b);

    // Compute K residual
    Eigen::VectorXd rK = b - A * x;
    kResidual_ = rK.lpNorm<2>();

    #pragma omp parallel for schedule(static)
    for (std::size_t i = 0; i < n; ++i) {
        double newVal = std::max(x(static_cast<Eigen::Index>(i)),
                                 config_.kMin);
        if (newVal > 1e-2) newVal = 1e-2;
        k_[i] = std::max(newVal, config_.kMin);
    }
}

template<std::size_t Dim>
void SSTModel<Dim>::solveOmegaTransport(const VectorField<Dim>& velocity,
                                        ConvectionScheme scheme) {
    const std::size_t n = mesh_.nCells();
    const auto& c = config_.constants;

    Field<Dim> diffCoeff(mesh_, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        double sigmaW = F1_[i] * c.sigma_w1 + (1.0 - F1_[i]) * c.sigma_w2;
        diffCoeff[i] = (mu_ / rho_) + sigmaW * nut_[i];
    }

    Field<Dim> sourceOmega(mesh_, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        double w = std::max(omega_[i], config_.omegaMin);
        double k = std::max(k_[i], config_.kMin);
        double alpha = F1_[i] * c.alpha1 + (1.0 - F1_[i]) * c.alpha2;
        double nut = std::max(nut_[i], 1e-20);

        double Pk = nut_[i] * S_[i] * S_[i];
        double PkLim = std::min(Pk, sstProductionLimitFactor * c.beta_star * w * k);
        double prodTerm;
        if (PkLim >= Pk * 0.999999) {
            // Uncapped: exact cancellation Pk/nut = S^2
            prodTerm = alpha * S_[i] * S_[i];
        } else {
            // Capped: guard against tiny nut and bound the term
            double nutSafe = std::max(nut_[i], 1e-12);
            prodTerm = (alpha / nutSafe) * PkLim;
            if (prodTerm > 1e3) prodTerm = 1e3;
            if (prodTerm < -1e3) prodTerm = -1e3;
        }

        double dotProd = 0.0;
        for (std::size_t d = 0; d < Dim; ++d)
            dotProd += gradK_[d][i] * gradOmega_[d][i];
        double crossDiff = 2.0 * (1.0 - F1_[i]) * c.sigma_w2
                         * dotProd / w;
        if (crossDiff > 1e3) crossDiff = 1e3;
        if (crossDiff < -1e3) crossDiff = -1e3;

        sourceOmega[i] = prodTerm + crossDiff;
    }

    SparseMatrix A;
    Eigen::VectorXd b;
    poisson_.discretize(A, b, diffCoeff, sourceOmega,
                        omegaBCMask_, omegaBCVals_);

    std::array<const Field<Dim>*, Dim> velPtrs{};
    for (std::size_t d = 0; d < Dim; ++d)
        velPtrs[d] = &velocity[d];
    convection_.setScheme(scheme);

    std::array<Field<Dim>, Dim> gradOmega;
    for (auto& g : gradOmega) g = Field<Dim>(mesh_, 0.0);

    convection_.discretize(A, b, omega_, velPtrs, omegaBCMask_, omegaBCVals_,
                           nullptr);

    for (std::size_t i = 0; i < n; ++i) {
        if (omegaBCMask_[i]) continue;
        double beta = F1_[i] * c.beta1 + (1.0 - F1_[i]) * c.beta2;
        double w = std::max(omega_[i], config_.omegaMin);
        double diagInc = beta * w * mesh_.cellVolume(i);
        for (SparseMatrix::InnerIterator it(A, static_cast<Eigen::Index>(i)); it; ++it) {
            if (it.col() == static_cast<Eigen::Index>(i)) {
                it.valueRef() += diagInc;
                break;
            }
        }
    }

    // Under-relaxation of omega equation
    {
        double alpha = config_.alphaOmega; // config_.alphaOmega;
        double relax = (1.0 - alpha) / alpha;
        if (relax > 1e-14) {
            A.makeCompressed();
            for (std::size_t i = 0; i < n; ++i) {
                if (omegaBCMask_[i]) continue;
                for (SparseMatrix::InnerIterator it(A, static_cast<Eigen::Index>(i)); it; ++it) {
                    if (it.col() == static_cast<Eigen::Index>(i)) {
                        double aP = it.value();
                        if (aP > 1e-15) {
                            b(static_cast<Eigen::Index>(i)) += relax * aP * omega_[i];
                            it.valueRef() = aP / alpha;
                        }
                        break;
                    }
                }
            }
        }
    }

    Eigen::VectorXd x = solveOmegaSystem_(A, b);

    // Compute omega residual
    Eigen::VectorXd rOmega = b - A * x;
    omegaResidual_ = rOmega.lpNorm<2>();

    #pragma omp parallel for schedule(static)
    for (std::size_t i = 0; i < n; ++i) {
        double newVal = std::max(x(static_cast<Eigen::Index>(i)),
                                  config_.omegaMin);
        if (newVal > 1e3) newVal = 1e3;
        omega_[i] = std::max(newVal, config_.omegaMin);
    }
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
    gradK_ = computeScalarGradients(k_);
    gradOmega_ = computeScalarGradients(omega_);
    computeBlendingFunctions();
    computeEddyViscosity();
    solveKTransportTransient(velocity, scheme, dt, dtPrev, timeScheme, kOld, kOldPrev);
    solveOmegaTransportTransient(velocity, scheme, dt, dtPrev, timeScheme, omegaOld, omegaOldPrev);
    computeEddyViscosity();
}

template<std::size_t Dim>
void SSTModel<Dim>::solveKTransportTransient(const VectorField<Dim>& velocity,
                                            ConvectionScheme scheme,
                                            double dt,
                                            double dtPrev,
                                            TimeScheme timeScheme,
                                            const Field<Dim>& kOld,
                                            const Field<Dim>* kOldPrev) {
    const std::size_t n = mesh_.nCells();
    const auto& c = config_.constants;
    Field<Dim> diffCoeff(mesh_, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        double sigmaK = F1_[i] * c.sigma_k1 + (1.0 - F1_[i]) * c.sigma_k2;
        diffCoeff[i] = (mu_ / rho_) + sigmaK * nut_[i];
    }
    Field<Dim> sourceK(mesh_, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        double Pk = nut_[i] * S_[i] * S_[i];
        double PkLim = std::min(Pk, sstProductionLimitFactor * c.beta_star
                                    * std::max(omega_[i], config_.omegaMin)
                                    * std::max(k_[i], config_.kMin));
        sourceK[i] = PkLim;
    }
    SparseMatrix A;
    Eigen::VectorXd b;
    poisson_.discretize(A, b, diffCoeff, sourceK, kBCMask_, kBCVals_);
    std::array<const Field<Dim>*, Dim> velPtrs{};
    for (std::size_t d = 0; d < Dim; ++d) velPtrs[d] = &velocity[d];
    convection_.setScheme(scheme);
    std::array<Field<Dim>, Dim> gradK;
    for (auto& g : gradK) g = Field<Dim>(mesh_, 0.0);
    convection_.discretize(A, b, k_, velPtrs, kBCMask_, kBCVals_, nullptr);
    for (std::size_t i = 0; i < n; ++i) {
        if (kBCMask_[i]) continue;
        double destroyCoeff = c.beta_star * std::max(omega_[i], config_.omegaMin) * mesh_.cellVolume(i);
        for (SparseMatrix::InnerIterator it(A, static_cast<Eigen::Index>(i)); it; ++it) {
            if (it.col() == static_cast<Eigen::Index>(i)) { it.valueRef() += destroyCoeff; break; }
        }
    }
    // Transient term
    if (dt > 0 && dt < 1e14) {
        ImplicitTimeCoeffs tc;
        if (timeScheme == TimeScheme::BDF2 && kOldPrev != nullptr) {
            tc = implicitTimeCoeffs(dt, dtPrev);
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (kBCMask_[i]) continue;
            double vol = mesh_.cellVolume(i);
            const double coeff = tc.c0 * vol;
            const double rhs = tc.secondOrder
                ? vol * (tc.cOld * kOld[i] + tc.cOldPrev * (*kOldPrev)[i])
                : coeff * kOld[i];
            for (SparseMatrix::InnerIterator it(A, static_cast<Eigen::Index>(i)); it; ++it) {
                if (it.col() == static_cast<Eigen::Index>(i)) { it.valueRef() += coeff; break; }
            }
            b(static_cast<Eigen::Index>(i)) += rhs;
        }
    }
    Eigen::VectorXd x = solveKSystem_(A, b);
    Eigen::VectorXd rK = b - A * x;
    kResidual_ = rK.lpNorm<2>();
    #pragma omp parallel for schedule(static)
    for (std::size_t i = 0; i < n; ++i) {
        double newVal = std::max(x(static_cast<Eigen::Index>(i)), config_.kMin);
        k_[i] = config_.alphaK * newVal + (1.0 - config_.alphaK) * k_[i];
        k_[i] = std::max(k_[i], config_.kMin);
    }
}

template<std::size_t Dim>
void SSTModel<Dim>::solveOmegaTransportTransient(const VectorField<Dim>& velocity,
                                                ConvectionScheme scheme,
                                                double dt,
                                                double dtPrev,
                                                TimeScheme timeScheme,
                                                const Field<Dim>& omegaOld,
                                                const Field<Dim>* omegaOldPrev) {
    const std::size_t n = mesh_.nCells();
    const auto& c = config_.constants;
    Field<Dim> diffCoeff(mesh_, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        double sigmaW = F1_[i] * c.sigma_w1 + (1.0 - F1_[i]) * c.sigma_w2;
        diffCoeff[i] = (mu_ / rho_) + sigmaW * nut_[i];
    }
    Field<Dim> sourceOmega(mesh_, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        double w = std::max(omega_[i], config_.omegaMin);
        double k = std::max(k_[i], config_.kMin);
        double alpha = F1_[i] * c.alpha1 + (1.0 - F1_[i]) * c.alpha2;
        double nut = std::max(nut_[i], 1e-20);
        double Pk = nut_[i] * S_[i] * S_[i];
        double PkLim = std::min(Pk, sstProductionLimitFactor * c.beta_star * w * k);
        double prodTerm = (alpha / nut) * PkLim;
        double dotProd = 0.0;
        for (std::size_t d = 0; d < Dim; ++d) dotProd += gradK_[d][i] * gradOmega_[d][i];
        double crossDiff = 2.0 * (1.0 - F1_[i]) * c.sigma_w2 * dotProd / w;
        sourceOmega[i] = prodTerm + crossDiff;
    }
    SparseMatrix A;
    Eigen::VectorXd b;
    poisson_.discretize(A, b, diffCoeff, sourceOmega, omegaBCMask_, omegaBCVals_);
    std::array<const Field<Dim>*, Dim> velPtrs{};
    for (std::size_t d = 0; d < Dim; ++d) velPtrs[d] = &velocity[d];
    convection_.setScheme(scheme);
    std::array<Field<Dim>, Dim> gradOmega;
    for (auto& g : gradOmega) g = Field<Dim>(mesh_, 0.0);
    convection_.discretize(A, b, omega_, velPtrs, omegaBCMask_, omegaBCVals_, nullptr);
    for (std::size_t i = 0; i < n; ++i) {
        if (omegaBCMask_[i]) continue;
        double beta = F1_[i] * c.beta1 + (1.0 - F1_[i]) * c.beta2;
        double w = std::max(omega_[i], config_.omegaMin);
        double diagInc = beta * w * mesh_.cellVolume(i);
        for (SparseMatrix::InnerIterator it(A, static_cast<Eigen::Index>(i)); it; ++it) {
            if (it.col() == static_cast<Eigen::Index>(i)) { it.valueRef() += diagInc; break; }
        }
    }
    if (dt > 0 && dt < 1e14) {
        ImplicitTimeCoeffs tc;
        if (timeScheme == TimeScheme::BDF2 && omegaOldPrev != nullptr) {
            tc = implicitTimeCoeffs(dt, dtPrev);
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (omegaBCMask_[i]) continue;
            double vol = mesh_.cellVolume(i);
            const double coeff = tc.c0 * vol;
            const double rhs = tc.secondOrder
                ? vol * (tc.cOld * omegaOld[i] + tc.cOldPrev * (*omegaOldPrev)[i])
                : coeff * omegaOld[i];
            for (SparseMatrix::InnerIterator it(A, static_cast<Eigen::Index>(i)); it; ++it) {
                if (it.col() == static_cast<Eigen::Index>(i)) { it.valueRef() += coeff; break; }
            }
            b(static_cast<Eigen::Index>(i)) += rhs;
        }
    }
    Eigen::VectorXd x = solveOmegaSystem_(A, b);
    Eigen::VectorXd rOmega = b - A * x;
    omegaResidual_ = rOmega.lpNorm<2>();
    #pragma omp parallel for schedule(static)
    for (std::size_t i = 0; i < n; ++i) {
        double newVal = std::max(x(static_cast<Eigen::Index>(i)), config_.omegaMin);
        omega_[i] = config_.alphaOmega * newVal + (1.0 - config_.alphaOmega) * omega_[i];
        omega_[i] = std::max(omega_[i], config_.omegaMin);
    }
}

template<std::size_t Dim>
Eigen::VectorXd SSTModel<Dim>::solveKSystem_(const SparseMatrix& A,
                                              const Eigen::VectorXd& b) {
    // Use compute (analyze+factorize) for robustness; IncompleteLUT needs compute
    kSolver_.compute(A);
    Eigen::VectorXd x = kSolver_.solve(b);
    if (kSolver_.info() != Eigen::Success) {
        // Fallback: try without preconditioner if fails
        Eigen::BiCGSTAB<SparseMatrix> fallback;
        fallback.setMaxIterations(static_cast<int>(sstMaxIterations));
        fallback.setTolerance(sstTolerance);
        fallback.compute(A);
        x = fallback.solve(b);
    }
    for (Eigen::Index i = 0; i < x.size(); ++i)
        if (std::isnan(x(i)) || std::isinf(x(i))) {
            std::fprintf(stderr, "  WARNING: SST solver produced non-finite value at %ld\n", (long)i);
            x(i) = 0.0;
        }
    return x;
}

template<std::size_t Dim>
Eigen::VectorXd SSTModel<Dim>::solveOmegaSystem_(const SparseMatrix& A,
                                                  const Eigen::VectorXd& b) {
    omegaSolver_.compute(A);
    Eigen::VectorXd x = omegaSolver_.solve(b);
    // fallback check
    if (omegaSolver_.info() != Eigen::Success) {
        Eigen::BiCGSTAB<SparseMatrix> fallback;
        fallback.setMaxIterations(static_cast<int>(sstMaxIterations));
        fallback.setTolerance(sstTolerance);
        fallback.compute(A);
        x = fallback.solve(b);
    }
    for (Eigen::Index i = 0; i < x.size(); ++i)
        if (std::isnan(x(i)) || std::isinf(x(i))) {
            std::fprintf(stderr, "  WARNING: SST solver produced non-finite value at %ld\n", (long)i);
            x(i) = 0.0;
        }
    return x;
}

template<std::size_t Dim>
Eigen::VectorXd SSTModel<Dim>::biCGSTABSolve(const SparseMatrix& A,
                                              const Eigen::VectorXd& b) {
    Eigen::VectorXd x;
    Eigen::BiCGSTAB<SparseMatrix, Eigen::IncompleteLUT<double>> solver;
    solver.setMaxIterations(static_cast<int>(sstMaxIterations));
    solver.setTolerance(sstTolerance);
    if (patternAnalyzed_) {
        solver.analyzePattern(A);
    } else {
        solver.compute(A);
        patternAnalyzed_ = true;
    }
    solver.factorize(A);
    x = solver.solve(b);
    for (Eigen::Index i = 0; i < x.size(); ++i)
        if (std::isnan(x(i)) || std::isinf(x(i))) {
            std::fprintf(stderr, "  WARNING: SST solver produced non-finite value at %ld\n", (long)i);
            x(i) = 0.0;
        }
    return x;
}

template class SSTModel<2>;
template class SSTModel<3>;

} // namespace fvm
