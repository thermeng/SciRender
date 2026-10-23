#include "fvm/SIMPLE.h"

#include "fvm/PressureCorrection.h"
#include "fvm/Residual.h"
#include "fvm/VelocityCorrection.h"
#include "fvm/SSTConstants.h"
#include "io/VTKXMLWriter.h"

#include <iomanip>
#include <cmath>
#include <limits>
#include <cctype>
#include <fstream>
#include <algorithm>
#include <chrono>

namespace fvm {

template<std::size_t Dim>
SIMPLE<Dim>::SIMPLE(const Mesh<Dim>& mesh, double rho, double nu, const Config& config)
    : mesh_(mesh),
      rho_(rho),
      nu_(nu),
      config_(config),
      momentum_(mesh, rho, nu),
      pressureCorrection_(mesh),
      pPrime_(mesh, 0.0),
      momDiag_(mesh, 0.0),
      uStar_(mesh, 0.0),
      bc_(mesh) {
    applyPressureSolverConfig_();
    for (auto& dc : dCoeff_) dc.assign(mesh.nCells(), 0.0);
    for (auto& g : velGrad_) g = Field<Dim>(mesh, 0.0);
    for (auto& g : pressureGrad_) g = Field<Dim>(mesh, 0.0);
}

template<std::size_t Dim>
void SIMPLE<Dim>::applyPressureSolverConfig_() {
    typename PressureCorrection<Dim>::Config pcCfg;
    pcCfg.rhieChow = config_.rhieChow;
    pcCfg.pressureRefCell = config_.pressureRefCell;
    pcCfg.useAMG = config_.pressureUseAMG;
    pcCfg.useAMGPreconditioner = config_.pressureUseAMGPreconditioner;
    pcCfg.amgThreshold = config_.pressureAmgThreshold;
    pcCfg.amgConfig = config_.pressureAmgConfig;
    pressureCorrection_.setConfig(pcCfg);
}

template<std::size_t Dim>
void SIMPLE<Dim>::bindVelocity(VectorField<Dim>& velocity) {
    velocity_.emplace(std::ref(velocity));
}

template<std::size_t Dim>
void SIMPLE<Dim>::bindPressure(Field<Dim>& pressure) {
    pressure_.emplace(std::ref(pressure));
}

template<std::size_t Dim>
VectorField<Dim>& SIMPLE<Dim>::boundVelocity() {
    return velocity_->get();
}

template<std::size_t Dim>
const VectorField<Dim>& SIMPLE<Dim>::boundVelocity() const {
    return velocity_->get();
}

template<std::size_t Dim>
Field<Dim>& SIMPLE<Dim>::boundPressure() {
    return pressure_->get();
}

template<std::size_t Dim>
const Field<Dim>& SIMPLE<Dim>::boundPressure() const {
    return pressure_->get();
}

template<std::size_t Dim>
void SIMPLE<Dim>::setVelocityBC(std::size_t d, int sign, VelocityBCType type,
                                 double val1, double val2, double val3) {
    bc_.setVelocityBC(d, sign, type, val1, val2, val3);
}

template<std::size_t Dim>
void SIMPLE<Dim>::setMin(std::size_t d, VelocityBCType type,
                         double u, double v, double w) {
    bc_.setVelocityBC(d, -1, type, u, v, w);
}

template<std::size_t Dim>
void SIMPLE<Dim>::setMax(std::size_t d, VelocityBCType type,
                         double u, double v, double w) {
    bc_.setVelocityBC(d, +1, type, u, v, w);
}

template<std::size_t Dim>
void SIMPLE<Dim>::addVelocityPatch(const std::string& name, std::size_t d, int sign,
                                   VelocityBCType type, double u, double v, double w) {
    bc_.addVelocityPatch(name, d, sign, type, u, v, w);
}

template<std::size_t Dim>
const typename SIMPLE<Dim>::VelocityPatch* SIMPLE<Dim>::findVelocityPatch(
    const std::string& name) const {
    return bc_.findVelocityPatch(name);
}

template<std::size_t Dim>
const std::vector<typename SIMPLE<Dim>::VelocityPatch>&
SIMPLE<Dim>::velocityPatches() const {
    return bc_.velocityPatches();
}

template<std::size_t Dim>
void SIMPLE<Dim>::setVelocityBCOnFaces(const std::vector<std::size_t>& faceIndices,
                                       VelocityBCType type,
                                       double u, double v, double w) {
    bc_.setVelocityBCOnFaces(faceIndices, type, u, v, w);
}

template<std::size_t Dim>
void SIMPLE<Dim>::setVelocityBCOnFacesWithSlip(const std::vector<std::size_t>& faceIndices,
                                               const std::string& typeStr,
                                               double u, double v, double w,
                                               bool slip,
                                               double pressure) {
    bc_.setVelocityBCOnFacesWithSlip(faceIndices, typeStr, u, v, w, slip, pressure);
}

template<std::size_t Dim>
void SIMPLE<Dim>::setPressureOutlet(std::size_t d, int sign, double pressure) {
    bc_.setPressureOutlet(d, sign, pressure);
}

template<std::size_t Dim>
void SIMPLE<Dim>::setPressureOutletOnFaces(const std::vector<std::size_t>& faceIndices,
                                           double pressure) {
    bc_.setPressureOutletOnFaces(faceIndices, pressure);
}

template<std::size_t Dim>
void SIMPLE<Dim>::setPressureOutletOnFacesWithBackflow(const std::vector<std::size_t>& faceIndices,
                                                       double pressure,
                                                       bool suppressBackflow,
                                                       double u, double v, double w,
                                                       double k, double omega) {
    bc_.setPressureOutletOnFacesWithBackflow(faceIndices, pressure, suppressBackflow, u, v, w, k, omega);
}

template<std::size_t Dim>
void SIMPLE<Dim>::enableTurbulence(double kInf, double omegaInf,
                                   double alphaK, double alphaOmega) {
    config_.turbulenceEnabled = true;
    config_.kInf = kInf;
    config_.omegaInf = omegaInf;
    config_.alphaK = alphaK;
    config_.alphaOmega = alphaOmega;
}

template<std::size_t Dim>
void SIMPLE<Dim>::initializeSolve_(VectorField<Dim>& velocity, Field<Dim>& pressure) {
    bindVelocity(velocity);
    bindPressure(pressure);

    lastIterations_ = 0;
    lastContResid_ = 0.0;
    lastMomResid_ = 0.0;
    convergedStreak_ = 0;
    aborted_ = false;
    {
        bool isPISO = (config_.solverType == SolverType::PISO) ||
                      (config_.transient.enabled && config_.transient.piso);
        adaptiveAlphaP_ = isPISO ? 1.0 : config_.alphaP;
    }
    prevContResid_ = 0.0;
    stallCount_ = 0;
    headerCounter_ = 0;

    if (!config_.checkpoint.restartFrom.empty()) {
        std::size_t savedIter = 0;
        double savedCont = 0.0, savedMom = 0.0;
        double savedAlphaP = config_.alphaP;
        std::size_t savedStall = 0;
        double savedPrev = 0.0;

        Field<Dim>* kPtr = nullptr;
        Field<Dim>* omegaPtr = nullptr;
        Field<Dim>* nutPtr = nullptr;
        if (turbulence_) {
            kPtr = &turbulence_->k();
            omegaPtr = &turbulence_->omega();
            nutPtr = &turbulence_->nut();
        }

        if (Checkpoint::load(config_.checkpoint.restartFrom, mesh_,
                             savedIter, savedCont, savedMom,
                             savedAlphaP, savedStall, savedPrev,
                             velocity, pressure,
                             kPtr, omegaPtr, nutPtr)) {
            startIteration_ = savedIter;
            restarting_ = true;
            {
                bool isPISO = (config_.solverType == SolverType::PISO) ||
                              (config_.transient.enabled && config_.transient.piso);
                adaptiveAlphaP_ = isPISO ? 1.0 : savedAlphaP;
            }
            stallCount_ = savedStall;
            prevContResid_ = savedPrev;
            lastContResid_ = std::numeric_limits<double>::max();
            lastMomResid_ = std::numeric_limits<double>::max();
            std::cout << "  [checkpoint] loaded from " << config_.checkpoint.restartFrom
                      << " at iteration " << savedIter << "\n";
        } else {
            std::cerr << "  [checkpoint] WARNING: failed to load " << config_.checkpoint.restartFrom
                      << ", starting from scratch\n";
            startIteration_ = 0;
        }
    } else {
        startIteration_ = 0;
    }

    updateMaxInletSpeed_();

    divBuffer_ = Field<Dim>(mesh_, 0.0);

    for (std::size_t d = 0; d < Dim; ++d) {
        dpdx_[d] = Field<Dim>(mesh_, 0.0);
    }

    gradientCalc_ = std::make_unique<Gradient<Dim>>(mesh_);
    gradientCalc_->setScheme(config_.gradientScheme);

    if (config_.turbulenceEnabled) {
        typename SSTModel<Dim>::Config sstCfg;
        sstCfg.kInf       = config_.kInf;
        sstCfg.omegaInf   = config_.omegaInf;
        sstCfg.alphaK     = config_.alphaK;
        sstCfg.alphaOmega = config_.alphaOmega;
        turbulence_ = std::make_unique<SSTModel<Dim>>(mesh_, sstCfg);
        turbulence_->initialize(nu_ * rho_, rho_);

        using BC = VelocityBCType;
        std::array<BC, 2*Dim> velBCEnums{};
        for (std::size_t i = 0; i < 2 * Dim; ++i)
            velBCEnums[i] = bc_.velBCs()[i].type;
        turbulence_->computeWallDistance(velBCEnums);
        turbulence_->setBoundaryConditions(velBCEnums);
    }

    velocityOld_ = VectorField<Dim>(mesh_, 0.0);
    nutOld_ = Field<Dim>(mesh_, 0.0);
    initialMassImbalance_ = 0.0;
    initialMassImbalanceSet_ = false;
    residualHistory_.clear();
    divergenceCount_ = 0;
    stallCount_ = 0;
    prevContResid_ = 0.0;
}

template<std::size_t Dim>
void SIMPLE<Dim>::performIteration_(VectorField<Dim>& velocity, Field<Dim>& pressure, std::size_t iter) {
    for (std::size_t d = 0; d < Dim; ++d)
        std::copy((boundVelocity())[d].values().begin(), (boundVelocity())[d].values().end(),
                  velocityOld_[d].values().begin());

    // The boundary discretization is rebuilt only when the boundary
    // conditions actually changed. Rebuilding it every outer iteration copied
    // six per-cell vectors (and their allocations) for nothing.
    if (bc_.isDirty()) bcDiscretization_ = bc_.build();

    solveMomentum_(uStar_, velocityOld_);

    int nCorr = static_cast<int>(config_.pressureCorrectors);
    if (nCorr < 1) nCorr = 1;
    for (int corr = 0; corr < nCorr; ++corr) {
        pressureCorrection_.solve(uStar_, dCoeff_, boundPressure(), pPrime_,
                                 bcDiscretization_.pressureOutletCells(), bcDiscretization_.isPressureNeumann(),
                                 &bcDiscretization_.isPressureDirichletFaces(), &bcDiscretization_.pressureOutletFaces(),
                                 &bcDiscretization_.faceBCs(), &bcDiscretization_.faceHasBCs(),
                                 &bcDiscretization_.dirichletMask(), &bcDiscretization_.dirichletVals());

        // Scale p' only if extreme (configurable multiple of dynamic pressure).
        limitPressureCorrection_(pPrime_);

        pressureBeforeCorrection_ = boundPressure();
        pressureGradBefore_ = pressureCorrection_.pressureGradient();

        correctVelocity(mesh_, uStar_, pPrime_, dCoeff_,
                    bcDiscretization_.dirichletMask(), bcDiscretization_.dirichletVals(), dpdx_, boundVelocity(),
                    &bcDiscretization_.faceBCs(), &bcDiscretization_.faceHasBCs());

        #pragma omp parallel for schedule(static)
        for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
            (boundPressure())[i] += adaptiveAlphaP_ * pPrime_[i];
        }

        // Face-based pressure outlet (exact per-face pRef, fixes cell dedup loss)
        if (!bcDiscretization_.pressureOutletFaces().empty()) {
            for (const auto& [face, pRef] : bcDiscretization_.pressureOutletFaces()) {
                if (face < mesh_.nFaces()) {
                    std::size_t cell = mesh_.owner(face);
                    (boundPressure())[cell] = pRef;
                }
            }
        } else {
            for (const auto& [cell, pRef] : bcDiscretization_.pressureOutletCells()) {
                (boundPressure())[cell] = pRef;
            }
        }

        // For second corrector, update uStar to corrected velocity for next div calculation
        if (corr + 1 < nCorr) {
            uStar_ = boundVelocity();
        }
    }

    if (turbulence_) {
        nutOld_ = turbulence_->nut();
        turbulence_->solveIteration(boundVelocity(), config_.convectionScheme);
        double alphaNut = nutUnderRelax;
        auto& nut = turbulence_->nut();
        #pragma omp parallel for schedule(static)
        for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
            nut[i] = alphaNut * nut[i] + (1.0 - alphaNut) * nutOld_[i];
        }
    }
}

template<std::size_t Dim>
std::size_t SIMPLE<Dim>::sparsityHash_(const SparseMatrix& A) {
    // FNV-1a over the structural (i, j) pairs only — values are excluded so the
    // hash changes only when the stencil does.
    std::size_t h = 1469598103934665603ull;
    auto mix = [&h](std::size_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    for (Eigen::Index j = 0; j < A.outerSize(); ++j) {
        for (SparseMatrix::InnerIterator it(A, j); it; ++it) {
            mix(static_cast<std::size_t>(it.row()));
            mix(static_cast<std::size_t>(it.col()));
        }
    }
    return h;
}

template<std::size_t Dim>
bool SIMPLE<Dim>::hasPositiveOffDiagonals_(const SparseMatrix& A) {
    for (Eigen::Index k = 0; k < A.outerSize(); ++k) {
        for (SparseMatrix::InnerIterator it(A, k); it; ++it) {
            if (it.row() != it.col() && it.value() > 0.0) return true;
        }
    }
    return false;
}

template <std::size_t Dim>
Eigen::VectorXd SIMPLE<Dim>::solveMomentumILU_(std::size_t d, std::size_t& itersOut) {
    if (!momPatternAnalyzed_) {
        momSolver_.setMaxIterations(50000);
        momSolver_.setTolerance(1e-8);
        momSolver_.analyzePattern(momA_);
        momPatternAnalyzed_ = true;
    }
    momSolver_.factorize(momA_);
    Eigen::VectorXd x = momSolver_.solve(momB_);
    itersOut = static_cast<std::size_t>(momSolver_.iterations());
    if (momSolver_.info() != Eigen::Success) {
        std::cerr << "  WARNING: momentum-" << (d == 0 ? "U" : d == 1 ? "V" : "W")
                  << " BiCGSTAB failed (iters=" << momSolver_.iterations()
                  << ", resid=" << momSolver_.error() << ")\n";
    }
    return x;
}

template <std::size_t Dim>
Eigen::VectorXd SIMPLE<Dim>::solveMomentumLinear_(std::size_t d) {
    const std::size_t n = mesh_.nCells();
    ++momStats_.solves;

    const bool amgEligible =
        config_.useAMGMomentum &&
        !momAmgDisabledReason_ &&
        n >= config_.momentumAmgThreshold;

    if (!amgEligible) {
        if (config_.useAMGMomentum && !momAmgDisabledReason_ &&
            n < config_.momentumAmgThreshold) {
            momAmgDisabledReason_ = true;  // decision is permanent
        }
        std::size_t iters = 0;
        const auto t0 = std::chrono::steady_clock::now();
        Eigen::VectorXd x = solveMomentumILU_(d, iters);
        momStats_.iluSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        ++momStats_.iluSolves;
        momStats_.krylovIters += iters;
        return x;
    }

    // Central differencing puts positive entries on the outflow off-diagonals,
    // which destroys the M-matrix property the interpolation relies on. Detect
    // it from the assembled matrix rather than trusting the scheme enum, since
    // the sign pattern also depends on BC configuration.
    if (hasPositiveOffDiagonals_(momA_)) {
        momAmgDisabledReason_ = true;
        std::cerr << "  NOTE: momentum matrix has positive off-diagonal entries "
                     "(not an M-matrix) — AMG preconditioning disabled, using ILU.\n";
        std::size_t iters = 0;
        Eigen::VectorXd x = solveMomentumILU_(d, iters);
        ++momStats_.iluSolves;
        momStats_.krylovIters += iters;
        return x;
    }

    const std::size_t hash = sparsityHash_(momA_);
    const bool needBuild = !momAmgBuilt_[d] || hash != momAmgPatternHash_[d];

    if (needBuild) {
        const auto tb = std::chrono::steady_clock::now();
        momAmg_[d].setConfig(config_.momentumAmgConfig);
        momAmg_[d].build(momA_);
        momAmgPatternHash_[d] = hash;
        momAmgBuilt_[d] = momAmg_[d].isBuilt();
        momAmgValueUpdates_[d] = 0;
        ++momStats_.amgBuilds;
        momStats_.buildSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - tb).count();
    } else if (++momAmgValueUpdates_[d] >= std::max<std::size_t>(1, config_.momentumAmgRebuildInterval)) {
        // The momentum matrix VALUES change every solve (convective flux,
        // pressure gradient, under-relaxation, rho*V/dt), unlike the pressure
        // correction matrix which is constant between pattern changes. Without
        // this the fine-level smoother runs on iteration-1 coefficients and the
        // preconditioner degrades until BiCGSTAB stalls.
        //
        // The rebuild is amortised: updateFineValues() rebuilds the whole
        // hierarchy (the coarse levels depend on A through the Galerkin
        // product, so a value-only fine update would be unsound), and doing
        // that on every solve costs more than the solve it accelerates.
        momAmgValueUpdates_[d] = 0;
        momAmg_[d].updateFineValues(momA_);
    }

    if (!momAmgBuilt_[d]) {
        std::size_t iters = 0;
        Eigen::VectorXd x = solveMomentumILU_(d, iters);
        ++momStats_.iluSolves;
        momStats_.krylovIters += iters;
        return x;
    }

    momAmgPrec_[d].setAMG(&momAmg_[d]);
    momAmgPrec_[d].setCycles(config_.momentumAmgCycles);
    if (!momAmgPatternAnalyzed_[d]) {
        momAmgSolver_[d].setMaxIterations(5000);
        momAmgSolver_[d].setTolerance(config_.momentumAmgMaxResidual);
        momAmgSolver_[d].analyzePattern(momA_);
        momAmgPatternAnalyzed_[d] = true;
    }

    const auto t0 = std::chrono::steady_clock::now();
    momAmgSolver_[d].factorize(momA_);
    Eigen::VectorXd x = momAmgSolver_[d].solve(momB_);
    momStats_.amgSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // Verify the true residual rather than trusting the Krylov solver's own
    // estimate: BiCGSTAB can break down with a nonsymmetric preconditioner and
    // return a plausible-looking wrong answer. The threshold has to be tight —
    // a 1e-4-accurate velocity solve fed back into SIMPLE compounds over
    // hundreds of outer iterations and saturates the velocity clamp.
    const double bnorm = momB_.norm();
    const double rnorm = (momA_ * x - momB_).norm();
    const double rel = bnorm > 0.0 ? rnorm / bnorm : rnorm;

    // NaN-safe: `rel > tol` is FALSE when rel is NaN, so a diverged solve
    // would slip through a naive comparison. Negating the `<=` catches it.
    const bool residualOk = (rel <= config_.momentumAmgMaxResidual);
    const bool bad =
        !residualOk ||
        !x.allFinite() ||
        !std::isfinite(rel) ||
        (momAmgSolver_[d].info() != Eigen::Success);

    if (bad) {
        ++momStats_.amgRejected;
        std::size_t iters = 0;
        Eigen::VectorXd xf = solveMomentumILU_(d, iters);
        ++momStats_.iluSolves;
        momStats_.krylovIters += iters;
        return xf;
    }

    ++momStats_.amgSolves;
    momStats_.amgKrylovIters += static_cast<std::size_t>(momAmgSolver_[d].iterations());
    momStats_.krylovIters += static_cast<std::size_t>(momAmgSolver_[d].iterations());
    return x;
}

template <std::size_t Dim>
void SIMPLE<Dim>::printMomentumSolverStats_() const {
    if (!config_.momentumSolverStats) return;
    std::cerr << "\n  Momentum linear solver statistics\n";
    std::cerr << "    solves:            " << momStats_.solves << "\n";
    std::cerr << "    AMG solves:        " << momStats_.amgSolves << "\n";
    std::cerr << "    ILU solves:        " << momStats_.iluSolves << "\n";
    std::cerr << "    AMG builds:        " << momStats_.amgBuilds << "\n";
    std::cerr << "    AMG rejected:      " << momStats_.amgRejected
              << "  (fell back to ILU)\n";
    std::cerr << "    Krylov iters:      " << momStats_.krylovIters << "\n";
    if (momStats_.solves > 0) {
        std::cerr << "    Krylov iters/solve:"
                  << "  " << (static_cast<double>(momStats_.krylovIters) /
                              static_cast<double>(momStats_.solves)) << "\n";
    }
    std::cerr << "    AMG build time:    " << momStats_.buildSeconds << " s\n";
    std::cerr << "    AMG solve time:    " << momStats_.amgSeconds << " s\n";
    std::cerr << "    ILU solve time:    " << momStats_.iluSeconds << " s\n";
}

template<std::size_t Dim>
double SIMPLE<Dim>::momentumResidual_(std::size_t d) const {
    if (d >= Dim) return 0.0;
    // Use the per-direction snapshot. momA_/momB_ are shared across
    // components and hold only the LAST component's system, so using them
    // here scores the u field against the v operator.
    const SparseMatrix& A = momAsnap_[d];
    const Eigen::VectorXd& b = momBsnap_[d];
    const Eigen::Index n = A.rows();
    if (n <= 0 || A.cols() != n || b.size() != n) return 0.0;
    const auto& u = (boundVelocity()[d]).values();
    if (static_cast<Eigen::Index>(u.size()) != n) return 0.0;

    // |A u - b| / sum|b| evaluated at the current velocity. The equation was
    // assembled with the previous pressure, so at convergence this measures
    // how much the pressure/velocity corrections displaced the solution, which
    // is the same convention Fluent uses. It tends to zero as p' -> 0.
    // A is stored column-major, so the row-wise dot products must be computed
    // via a proper matrix-vector product rather than by iterating columns.
    Eigen::Map<const Eigen::VectorXd> uvec(u.data(), n);
    Eigen::VectorXd r = A * uvec - b;
    double num = r.cwiseAbs().sum();
    double den = b.cwiseAbs().sum();
    return den > 1e-30 ? num / den : num;
}

template<std::size_t Dim>
void SIMPLE<Dim>::updateResiduals_() {    ResidualResult resid = computeResiduals(mesh_, boundVelocity(), velocityOld_, diag_,
                                             divBuffer_, bcDiscretization_.dirichletMask(), bcDiscretization_.dirichletVals(),
                                              &bcDiscretization_.faceBCs(), &bcDiscretization_.faceHasBCs(),
                                              &dCoeff_, &pressureBeforeCorrection_, &pressureGradBefore_, &pressureCorrection_.pressureMask(),
                                              config_.rhieChow);
    if (!initialMassImbalanceSet_) {
        initialMassImbalance_ = resid.continuity;
        initialMassImbalanceSet_ = true;
    }
    // resid.continuity is ALREADY a normalised, scale-free mass imbalance
    // (sum|div|*V / sum|flux|), exactly the quantity Fluent reports. Dividing it
    // again by a reference captured after the first pressure corrector turned it
    // into a ratio whose denominator is whatever the first corrector happened to
    // leave behind. That made `tolerance` unreachable in practice, so every run
    // ground on to maxIterations while printing an O(1) "residual".
    lastContResid_ = resid.continuity;
    if (std::getenv("CFDSOLVER_TRACE_RESID")) {
        std::cerr << "  [trace] cont=" << resid.continuity
                  << " momLin=" << momentumResidual_(0) << "\n";
    }

    lastMomResidU_ = momentumResidual_(0);
    lastMomResidV_ = (Dim >= 2) ? momentumResidual_(1) : 0.0;
    lastMomResidW_ = (Dim >= 3) ? momentumResidual_(2) : 0.0;
    lastMomResid_ = std::max(lastMomResidU_, lastMomResidV_);
    if constexpr (Dim == 3) {
        lastMomResid_ = std::max(lastMomResid_, lastMomResidW_);
    }
    lastMomChange_ = std::max(resid.momU, resid.momV);
    if constexpr (Dim == 3) lastMomChange_ = std::max(lastMomChange_, resid.momW);

    // PISO robustness: no adaptive alphaP reduction (Fluent keeps 1.0)
    bool isPISO = (config_.solverType == SolverType::PISO) ||
                  (config_.transient.enabled && config_.transient.piso);
    if (isPISO) adaptiveAlphaP_ = 1.0;
    prevContResid_ = lastContResid_;

    residualHistory_.push_back(lastContResid_);
    if (residualHistory_.size() > config_.residualHistoryWindow) {
        bool diverging = true;
        for (std::size_t h = 1; h < residualHistory_.size(); ++h) {
            if (residualHistory_[h] <= residualHistory_[h-1]) {
                diverging = false;
                break;
            }
        }
        if (diverging) {
            ++divergenceCount_;
            if (divergenceCount_ >= divergenceAbortCount) {
                std::cerr << "  WARNING: continuity residual increasing for "
                          << divergenceCount_ << " iterations, aborting\n";
            }
        } else {
            divergenceCount_ = 0;
        }
        residualHistory_.clear();
    }
}

template<std::size_t Dim>
bool SIMPLE<Dim>::checkConvergence_(std::size_t iter, bool checkTolerance) {
    if (std::isnan(lastContResid_) || std::isinf(lastContResid_)) {
        aborted_ = true;
        std::cerr << "  WARNING: NaN/Inf continuity residual at iteration " << (iter + 1) << ", aborting\n";
        return true;
    }
    if (divergenceCount_ >= divergenceAbortCount) {
        aborted_ = true;
        return true;
    }
    if (cancelRequested_.load(std::memory_order_acquire)) {
        return true;
    }
    if (checkTolerance && !config_.runFixedIterations) {
        if (lastContResid_ < config_.tolerance && lastMomChange_ < config_.tolerance) {
            // The criterion must hold for `convergenceHoldIterations`
            // consecutive iterations before the run is declared converged: a
            // single lucky iteration is not a steady state.
            if (++convergedStreak_ >= std::max(1, config_.convergenceHoldIterations))
                return true;
        } else {
            convergedStreak_ = 0;
        }
    }
    return false;
}

template<std::size_t Dim>
void SIMPLE<Dim>::saveCheckpoint_(std::size_t iter) {
    saveCheckpoint_(iter, nullptr);
}

template<std::size_t Dim>
void SIMPLE<Dim>::saveCheckpoint_(std::size_t iter, const TransientBlock<Dim>* transient) {
    if (config_.checkpoint.saveEvery > 0 &&
        (iter + 1) % config_.checkpoint.saveEvery == 0) {
        const Field<Dim>* kPtr = nullptr;
        const Field<Dim>* omegaPtr = nullptr;
        const Field<Dim>* nutPtr = nullptr;
        if (turbulence_) {
            kPtr = &turbulence_->k();
            omegaPtr = &turbulence_->omega();
            nutPtr = &turbulence_->nut();
        }
        Checkpoint::save(config_.checkpoint, iter + 1,
                         lastContResid_, lastMomResid_,
                         adaptiveAlphaP_, stallCount_, prevContResid_,
                         boundVelocity(), boundPressure(),
                         kPtr, omegaPtr, nutPtr, transient);
    }
}

template<std::size_t Dim>
void SIMPLE<Dim>::printHeader_(bool isTransient) const {
    constexpr int wIter = 6;
    constexpr int wRes = 13;
    constexpr int wTime = 13;
    int nCols = 1; // iter/step
    if (isTransient) nCols += 2; // time, dt
    nCols += 1; // continuity
    nCols += Dim; // velocities
    if (turbulence_) nCols += 2;
    int width = wIter + nCols * wRes;
    if (width < 78) width = 78;
    std::ostringstream hdr;
    hdr << std::string(width, '-') << "\n";
    if (isTransient) {
        hdr << std::setw(wIter) << "step"
            << std::setw(wTime) << "time"
            << std::setw(wTime) << "dt"
            << std::setw(wRes) << "continuity"
            << std::setw(wRes) << "x-velocity"
            << std::setw(wRes) << "y-velocity";
        if constexpr (Dim == 3) hdr << std::setw(wRes) << "z-velocity";
        if (turbulence_) hdr << std::setw(wRes) << "k" << std::setw(wRes) << "omega";
        hdr << "\n" << std::string(width, '-') ;
    } else {
        hdr << std::setw(wIter) << "iter"
            << std::setw(wRes) << "continuity"
            << std::setw(wRes) << "x-velocity"
            << std::setw(wRes) << "y-velocity";
        if constexpr (Dim == 3) hdr << std::setw(wRes) << "z-velocity";
        if (turbulence_) hdr << std::setw(wRes) << "k" << std::setw(wRes) << "omega";
        hdr << "\n" << std::string(width, '-') ;
    }
    std::cout << hdr.str() << std::endl;
}

template<std::size_t Dim>
void SIMPLE<Dim>::printIterationOutput_(std::size_t iter) {
    if ((iter + 1) % config_.printFrequency == 0 || iter + 1 == config_.maxIterations) {
        if (headerCounter_ % 25 == 0) printHeader_(false);
        ++headerCounter_;
        constexpr int wIter = 6;
        constexpr int wRes = 13;
        std::ostringstream line;
        line << std::setw(wIter) << (iter + 1)
             << std::scientific << std::setprecision(4)
             << std::setw(wRes) << lastContResid_
             << std::setw(wRes) << lastMomResidU_
             << std::setw(wRes) << lastMomResidV_;
        if constexpr (Dim == 3) line << std::setw(wRes) << lastMomResidW_;
        if (turbulence_) line << std::setw(wRes) << turbulence_->finalKResid()
                               << std::setw(wRes) << turbulence_->finalOmegaResid();
        std::cout << line.str() << std::endl;
    }
}

template<std::size_t Dim>
void SIMPLE<Dim>::printFinalStats_() const {
    double maxU = 0.0, maxV = 0.0;
    for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
        if (mesh_.isBoundary(i)) continue;
        maxU = std::max(maxU, std::abs((boundVelocity())[0][i]));
        maxV = std::max(maxV, std::abs((boundVelocity())[1][i]));
    }
    // The verdict comes first: "max|u|" alone says nothing about whether the
    // result is a steady state.
    std::cerr << "  [final] status: " << statusText()
              << "  (iterations=" << lastIterations_
              << ", continuity=" << lastContResid_ << ")\n";
    if (aborted_) {
        std::cerr << "  [final] ABORTED (diverged or NaN residuals)\n";
    } else {
        std::cerr << "  [final] max|u|=" << maxU << "  max|v|=" << maxV << "\n";
    }
    if (pPrimeLimiterHits_ > 0) {
        std::cerr << "  WARNING: the p' magnitude limiter fired " << pPrimeLimiterHits_
                  << " time(s). It rescales p' uniformly, which imposes an effective\n"
                     "         alphaP=alphaU<1 and weakens the pressure/velocity coupling:\n"
                     "         continuity is no longer exactly satisfied at the end of a\n"
                     "         corrector. Raise simple.pressure_correction_limit or turn the\n"
                     "         limiter off if the run is well behaved.\n";
    }
}

template<std::size_t Dim>
void SIMPLE<Dim>::solve(VectorField<Dim>& velocity, Field<Dim>& pressure) {
    initializeSolve_(velocity, pressure);

    status_ = Status::NotConverged;

    for (std::size_t iter = startIteration_; iter < config_.maxIterations; ++iter) {
        if (cancelRequested_.load(std::memory_order_acquire)) {
            status_ = Status::Cancelled;
            break;
        }

        performIteration_(velocity, pressure, iter);

        updateResiduals_();

        lastIterations_ = static_cast<int>(iter + 1);

        printIterationOutput_(iter);

        if (config_.iterationCallback) {
            config_.iterationCallback(static_cast<int>(iter + 1), lastContResid_, lastMomResid_);
        }

        saveCheckpoint_(iter);

        if (checkConvergence_(iter)) {
            // Distinguish the three reasons the loop can end: the tolerance was
            // met, the run blew up, or the user cancelled it. Reporting "done"
            // for all three is what made an unconverged result look converged.
            if (aborted_) {
                status_ = Status::Aborted;
            } else if (cancelRequested_.load(std::memory_order_acquire)) {
                status_ = Status::Cancelled;
            } else {
                status_ = Status::Converged;
            }
            break;
        }
    }

    printMomentumSolverStats_();
    printFinalStats_();
}

template<std::size_t Dim>
const char* SIMPLE<Dim>::statusText() const {
    switch (status_) {
        case Status::Converged:   return "converged";
        case Status::Aborted:     return "aborted (diverged or non-finite residual)";
        case Status::Cancelled:   return "cancelled";
        case Status::NotConverged: break;
    }
    return "not converged (iteration limit reached)";
}

template<std::size_t Dim>
int SIMPLE<Dim>::lastIterations() const { return lastIterations_; }

template<std::size_t Dim>
double SIMPLE<Dim>::finalContinuityResidual() const { return lastContResid_; }

template<std::size_t Dim>
double SIMPLE<Dim>::finalMomentumResidual() const { return lastMomResid_; }

template<std::size_t Dim>
SSTModel<Dim>* SIMPLE<Dim>::turbulence() { return turbulence_.get(); }

template<std::size_t Dim>
const SSTModel<Dim>* SIMPLE<Dim>::turbulence() const { return turbulence_.get(); }

// (cancel control now inherited from Cancellable)

// --- Private method implementations ---

template<std::size_t Dim>
const Field<Dim>& SIMPLE<Dim>::zeroSource_() const {
    if (!zeroSourceInitialized_ || zeroSourceCache_.size() != mesh_.nCells()) {
        zeroSourceCache_ = Field<Dim>(mesh_, 0.0);
        zeroSourceInitialized_ = true;
    }
    return zeroSourceCache_;
}

template<std::size_t Dim>
void SIMPLE<Dim>::solveMomentum_(VectorField<Dim>& uStar, const VectorField<Dim>& velocityOld) {
    momentum_.computePressureGradient(boundPressure(), pressureGrad_);

    if constexpr (Dim == 2) {
        solveMomentumComponent_(0, uStar, velocityOld, pressureGrad_);
        solveMomentumComponent_(1, uStar, velocityOld, pressureGrad_);
    } else {
        solveMomentumComponent_(0, uStar, velocityOld, pressureGrad_);
        solveMomentumComponent_(1, uStar, velocityOld, pressureGrad_);
        solveMomentumComponent_(2, uStar, velocityOld, pressureGrad_);
    }
}

template<std::size_t Dim>
void SIMPLE<Dim>::solveMomentumComponent_(std::size_t d, VectorField<Dim>& uStar,
                                           const VectorField<Dim>& velocityOld,
                                           const std::array<Field<Dim>, Dim>& pressureGrad) {
    momTriplets_.clear();
    momTriplets_.reserve(mesh_.nCells() * (2 * Dim + 1));
    momB_.resize(mesh_.nCells());
    momB_.setZero();

    const Field<Dim>* nutPtr = turbulence_ ? &turbulence_->nut() : nullptr;

    gradientCalc_->compute((boundVelocity())[d], velGrad_);
    const std::array<Field<Dim>, Dim>* velGradPtr = &velGrad_;

    fvm::MomentumBCs momBCs;
    momBCs.slipWallMask = &bcDiscretization_.slipWallMask(d);
    momBCs.dirichletMaskAll = &bcDiscretization_.dirichletMask();
    momBCs.dirichletValsAll = &bcDiscretization_.dirichletVals();
    momBCs.faceBC = &bcDiscretization_.faceBCs();
    momBCs.faceHasBC = &bcDiscretization_.faceHasBCs();

    momentum_.assembleWithConvection(d, boundVelocity(), pressureGrad,
                                     zeroSource_(),
                                     bcDiscretization_.dirichletMask(d), bcDiscretization_.dirichletVals(d),
                                     momA_, momB_, momDiag_,
                                     config_.convectionScheme,
                                     nutPtr, velGradPtr,
                                     momBCs);

    diag_[d] = momDiag_;

    momentum_.computeDCoeffAndRelax(momA_, momB_, bcDiscretization_.dirichletMask(d),
                                    velocityOld[d], config_.alphaU,
                                    dCoeff_[d], config_.simplec);

    Eigen::VectorXd x = solveMomentumLinear_(d);

    // Snapshot this component's operator: momA_/momB_ are reused by the next
    // component, so the residual pass needs its own copy.
    momAsnap_[d] = momA_;
    momBsnap_[d] = momB_;

    for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
        double val = x(static_cast<Eigen::Index>(i));
        if (std::isnan(val) || std::isinf(val)) val = (boundVelocity())[d][i];
        if (bcDiscretization_.dirichletMask(d)[i]) val = bcDiscretization_.dirichletVals(d)[i];
        uStar[d][i] = val;
    }
}

template<std::size_t Dim>
void SIMPLE<Dim>::solveMomentumTransient_(VectorField<Dim>& uStar, const VectorField<Dim>& velocityOld) {
    momentum_.computePressureGradient(boundPressure(), pressureGrad_);
    if constexpr (Dim == 2) {
        solveMomentumComponentTransient_(0, uStar, velocityOld, pressureGrad_);
        solveMomentumComponentTransient_(1, uStar, velocityOld, pressureGrad_);
    } else {
        solveMomentumComponentTransient_(0, uStar, velocityOld, pressureGrad_);
        solveMomentumComponentTransient_(1, uStar, velocityOld, pressureGrad_);
        solveMomentumComponentTransient_(2, uStar, velocityOld, pressureGrad_);
    }
}

template<std::size_t Dim>
void SIMPLE<Dim>::solveMomentumComponentTransient_(std::size_t d, VectorField<Dim>& uStar,
                                                   const VectorField<Dim>& velocityOld,
                                                   const std::array<Field<Dim>, Dim>& pressureGrad) {
    momTriplets_.clear();
    momTriplets_.reserve(mesh_.nCells() * (2 * Dim + 1));
    momB_.resize(mesh_.nCells());
    momB_.setZero();
    const Field<Dim>* nutPtr = turbulence_ ? &turbulence_->nut() : nullptr;
    gradientCalc_->compute((boundVelocity())[d], velGrad_);
    const std::array<Field<Dim>, Dim>* velGradPtr = &velGrad_;

    const Field<Dim>* oldTime = &velocityN_[d];
    const Field<Dim>* oldTimePrev = nullptr;
    // BDF2 needs two levels of history. dtPrev_ == 0 additionally forces
    // explicit Euler for this step (handled inside the assembler), which is
    // what a restart from a fresh field must do.
    if (config_.transient.scheme == TimeScheme::BDF2 && timeStep_ > 1 && dtPrev_ > 0.0) {
        oldTimePrev = &velocityNM1_[d];
    }
    TimeScheme ts = config_.transient.scheme;
    if (timeStep_ == 1 && ts == TimeScheme::BDF2) ts = TimeScheme::Euler;

    fvm::MomentumBCs momBCs;
    momBCs.slipWallMask = &bcDiscretization_.slipWallMask(d);
    momBCs.dirichletMaskAll = &bcDiscretization_.dirichletMask();
    momBCs.dirichletValsAll = &bcDiscretization_.dirichletVals();
    momBCs.faceBC = &bcDiscretization_.faceBCs();
    momBCs.faceHasBC = &bcDiscretization_.faceHasBCs();

    momentum_.assembleWithConvectionTransient(d, boundVelocity(), pressureGrad,
                                             zeroSource_(),
                                             bcDiscretization_.dirichletMask(d), bcDiscretization_.dirichletVals(d),
                                             momA_, momB_, momDiag_,
                                             dt_, dtPrev_, oldTime, oldTimePrev, ts,
                                             config_.convectionScheme,
                                             nutPtr, velGradPtr,
                                             momBCs);
    diag_[d] = momDiag_;
    // PISO: no momentum under-relaxation (alphaU=1). PIMPLE/SIMPLE use config.alphaU.
    // solverType is explicit; legacy piso bool kept for compat.
    bool isPISO = (config_.solverType == SolverType::PISO) ||
                  (config_.transient.enabled && config_.transient.piso);
    double alphaU = isPISO ? 1.0 : config_.alphaU;
    momentum_.computeDCoeffAndRelax(momA_, momB_, bcDiscretization_.dirichletMask(d),
                                    velocityOld[d], alphaU,
                                    dCoeff_[d], config_.simplec);
    // Transient greatly increases diagonal dominance, keep pattern re-analysis cheap: still cache
    // For transient, matrix pattern identical, but values change with dt:
    // the linear-solver dispatch below re-factorizes / rebuilds as needed.
    Eigen::VectorXd x = solveMomentumLinear_(d);

    momAsnap_[d] = momA_;
    momBsnap_[d] = momB_;

    for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
        double val = x(static_cast<Eigen::Index>(i));
        if (std::isnan(val) || std::isinf(val)) val = (boundVelocity())[d][i];
        if (bcDiscretization_.dirichletMask(d)[i]) val = bcDiscretization_.dirichletVals(d)[i];
        uStar[d][i] = val;
    }
}

template<std::size_t Dim>
void SIMPLE<Dim>::updateMaxInletSpeed_() {
    // Must be re-evaluated whenever BCs change. Includes moving walls so a
    // lid-driven cavity does not fall back to an arbitrary 1.0 (which the
    // velocity clamp could then use to override the lid velocity itself).
    maxInletSpeed_ = bc_.maxPrescribedSpeed(1.0);
    if (maxInletSpeed_ < minDivisor) maxInletSpeed_ = 1.0;
}

template<std::size_t Dim>
bool SIMPLE<Dim>::limitPressureCorrection_(Field<Dim>& pPrime) {
    if (!config_.limitPressureCorrection) return false;

    double maxPP = 0.0;
    #pragma omp parallel for reduction(max:maxPP) schedule(static)
    for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
        maxPP = std::max(maxPP, std::abs(pPrime[i]));
    }

    const double pLimit =
        config_.pressureCorrectionLimit * rho_ * maxInletSpeed_ * maxInletSpeed_;
    if (!(maxPP > pLimit) || !(pLimit > minDivisor)) return false;

    // Scale EVERY cell, not just the interior ones. p' is Dirichlet-zero on
    // masked cells (walls / outlets) so they are unaffected by the factor, and
    // a partial scale would introduce a jump between a wall cell and its
    // interior neighbours that the Poisson operator never saw.
    const double scale = pLimit / maxPP;
    #pragma omp parallel for schedule(static)
    for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
        pPrime[i] *= scale;
    }
    ++pPrimeLimiterHits_;
    return true;
}

template<std::size_t Dim>
double SIMPLE<Dim>::computeAdaptiveDt_() const {
    // Cell size for the CFL estimate must be the *controlling* (smallest) cell
    // dimension, not the geometric mean. sqrt(V)/cbrt(V) over-estimates it on
    // stretched or anisotropic cells, so the advertised `cfl` value would be
    // quietly exceeded exactly where PISO needs it most. We use the
    // hydraulic size 2V/sum(face areas), which is <= the smallest cell
    // dimension, and still blend 70% average / 30% minimum so a single tiny
    // cell (e.g. in a cylinder boundary layer) does not collapse dt.
    const std::size_t n = mesh_.nCells();
    const std::size_t nF = mesh_.nFaces();

    std::vector<double> areaSum(n, 0.0);
    for (std::size_t f = 0; f < nF; ++f) {
        const double a = mesh_.faceArea(f);
        if (!(a > 0.0)) continue;
        areaSum[mesh_.owner(f)] += a;
        const std::size_t nb = mesh_.neighbor(f);
        if (nb != std::size_t(-1)) areaSum[nb] += a;
    }

    double minDx = std::numeric_limits<double>::max();
    double sumDx = 0.0;
    std::size_t count = 0;
    double maxVel = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (mesh_.isBoundary(i)) continue;
        double dx = (areaSum[i] > 0.0)
            ? 2.0 * mesh_.cellVolume(i) / areaSum[i]
            : ((Dim == 2) ? std::sqrt(mesh_.cellVolume(i)) : std::cbrt(mesh_.cellVolume(i)));
        if (dx < minDx) minDx = dx;
        sumDx += dx;
        ++count;
        double vel2 = 0.0;
        for (std::size_t d = 0; d < Dim; ++d) vel2 += (boundVelocity())[d][i] * (boundVelocity())[d][i];
        double vm = std::sqrt(vel2);
        if (vm > maxVel) maxVel = vm;
    }
    if (count == 0) return std::min(config_.transient.maxDt, std::max(config_.transient.dt, config_.transient.minDt));
    double avgDx = sumDx / static_cast<double>(count);
    // Blend: 70% avg, 30% min to avoid single tiny cell dominating; for uniform meshes min~avg
    double charDx = 0.7 * avgDx + 0.3 * minDx;
    if (charDx <= 1e-12) charDx = minDx;
    if (maxVel < 1e-12) return std::min(config_.transient.maxDt, std::max(config_.transient.dt, config_.transient.minDt));
    // Clamp maxVel to at least the prescribed driving speed so a stalled field
    // cannot let dt grow without bound.
    double velRef = std::max(maxVel, maxInletSpeed_);
    if (velRef < 1e-12) velRef = maxVel;
    double dtCFL = config_.transient.cfl * charDx / velRef;
    dtCFL = std::max(config_.transient.minDt, std::min(dtCFL, config_.transient.maxDt));
    return dtCFL;
}

template<std::size_t Dim>
void SIMPLE<Dim>::performTransientIteration_(VectorField<Dim>& velocity, Field<Dim>& pressure) {
    // PISO: no pressure under-relaxation. PIMPLE keeps SIMPLE relaxation.
    bool isPISO = (config_.solverType == SolverType::PISO) ||
                  (config_.transient.enabled && config_.transient.piso);
    if (isPISO) adaptiveAlphaP_ = 1.0;
    for (std::size_t d = 0; d < Dim; ++d)
        std::copy((boundVelocity())[d].values().begin(), (boundVelocity())[d].values().end(),
                  velocityOld_[d].values().begin());
    if (bc_.isDirty()) bcDiscretization_ = bc_.build();
    solveMomentumTransient_(uStar_, velocityOld_);
    pressureCorrection_.solve(uStar_, dCoeff_, boundPressure(), pPrime_,
                              bcDiscretization_.pressureOutletCells(), bcDiscretization_.isPressureNeumann(),
                              &bcDiscretization_.isPressureDirichletFaces(), &bcDiscretization_.pressureOutletFaces(),
                              &bcDiscretization_.faceBCs(), &bcDiscretization_.faceHasBCs(),
                              &bcDiscretization_.dirichletMask(), &bcDiscretization_.dirichletVals());
    limitPressureCorrection_(pPrime_);
    pressureBeforeCorrection_ = boundPressure();
    pressureGradBefore_ = pressureCorrection_.pressureGradient();
    correctVelocity(mesh_, uStar_, pPrime_, dCoeff_,
                    bcDiscretization_.dirichletMask(), bcDiscretization_.dirichletVals(), dpdx_, boundVelocity(),
                    &bcDiscretization_.faceBCs(), &bcDiscretization_.faceHasBCs());
    #pragma omp parallel for schedule(static)
    for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
        (boundPressure())[i] += adaptiveAlphaP_ * pPrime_[i];
    }
    if (!bcDiscretization_.pressureOutletFaces().empty()) {
        for (const auto& [face, pRef] : bcDiscretization_.pressureOutletFaces()) {
            if (face < mesh_.nFaces()) {
                std::size_t cell = mesh_.owner(face);
                (boundPressure())[cell] = pRef;
            }
        }
    } else {
        for (const auto& [cell, pRef] : bcDiscretization_.pressureOutletCells()) {
            (boundPressure())[cell] = pRef;
        }
    }
    if (turbulence_) {
        nutOld_ = turbulence_->nut();
        // Transient turbulence
        if (config_.transient.enabled && dt_ > 0) {
            TimeScheme ts = config_.transient.scheme;
            if (timeStep_ == 1 && ts == TimeScheme::BDF2) ts = TimeScheme::Euler;
            // Two levels of history are needed for BDF2; dtPrev_ == 0 (fresh
            // start or restart) makes the assembler fall back to Euler.
            const Field<Dim>* kPrevPrev =
                (timeStep_ > 1 && dtPrev_ > 0.0) ? &kNM1_ : nullptr;
            const Field<Dim>* oPrevPrev =
                (timeStep_ > 1 && dtPrev_ > 0.0) ? &omegaNM1_ : nullptr;
            // kN_/omegaN_ hold the previous time level, unchanged across the
            // outer correctors of this step.
            turbulence_->solveIterationTransient(boundVelocity(), config_.convectionScheme, dt_, dtPrev_, ts,
                                                kN_, omegaN_, kPrevPrev, oPrevPrev);
        } else {
            turbulence_->solveIteration(boundVelocity(), config_.convectionScheme);
        }
        double alphaNut = nutUnderRelax;
        auto& nut = turbulence_->nut();
        #pragma omp parallel for schedule(static)
        for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
            nut[i] = alphaNut * nut[i] + (1.0 - alphaNut) * nutOld_[i];
        }
    }
}

// Fluent-like PISO: frozen-A predictor + pressure correctors (robust, no momentum rebuild)
template<std::size_t Dim>
void SIMPLE<Dim>::performPISOPredictor_(VectorField<Dim>& velocity, Field<Dim>& pressure) {
    // Force Rhie-Chow for PISO (Fluent/StarCCM+ always use it on collocated grids)
    // and disable under-relaxation
    adaptiveAlphaP_ = 1.0;
    for (std::size_t d = 0; d < Dim; ++d)
        std::copy((boundVelocity())[d].values().begin(), (boundVelocity())[d].values().end(),
                  velocityOld_[d].values().begin());
    if (bc_.isDirty()) bcDiscretization_ = bc_.build();
    solveMomentumTransient_(uStar_, velocityOld_);
    // dCoeff_ now frozen for all correctors in this time step
}

template<std::size_t Dim>
void SIMPLE<Dim>::performPISOCorrector_(VectorField<Dim>& velocity, Field<Dim>& pressure) {
    // Pressure correction with frozen dCoeff - true PISO, no momentum rebuild (Fluent-like)
    // Use predicted uStar_ (frozen) for divU; after correction uStar_ is updated to corrected velocity for next loop
    pressureCorrection_.solve(uStar_, dCoeff_, boundPressure(), pPrime_,
                              bcDiscretization_.pressureOutletCells(), bcDiscretization_.isPressureNeumann(),
                              &bcDiscretization_.isPressureDirichletFaces(), &bcDiscretization_.pressureOutletFaces(),
                              &bcDiscretization_.faceBCs(), &bcDiscretization_.faceHasBCs(),
                              &bcDiscretization_.dirichletMask(), &bcDiscretization_.dirichletVals());
    limitPressureCorrection_(pPrime_);
    pressureBeforeCorrection_ = boundPressure();
    pressureGradBefore_ = pressureCorrection_.pressureGradient();
    // Correct velocity with frozen dCoeff - Fluent PISO uses predicted uStar
    correctVelocity(mesh_, uStar_, pPrime_, dCoeff_,
                    bcDiscretization_.dirichletMask(), bcDiscretization_.dirichletVals(), dpdx_, boundVelocity(),
                    &bcDiscretization_.faceBCs(), &bcDiscretization_.faceHasBCs());
    #pragma omp parallel for schedule(static)
    for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
        (boundPressure())[i] += pPrime_[i]; // alphaP=1 for PISO
    }
    if (!bcDiscretization_.pressureOutletFaces().empty()) {
        for (const auto& [face, pRef] : bcDiscretization_.pressureOutletFaces()) {
            if (face < mesh_.nFaces()) {
                std::size_t cell = mesh_.owner(face);
                (boundPressure())[cell] = pRef;
            }
        }
    } else {
        for (const auto& [cell, pRef] : bcDiscretization_.pressureOutletCells()) {
            (boundPressure())[cell] = pRef;
        }
    }
    // Prepare for next corrector: update uStar_ to corrected velocity
    uStar_ = boundVelocity();
}

template<std::size_t Dim>
void SIMPLE<Dim>::printTransientOutput_(int step, double time) {
    if (headerCounter_ % 25 == 0) printHeader_(true);
    ++headerCounter_;
    constexpr int wIter = 6;
    constexpr int wRes = 13;
    constexpr int wTime = 13;
    std::ostringstream line;
    line << std::setw(wIter) << step
         << std::scientific << std::setprecision(4)
         << std::setw(wTime) << time
         << std::setw(wTime) << dt_
         << std::setw(wRes) << lastContResid_
         << std::setw(wRes) << lastMomResidU_
         << std::setw(wRes) << lastMomResidV_;
    if constexpr (Dim == 3) line << std::setw(wRes) << lastMomResidW_;
    if (turbulence_) line << std::setw(wRes) << turbulence_->finalKResid()
                           << std::setw(wRes) << turbulence_->finalOmegaResid();
    std::cout << line.str() << std::endl;
}

template<std::size_t Dim>
void SIMPLE<Dim>::saveTransientCheckpoint_(int step, double time) {
    // Ship the full transient state (time, dt, dt/dtPrev ratio and the BDF2
    // history) so a restart can resume mid-run instead of replaying from t=0.
    TransientBlock<Dim> tb;
    tb.time = time;
    tb.dt = dt_;
    tb.dtPrev = dtPrev_;
    tb.step = static_cast<std::size_t>(step);
    tb.velocityN = &velocityN_;
    tb.velocityNM1 = &velocityNM1_;
    tb.kN = turbulence_ ? &kN_ : nullptr;
    tb.kNM1 = turbulence_ ? &kNM1_ : nullptr;
    tb.omegaN = turbulence_ ? &omegaN_ : nullptr;
    tb.omegaNM1 = turbulence_ ? &omegaNM1_ : nullptr;
    tb.pvdEntries = pvdEntries_;
    saveCheckpoint_(static_cast<std::size_t>(step), &tb);
}

template<std::size_t Dim>
void SIMPLE<Dim>::updatePVDFile_() {
    if (pvdEntries_.empty()) return;
    // The collection file and the .vtu files must live in the same directory,
    // otherwise ParaView resolves the bare filenames against the CWD and finds
    // nothing. Write the .pvd beside the .vtu, derived from the VTU base name.
    const std::size_t sep = pvdEntries_.back().find('|');
    const std::string lastFile =
        (sep == std::string::npos) ? std::string() : pvdEntries_.back().substr(sep + 1);

    std::string dir;
    const std::size_t slash = lastFile.find_last_of("/\\");
    if (slash != std::string::npos) dir = lastFile.substr(0, slash + 1);

    const std::string base =
        dir + (pvdBaseName_.empty() ? std::string("transient") : pvdBaseName_);

    std::string pvdFile = base + ".pvd";
    if (!config_.transient.pvdFile.empty()) {
        const std::string& want = config_.transient.pvdFile;
        const std::size_t wslash = want.find_last_of("/\\");
        if (wslash != std::string::npos) {
            pvdFile = want; // honour an explicit path from the config
        } else {
            pvdFile = dir + want;
        }
    }

    std::ofstream out(pvdFile);
    if (!out) {
        std::cerr << "  WARNING: cannot write PVD file '" << pvdFile << "'\n";
        return;
    }
    out << "<?xml version=\"1.0\"?>\n";
    out << "<VTKFile type=\"Collection\" version=\"0.1\">\n";
    out << "  <Collection>\n";
    for (size_t i = 0; i < pvdEntries_.size(); ++i) {
        // pvdEntries_ stores "time|file"
        auto pos = pvdEntries_[i].find('|');
        std::string t = (pos == std::string::npos) ? "0" : pvdEntries_[i].substr(0, pos);
        std::string f = (pos == std::string::npos) ? pvdEntries_[i] : pvdEntries_[i].substr(pos+1);
        out << "    <DataSet timestep=\"" << t << "\" file=\"" << f << "\"/>\n";
    }
    out << "  </Collection>\n";
    out << "</VTKFile>\n";
}

template<std::size_t Dim>
void SIMPLE<Dim>::solveTransient(VectorField<Dim>& velocity, Field<Dim>& pressure) {
    initializeSolve_(velocity, pressure);

    // --- Transient state -------------------------------------------------
    // initializeSolve_ may have loaded a checkpoint. If that checkpoint carried
    // transient state we resume at the stored time with the stored dt and
    // BDF2 history; otherwise we start a brand new run at t=0. Previously this
    // was hard-coded to t=0, so "restart" silently replayed the whole run on
    // top of a mid-run field and lost u^{n-1}.
    bool haveHistory = false;
    if (restarting_) {
        TransientBlock<Dim> tb;
        tb.velocityN = &velocityN_;
        tb.velocityNM1 = &velocityNM1_;
        tb.kN = turbulence_ ? &kN_ : nullptr;
        tb.kNM1 = turbulence_ ? &kNM1_ : nullptr;
        tb.omegaN = turbulence_ ? &omegaN_ : nullptr;
        tb.omegaNM1 = turbulence_ ? &omegaNM1_ : nullptr;
        if (Checkpoint::load(config_.checkpoint.restartFrom, mesh_,
                             startIteration_, lastContResid_, lastMomResid_,
                             adaptiveAlphaP_, stallCount_, prevContResid_,
                             velocity, pressure,
                             turbulence_ ? &turbulence_->k() : nullptr,
                             turbulence_ ? &turbulence_->omega() : nullptr,
                             turbulence_ ? &turbulence_->nut() : nullptr,
                             &tb)) {
            haveHistory = true;
            time_ = tb.time;
            timeStep_ = static_cast<int>(tb.step);
            dt_ = (tb.dt > 0.0) ? tb.dt : config_.transient.dt;
            dtPrev_ = tb.dtPrev;
            pvdEntries_ = std::move(tb.pvdEntries);
            std::cout << "  [transient restart] resuming at t=" << time_
                      << " step=" << timeStep_ << " dt=" << dt_
                      << " dtPrev=" << dtPrev_ << "\n";
        } else {
            std::cerr << "  [transient] checkpoint had no transient state"
                      << " — restarting from t=0\n";
        }
    }

    time_ = haveHistory ? time_ : 0.0;
    timeStep_ = haveHistory ? timeStep_ : 0;
    if (!haveHistory) {
        dtPrev_ = 0.0;
        dt_ = config_.transient.dt;
        if (dt_ <= 0) dt_ = 1e-3;
        velocityN_ = velocity;
        velocityNM1_ = velocity;
        pvdEntries_.clear();
    }

    // Clamp dt to the configured limits, keeping whatever we restored.
    dt_ = std::max(config_.transient.minDt, std::min(dt_, config_.transient.maxDt));
    dtPrev_ = std::max(config_.transient.minDt,
                       std::min(dtPrev_, config_.transient.maxDt));
    pressureN_ = pressure;
    // Determine base name for PVD from transient.pvdFile or default
    pvdBaseName_ = config_.transient.pvdFile;
    if (pvdBaseName_.empty()) pvdBaseName_ = "transient";
    // Remove extension if provided
    if (pvdBaseName_.size() > 4 && pvdBaseName_.substr(pvdBaseName_.size()-4) == ".pvd") {
        pvdBaseName_ = pvdBaseName_.substr(0, pvdBaseName_.size()-4);
    }
    if (turbulence_) {
        // On restart kN_/kNM1_ were restored from the checkpoint; only seed
        // them from the (also restored) field on a fresh run.
        if (!haveHistory) {
            kN_ = turbulence_->k();
            omegaN_ = turbulence_->omega();
            kNM1_ = kN_;
            omegaNM1_ = omegaN_;
        }
    } else {
        kN_ = Field<Dim>(mesh_, 0.0);
        omegaN_ = Field<Dim>(mesh_, 0.0);
        kNM1_ = Field<Dim>(mesh_, 0.0);
        omegaNM1_ = Field<Dim>(mesh_, 0.0);
    }
    int nSteps = 1;
    if (config_.transient.tEnd > 0 && dt_ > 0) {
        nSteps = static_cast<int>(std::ceil(config_.transient.tEnd / dt_));
        if (nSteps < 1) nSteps = 1;
    }
    // If adaptive, nSteps is just upper bound; we loop on time
    {
        std::string solverName = solverTypeToString(config_.solverType);
        // Legacy piso bool still honored for compat
        if (config_.transient.piso && config_.solverType==SolverType::SIMPLE) solverName="PISO";
        std::cout << "Transient " << solverName << ": dt=" << dt_ << " tEnd=" << config_.transient.tEnd
                  << " scheme=" << (config_.transient.scheme==TimeScheme::BDF2?"BDF2":"Euler")
                  << " adaptive=" << (config_.transient.adaptive?"yes":"no")
                  << " nOuter=" << config_.transient.maxOuterCorrectors
                  << (config_.simplec ? " SIMPLEC" : "") << "\n";
    }
    double tEnd = config_.transient.tEnd;
    int step = timeStep_;
    if (!haveHistory) time_ = 0.0;
    // For adaptive we loop while time < tEnd
    while (time_ < tEnd - 1e-12) {
        if (cancelRequested_.load(std::memory_order_acquire)) break;
        // A diverged run (NaN/inf or repeated residual growth) must stop, not
        // keep integrating garbage to tEnd while writing output.
        if (aborted_) break;

        if (config_.transient.adaptive && step > 0) {
            double newDt = computeAdaptiveDt_();
            // Limit change to 50% per step for stability (was 20%, too restrictive for shedding)
            newDt = std::max(0.5*dt_, std::min(newDt, 1.5*dt_));
            dt_ = newDt;
        }
        // Truncate the final step so we land exactly on tEnd. This must come
        // AFTER the adaptive limiter and must NOT be undone by the minDt clamp
        // below, or the run overshoots tEnd (and changes dt without telling the
        // BDF2 coefficients, which is exactly the inconsistency we fixed).
        if (time_ + dt_ > tEnd) dt_ = tEnd - time_;
        if (dt_ < config_.transient.minDt) dt_ = std::min(config_.transient.minDt, tEnd - time_);
        time_ += dt_;
        ++step;
        timeStep_ = step;
        // Outer correctors per time step - Fluent-like distinction
        bool isPISO = (config_.solverType == SolverType::PISO) ||
                      (config_.transient.enabled && config_.transient.piso);
        // PISO robustness: force Rhie-Chow (like Fluent/StarCCM+ on collocated grids)
        if (isPISO && !config_.rhieChow) {
            config_.rhieChow = true;
            applyPressureSolverConfig_();
        }
        int nOuter = std::max(1, config_.transient.maxOuterCorrectors);
        if (isPISO) {
            // True PISO: 1 momentum predictor + nOuter pressure correctors (frozen dCoeff)
            // No under-relaxation, no adaptive alphaP reduction inside correctors
            performPISOPredictor_(velocity, pressure);
            for (int corr = 0; corr < nOuter; ++corr) {
                if (cancelRequested_.load(std::memory_order_acquire)) break;
                performPISOCorrector_(velocity, pressure);
                updateResiduals_();
                lastIterations_ = step * nOuter + corr + 1;
                if (lastContResid_ < config_.tolerance && lastMomResid_ < config_.tolerance) break;
            }
            // checkConvergence_ detects NaN/inf divergence; act on it instead of
            // discarding the result and marching on to tEnd.
            if (checkConvergence_(static_cast<std::size_t>(step), false)) break;
            // Turbulence updated once per time step after PISO correctors (frozen during correctors)
            if (turbulence_ && config_.transient.enabled && dt_ > 0) {
                // One turbulence iteration with the corrected velocity
                TimeScheme ts = config_.transient.scheme;
                if (timeStep_ == 1 && ts == TimeScheme::BDF2) ts = TimeScheme::Euler;
                const Field<Dim>* kPrevPrev =
                    (timeStep_ > 1 && dtPrev_ > 0.0) ? &kNM1_ : nullptr;
                const Field<Dim>* oPrevPrev =
                    (timeStep_ > 1 && dtPrev_ > 0.0) ? &omegaNM1_ : nullptr;
                Field<Dim> dummyNut = turbulence_->nut();
                // Use current corrected velocity
                turbulence_->solveIterationTransient(velocity, config_.convectionScheme, dt_, dtPrev_, ts,
                                                     kN_, omegaN_, kPrevPrev, oPrevPrev);
                double alphaNut = nutUnderRelax;
                auto& nut = turbulence_->nut();
                #pragma omp parallel for schedule(static)
                for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
                    nut[i] = alphaNut * nut[i] + (1.0 - alphaNut) * dummyNut[i];
                }
            }
        } else {
            for (int outer = 0; outer < nOuter; ++outer) {
                if (cancelRequested_.load(std::memory_order_acquire)) break;
                performTransientIteration_(velocity, pressure);
                updateResiduals_();
                lastIterations_ = step * nOuter + outer + 1;
                if (lastContResid_ < config_.tolerance && lastMomResid_ < config_.tolerance) break;
            }
            if (checkConvergence_(static_cast<std::size_t>(step), false)) break;
        }
        // Roll the BDF2 history forward. dtPrev_ records the step size that
        // produced velocityN_, which is what the next step's BDF2 coefficients
        // are derived from.
        if (cancelRequested_.load(std::memory_order_acquire)) break;
        velocityNM1_ = velocityN_;
        velocityN_ = velocity;
        pressureN_ = pressure;
        dtPrev_ = dt_;
        if (turbulence_) {
            kNM1_ = kN_;
            omegaNM1_ = omegaN_;
            kN_ = turbulence_->k();
            omegaN_ = turbulence_->omega();
        }
        printTransientOutput_(step, time_);
        if (config_.iterationCallback) {
            config_.iterationCallback(step, lastContResid_, lastMomResid_);
        }
        saveTransientCheckpoint_(step, time_);
        // Transient VTK/PVD handling
        if (config_.transient.writeInterval > 0 && step % config_.transient.writeInterval == 0) {
            if (config_.transientCallback) {
                config_.transientCallback(step, time_, velocity, pressure);
            } else {
                std::string ext;
                if (mesh_.isUnstructured()) ext = ".vtu";
                else if (mesh_.isUniform()) ext = ".vti";
                else ext = ".vtr";
                std::string fname = pvdBaseName_ + "_" + std::to_string(step) + ext;
                std::array<const Field<Dim>*, Dim> velPtrs{};
                for (std::size_t d = 0; d < Dim; ++d) velPtrs[d] = &velocity[d];
                VTKXMLWriter<Dim>::writeWithVelocity(fname, mesh_, pressure, velPtrs, "Pressure");
                std::string entry = std::to_string(time_) + "|" + fname;
                pvdEntries_.push_back(entry);
                updatePVDFile_();
                std::cout << "  [transient] VTK written to: " << fname << " (pvd updated)\n";
                if (turbulence_) {
                    std::string turbFile = pvdBaseName_ + "_turb_" + std::to_string(step) + ext;
                    VTKXMLWriter<Dim>::writeTurbulence(turbFile, mesh_, turbulence_->k(), turbulence_->omega(), turbulence_->nut());
                }
            }
        }
        if (step >= 1000000) break; // safety
        if (!config_.transient.adaptive && step >= nSteps) break;
    }
    std::cout << "Transient solve complete: steps=" << step << " time=" << time_ << " dt=" << dt_ << "\n";
    if (pPrimeLimiterHits_ > 0) {
        std::cerr << "  WARNING: p' magnitude limiter fired " << pPrimeLimiterHits_
                  << " time(s). This imposes an effective alphaP=alphaU<1 and breaks the\n"
                  << "           PISO pressure/velocity coupling (continuity is not exactly\n"
                  << "           satisfied at the end of a corrector). Consider a smaller dt, more\n"
                  << "           correctors, or raising simple.pressure_correction_limit.\n";
    }
    printMomentumSolverStats_();
    printFinalStats_();
}

template class SIMPLE<2>;
template class SIMPLE<3>;

} // namespace fvm
