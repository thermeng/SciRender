#include "fvm/CFDSolver.h"

namespace fvm {

template<std::size_t Dim>
CFDSolver<Dim>::CFDSolver(const Mesh<Dim>& mesh, double rho, double nu, const Config& config)
    : mesh_(mesh),
      rho_(rho),
      nu_(nu),
      config_(config),
      momentum_(mesh, rho, nu),
      pressureCorrection_(mesh),
      pPrime_(mesh, 0.0),
      momDiag_(mesh, 0.0),
      uStar_(mesh, 0.0),
      bc_(mesh),
      linearSolverDispatcher_(mesh, [&]{
          typename fvm::LinearSolverDispatcher<Dim>::Config c;
          c.amgCycles = config_.momentumAmgCycles;
          c.amgAsSolver = config_.momentumAmgAsSolver;
          c.amgMaxResidual = config_.momentumAmgMaxResidual;
          c.solverStats = config_.momentumSolverStats;
          c.amgConfig = config_.momentumAmg;
          return c;
      }()) {}

template<std::size_t Dim>
CFDSolver<Dim>::~CFDSolver() = default;

template<std::size_t Dim>
void CFDSolver<Dim>::applyStoppingPolicy_() {
    auto resolved = cfd::resolveStoppingPolicy(
        config_.stopping, Dim == 3, config_.turbulence, config_.transient.enabled);
    if (resolved) {
        config_.stoppingPolicy = *resolved;
    } else {
        // Invalid configuration (e.g. steady residual mode with no active gate
        // and no fixed budget): refuse to run by setting a zero iteration limit.
        config_.stoppingPolicy = cfd::ResolvedStoppingPolicy{};
    }
}

template<std::size_t Dim>
bool CFDSolver<Dim>::isPiso_() const {
    return (config_.solverType == SolverType::PISO) ||
           (config_.transient.enabled && config_.transient.piso);
}

template<std::size_t Dim>
void CFDSolver<Dim>::applyPressureSolverConfig_() {
    typename PressureCorrection<Dim>::Config pcCfg;
    pcCfg.useAMGPreconditioner = config_.pressureUseAMGPreconditioner;
    pcCfg.pressureAmg = config_.pressureAmg;
    pressureCorrection_.setConfig(pcCfg);
    pressureCorrection_.setPressureGradient(pressureGradientCalc_.get());
}

template<std::size_t Dim>
void CFDSolver<Dim>::bindVelocity(VectorField<Dim>& velocity) {
    velocity_.emplace(std::ref(velocity));
}

template<std::size_t Dim>
void CFDSolver<Dim>::bindPressure(Field<Dim>& pressure) {
    pressure_.emplace(std::ref(pressure));
}

template<std::size_t Dim>
VectorField<Dim>& CFDSolver<Dim>::boundVelocity() {
    return velocity_->get();
}

template<std::size_t Dim>
const VectorField<Dim>& CFDSolver<Dim>::boundVelocity() const {
    return velocity_->get();
}

template<std::size_t Dim>
Field<Dim>& CFDSolver<Dim>::boundPressure() {
    return pressure_->get();
}

template<std::size_t Dim>
const Field<Dim>& CFDSolver<Dim>::boundPressure() const {
    return pressure_->get();
}

template<std::size_t Dim>
void CFDSolver<Dim>::initializeSolve_(VectorField<Dim>& velocity, Field<Dim>& pressure) {
    bindVelocity(velocity);
    bindPressure(pressure);
    faceVolFlux_.assign(mesh_.nFaces(), 0.0);
    useProvisionalFlux_ = false;
    for (auto& g : facePressureGrad_) {
        if (g.size() != mesh_.nCells()) g = Field<Dim>(mesh_, 0.0);
    }

    lastIterations_ = 0;
    lastContResid_ = 0.0;
    lastContResidRaw_ = 0.0;
    lastContResidPost_ = 0.0;
    predictedContinuity_ = 0.0;
    contResidRef_ = 0.0;
    contResidRefSamples_ = 0;
    momResidRaw_.fill(0.0);
    momResidRef_.fill(0.0);
    momResidRefSamples_ = 0;
    lastMomResid_ = 0.0;
    lastMomResidU_ = 0.0;
    lastMomResidV_ = 0.0;
    lastMomResidW_ = 0.0;
    lastKResid_ = 0.0;
    lastOmegaResid_ = 0.0;
    kResidRef_ = 0.0;
    omegaResidRef_ = 0.0;
    turbResidRefSamples_ = 0;
    aborted_ = false;
    failure_ = Failure::None;
    const double alphaP = isPiso_() ? 1.0 : config_.alphaP;
    headerCounter_ = 0;
    // Cleared at the start of every run; only the checkpoint load below may
    // set it again. Leaving it set from a previous run made solveTransient()
    // re-read a checkpoint that no longer described this run.
    restarting_ = false;

    updateMaxInletSpeed_();

    divBuffer_ = Field<Dim>(mesh_, 0.0);

    for (std::size_t d = 0; d < Dim; ++d) {
        dpdx_[d] = Field<Dim>(mesh_, 0.0);
    }

    gradientCalc_ = std::make_unique<Gradient<Dim>>(mesh_);
    gradientCalc_->setScheme(config_.gradientScheme);
    // Arm the user-configured slope limiter on the gradient operators; the
    // boundness is a property of the scheme, not of the dimension or of the
    // structured/unstructured topology, so it applies to every Dim.
    gradientCalc_->setLimitGradients(true);
    // Arm the user-configured slope limiter, so the FVM limiter selected in
    // the UI (MinMod / Venkatakrishnan / Cubic) is the one applied - not the
    // hardcoded default.
    gradientCalc_->setLimiter(fvm::SlopeLimiterConfig{
        static_cast<fvm::LimiterKind>(config_.limiterKind),
        static_cast<fvm::LimiterAggregation>(config_.limiterAggregation),
        static_cast<fvm::LimiterSpan>(config_.limiterSpan),
        config_.limiterK,
        config_.cubicTransition});
    for (auto& g : pressureGrad_) g = Field<Dim>(mesh_, 0.0);

    // The compiled boundary view is built on first use below, before the
    // turbulence model; its k/omega masks come from the real per-face BCs.

    if (config_.turbulence) {
        typename SSTModel<Dim>::Config sstCfg;
        sstCfg.kInf       = config_.kInf;
        sstCfg.omegaInf   = config_.omegaInf;
        sstCfg.alphaK     = config_.alphaK;
        sstCfg.alphaOmega = config_.alphaOmega;
        sstCfg.wallFunction = config_.wallFunction;
        sstCfg.turbulenceAmgConfig = config_.turbulenceAmg;
        sstCfg.turbulenceAmgCycles = config_.turbulenceAmgCycles;
        sstCfg.turbulenceAmgMaxResidual = config_.turbulenceAmgMaxResidual;
        sstCfg.limiter = fvm::SlopeLimiterConfig{
            static_cast<fvm::LimiterKind>(config_.limiterKind),
            static_cast<fvm::LimiterAggregation>(config_.limiterAggregation),
            static_cast<fvm::LimiterSpan>(config_.limiterSpan),
            config_.limiterK,
            config_.cubicTransition};
        if (!turbulence_) {
            turbulence_ = std::make_unique<SSTModel<Dim>>(mesh_, sstCfg);
        } else {
            // Reuse the instance created by enableTurbulence(), so any pointer
            // the caller cached stays valid for the whole run.
            turbulence_->setConfig(sstCfg);
        }
        turbulence_->initialize(nu_ * rho_, rho_, maxInletSpeed_);
        turbulence_->setBoundaryConditions(bcDiscretization().faceBCs());
        for (const auto& it : config_.inletTurbulence) {
            // Prefer direct face indices (face-index-based inlet BCs). Fall
            // back to patch-name lookup for legacy patch-based configuration.
            if (!it.faces.empty()) {
                turbulence_->setInletTurbulence(it.faces, it.k, it.omega);
                continue;
            }
            auto git = mesh_.boundaryGroups().find(it.patch);
            if (git == mesh_.boundaryGroups().end()) {
                std::cerr << "  [turbulence] WARNING: inlet patch '" << it.patch
                          << "' not found; k/omega inlet values not applied\n";
                continue;
            }
            turbulence_->setInletTurbulence(git->second, it.k, it.omega);
        }
    }

    // Checkpoint load. It MUST run after the turbulence model exists, or a
    // steady restart silently drops k/omega/nut and reseeds them from
    // kInf/omegaInf.
    if (!config_.checkpoint.restartFrom.empty()) {
        std::size_t savedIter = 0;
        double savedCont = 0.0, savedMom = 0.0;

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
                             velocity, pressure,
                             kPtr, omegaPtr, nutPtr)) {
            startIteration_ = savedIter;
            restarting_ = true;
            lastContResid_ = std::numeric_limits<double>::max();
            lastMomResid_ = std::numeric_limits<double>::max();
            if (turbulence_) turbulence_->clampFields();
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

    // Apply a queued restart payload. This must happen HERE, after
    // turbulence_ has been constructed: restoring k/omega/nut before that
    // point would let initialize() overwrite them with kInf/omegaInf.
    if (restartState_) {
        const RestartState& rs = *restartState_;
        const std::size_t n = mesh_.nCells();
        if (rs.hasVelocity) {
            for (std::size_t d = 0; d < Dim; ++d) {
                const std::size_t m = std::min(n, rs.velocity[d].size());
                for (std::size_t i = 0; i < m; ++i) velocity[d][i] = rs.velocity[d][i];
            }
            const std::size_t mp = std::min(n, rs.pressure.size());
            for (std::size_t i = 0; i < mp; ++i) pressure[i] = rs.pressure[i];
        }
        if (turbulence_ && rs.hasTurbulence) {
            const std::size_t mk = std::min(n, rs.k.size());
            const std::size_t mo = std::min(n, rs.omega.size());
            for (std::size_t i = 0; i < mk; ++i) turbulence_->k()[i] = rs.k[i];
            for (std::size_t i = 0; i < mo; ++i) turbulence_->omega()[i] = rs.omega[i];
            // A snapshot may predate the current bounds (or be corrupt). Clamp
            // the interior and re-derive nut from k/omega: a restored nut that
            // disagrees with them would make the first momentum solve
            // inconsistent with the turbulence equations.
            turbulence_->clampFields();
        }
        if (rs.hasTransient) {
            transientDriver_->time_ = rs.time;
            transientDriver_->dt_ = rs.dt;
            transientDriver_->dtPrev_ = rs.dtPrev;
            timeStep_ = rs.step;
            hasRestoredTransientState_ = true;
        }
        if (rs.hasVelocityHistory) {
            transientDriver_->velocityNM1_ = VectorField<Dim>(mesh_, 0.0);
            for (std::size_t d = 0; d < Dim; ++d) {
                const std::size_t m = std::min(n, rs.velocityNM1[d].size());
                for (std::size_t i = 0; i < m; ++i) transientDriver_->velocityNM1_[d][i] = rs.velocityNM1[d][i];
            }
            hasVelocityHistory_ = true;
        }
        if (rs.hasTurbulenceHistory) {
            transientDriver_->kNM1_ = Field<Dim>(mesh_, 0.0);
            transientDriver_->omegaNM1_ = Field<Dim>(mesh_, 0.0);
            const std::size_t mk = std::min(n, rs.kNM1.size());
            const std::size_t mo = std::min(n, rs.omegaNM1.size());
            for (std::size_t i = 0; i < mk; ++i) transientDriver_->kNM1_[i] = rs.kNM1[i];
            for (std::size_t i = 0; i < mo; ++i) transientDriver_->omegaNM1_[i] = rs.omegaNM1[i];
        }
        restartState_.reset();
    } else {
        // No queued restart payload: this solver object starts a fresh run.
        // Without this reset a second solve()/solveTransient() call on the same
        // object would inherit the previous run's clock and BDF2 history.
        // Transient time/BDF2 state lives on TransientDriver and is reset
        // there (TransientDriver::run).
        timeStep_ = 0;
        hasRestoredTransientState_ = false;
        hasVelocityHistory_ = false;
    }

    velocityOld_ = VectorField<Dim>(mesh_, 0.0);
    nutOld_ = Field<Dim>(mesh_, 0.0);
    initialMassImbalance_ = 0.0;
    initialMassImbalanceSet_ = false;
    residualHistory_.clear();
    steadyTotalSeconds_ = 0.0;
    steadyMomentumSeconds_ = 0.0;
    steadyMomentumAssemblySeconds_ = 0.0;
    steadyPredictedContinuitySeconds_ = 0.0;
    steadyPressureSeconds_ = 0.0;
    steadyResidualSeconds_ = 0.0;
    steadyTurbulenceSeconds_ = 0.0;
    divergenceCount_ = 0;
    pPrimeLimiterHits_ = 0;
}

template<std::size_t Dim>
double CFDSolver<Dim>::momentumResidual_(std::size_t d) const {
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

    Eigen::Map<const Eigen::VectorXd> uvec(u.data(), n);
    Eigen::VectorXd r = A * uvec - b;
    if (b.norm() < 1e-30) return 0.0;
    double num = r.cwiseAbs().sum();
    double den = b.cwiseAbs().sum();
    return den > 1e-30 ? num / den : num;
}

template<std::size_t Dim>
void CFDSolver<Dim>::computeProvisionalFaceFlux_() {
    const auto& bc = bcDiscretization();
    const std::size_t nF = mesh_.nFaces();
    if (faceVolFlux_.size() != nF) faceVolFlux_.assign(nF, 0.0);
    // The pressure mask is only populated once PressureCorrection::solve() has
    // run. Until then there is no Rhie-Chow term (the first predictor also has
    // p = 0), so skip it instead of indexing an empty vector.
    const bool rhieChowActive = pressureCorrection_.pressureMask().size() == mesh_.nCells();
    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = mesh_.owner(f);
        const std::size_t N = mesh_.neighbor(f);
        const double* fn = mesh_.faceNormalPtr(f);
        const double area = mesh_.faceArea(f);
        if (N != kNoNeighbor) {
            const double w = mesh_.faceWeight(f);
            double un = dotNormal(
                interpolateFaceVelocity(boundVelocity(), P, N, w,
                                        &bc.dirichletMask(), &bc.dirichletVals()), fn);
            if (rhieChowActive) {
                un += rhieChowCorrection<Dim>(mesh_, f, dCoeff_, boundPressure(),
                                              pressureGrad_, pressureCorrection_.pressureMask());
            }
            faceVolFlux_[f] = un * area;
        } else {
            // Boundary: use the same policy as the momentum assembly. A face
            // with a BC entry uses boundaryFaceState (NoSlip -> 0); a face
            // without one uses the owner value. The outlet pressure term is a
            // pressure-correction boundary term and is added separately by
            // PressureCorrection::solve and the continuity residual.
            const bool hasFaceEntry = f < bc.faceBCs().size();
            double un = 0.0;
            if (hasFaceEntry) {
                un = boundaryFaceState<Dim>(boundVelocity(), P, fn,
                                            bc.faceBCs()[f]).normalVelocity;
            } else {
                for (std::size_t d = 0; d < Dim; ++d) {
                    double uFace = boundVelocity()[d][P];
                    prescribedValue(bc.dirichletMask(), bc.dirichletVals(), d, P, uFace);
                    un += uFace * fn[d];
                }
            }
            faceVolFlux_[f] = un * area;
        }
    }
}

template<std::size_t Dim>
void CFDSolver<Dim>::computeFaceBasedPressureGradient_() {
    const auto& bc = bcDiscretization();
    for (auto& g : facePressureGrad_) g.assign(0.0);
    for (std::size_t f = 0; f < mesh_.nFaces(); ++f) {
        const std::size_t P = mesh_.owner(f);
        const std::size_t N = mesh_.neighbor(f);
        const double* fn = mesh_.faceNormalPtr(f);
        const double area = mesh_.faceArea(f);
        double pFace;
        if (N == kNoNeighbor) {
            // Only pressure outlets carry a prescribed pressure; velocity
            // inlets and walls are Neumann for pressure.
            if (bc.isPressureDirichletFace(f)) pFace = bc.faceBCs()[f].pressureValue;
            else pFace = boundPressure()[P];
        } else {
            const double w = mesh_.faceWeight(f);
            pFace = (1.0 - w) * boundPressure()[P] + w * boundPressure()[N];
        }
        for (std::size_t d = 0; d < Dim; ++d) {
            const double contrib = pFace * fn[d] * area;
            facePressureGrad_[d][P] += contrib / mesh_.cellVolume(P);
            if (N != kNoNeighbor)
                facePressureGrad_[d][N] -= contrib / mesh_.cellVolume(N);
        }
    }
}

template<std::size_t Dim>
void CFDSolver<Dim>::capturePredictedContinuity_() {
    pressureCorrection_.refreshPressureGradient(boundPressure());
    ResidualResult r = computeResiduals(
        mesh_, uStar_, velocityOld_, diag_, divBuffer_,
        bcDiscretization().dirichletMask(), bcDiscretization().dirichletVals(),
        &bcDiscretization().faceBCs(),
        &dCoeff_,
        &boundPressure(),
        &pressureCorrection_.pressureGradient(),
        &pressureCorrection_.pressureMask(),
        useProvisionalFlux_ ? &faceVolFlux_ : nullptr,
        static_cast<const Field<Dim>*>(nullptr));
    predictedContinuity_ = r.continuity;
}

template<std::size_t Dim>
void CFDSolver<Dim>::updateResiduals_() {
    // Post-correction continuity: the velocity is u = u* - d grad p', so the
    // Rhie-Chow conversion must use p' and grad p' (not the pre-correction
    // pressure) for the residual to equal div(u*) + A p' = 0 exactly. This is
    // the whole point of the compact SPD operator: the continuity the solver
    // minimizes is the continuity this residual reports.
    ResidualResult resid = computeResiduals(mesh_, boundVelocity(), velocityOld_, diag_,
                                            divBuffer_, bcDiscretization().dirichletMask(), bcDiscretization().dirichletVals(),
                                            &bcDiscretization().faceBCs(),
                                            &dCoeff_, &pPrime_, &dpdx_, &pressureCorrection_.pressureMask(),
                                            useProvisionalFlux_ ? &faceVolFlux_ : nullptr,
                                            useProvisionalFlux_ ? &pPrime_ : nullptr);
    lastContResidPost_ = resid.continuity;
    // Steady reports the predictor imbalance (the value the pressure correction
    // is about to reduce). PISO reports the imbalance of the corrector that
    // just ran, so the reduction test and the residual monitor both see the
    // value that correction was built on.
    lastContResidRaw_ = isPiso_() ? lastPisoPreCorrectorResidual_
                                  : predictedContinuity_;
    if (!initialMassImbalanceSet_) {
        initialMassImbalance_ = predictedContinuity_;
        initialMassImbalanceSet_ = true;
    }

    for (std::size_t d = 0; d < Dim; ++d) {
        momResidRaw_[d] = momentumResidual_(d);
    }
    
    const std::size_t window =
        std::max<std::size_t>(1, config_.residualNormalizationIterations);
    ++contResidRefSamples_;
    if (contResidRefSamples_ <= window) {
        contResidRef_ = std::max(contResidRef_, lastContResidRaw_);
    }
    ++momResidRefSamples_;
    if (momResidRefSamples_ <= window) {
        for (std::size_t d = 0; d < Dim; ++d) {
            momResidRef_[d] = std::max(momResidRef_[d], momResidRaw_[d]);
        }
    }
    const auto scaled = [](double raw, double ref) {
        if (ref > 0.0) return std::isfinite(ref) ? raw / ref : raw;
        return 1.0;
    };
    if (config_.normalizeResiduals) {
        lastContResid_ = scaled(lastContResidRaw_, contResidRef_);
        lastMomResidU_ = scaled(momResidRaw_[0], momResidRef_[0]);
        lastMomResidV_ = (Dim >= 2) ? scaled(momResidRaw_[1], momResidRef_[1]) : 0.0;
        lastMomResidW_ = (Dim >= 3) ? scaled(momResidRaw_[2], momResidRef_[2]) : 0.0;
    } else {
        lastContResid_ = lastContResidRaw_;
        lastMomResidU_ = momResidRaw_[0];
        lastMomResidV_ = (Dim >= 2) ? momResidRaw_[1] : 0.0;
        lastMomResidW_ = (Dim >= 3) ? momResidRaw_[2] : 0.0;
    }
    if (std::getenv("CFDSOLVER_TRACE_RESID")) {
        std::cerr << "  [trace] contRaw=" << lastContResidRaw_
                  << " cont=" << lastContResid_
                  << " contPost=" << lastContResidPost_
                  << " momLin=" << momResidRaw_[0] << "\n";
    }

    lastMomResid_ = std::max(lastMomResidU_, lastMomResidV_);
    if constexpr (Dim == 3) {
        lastMomResid_ = std::max(lastMomResid_, lastMomResidW_);
    }
    lastMomChange_ = std::max(resid.momU, resid.momV);
    if constexpr (Dim == 3) lastMomChange_ = std::max(lastMomChange_, resid.momW);
    lastMomChangeU_ = resid.momU;
    lastMomChangeV_ = resid.momV;
    if constexpr (Dim == 3) lastMomChangeW_ = resid.momW;

    // A non-finite turbulence residual means the k/omega solve produced
    // garbage. Treat it like a diverged flow residual instead of letting the
    // run continue with NaN fields.
    if (turbulence_) {
        const double rawK = turbulence_->finalKResid();
        const double rawO = turbulence_->finalOmegaResid();
        if (!std::isfinite(rawK) || !std::isfinite(rawO)) {
            fail_(Failure::NonFiniteTurbulence);
        }
        ++turbResidRefSamples_;
        if (turbResidRefSamples_ <= window) {
            kResidRef_ = std::max(kResidRef_, rawK);
            omegaResidRef_ = std::max(omegaResidRef_, rawO);
        }
        if (config_.normalizeResiduals) {
            lastKResid_ = scaled(rawK, kResidRef_);
            lastOmegaResid_ = scaled(rawO, omegaResidRef_);
        } else {
            lastKResid_ = rawK;
            lastOmegaResid_ = rawO;
        }
    }

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
        } else {
            divergenceCount_ = 0;
        }
    residualHistory_.clear();
}
}

template<std::size_t Dim>
bool CFDSolver<Dim>::checkConvergence_(std::size_t iter, bool checkTolerance,
                                     bool checkCancel) {
    (void)iter; // kept for call-site symmetry; the failure carries no iteration
    // Any check that already declared a hard failure ends the run on this
    // iteration: without this, a breakdown detected during the iteration
    // (e.g. a non-finite linear solve) was only reported when some other
    // condition happened to stop the loop.
    if (aborted_) return true;
    if (std::isnan(lastContResid_) || std::isinf(lastContResid_)) {
        fail_(Failure::NonFiniteContinuity);
        return true;
    }
    if (divergenceCount_ >= divergenceAbortCount) {
        fail_(Failure::DivergingContinuity);
        return true;
    }
    if (checkCancel && cancelRequested_.load(std::memory_order_acquire)) {
        return true;
    }
        if (checkTolerance && config_.stoppingPolicy.steady.residualGateEnabled &&
            convergenceMonitor_) {
        // Per-equation residuals must each be below their own tolerance for
        // `convergenceHoldIterations` consecutive iterations. A single lucky
        // iteration is not a steady state. Momentum W is only consulted in 3D,
        // k/omega only when turbulence exists; the policy already filtered
        // those out, so we pass nullopt for the absent ones.
        std::optional<double> momW, kResid, omegaResid;
        if constexpr (Dim == 3) momW = lastMomResidW_;
        if (turbulence_) {
            kResid = lastKResid_;
            omegaResid = lastOmegaResid_;
        }
        if (convergenceMonitor_->update(lastContResid_, lastMomResidU_,
                                        lastMomResidV_, momW, kResid,
                                        omegaResid)) {
            return true;
        }
    }
    return false;
}

template<std::size_t Dim>
void CFDSolver<Dim>::performIteration_(VectorField<Dim>& velocity, Field<Dim>& pressure, std::size_t iter) {
    const double alphaP = isPiso_() ? 1.0 : config_.alphaP;
    for (std::size_t d = 0; d < Dim; ++d)
        std::copy((boundVelocity())[d].values().begin(), (boundVelocity())[d].values().end(),
                  velocityOld_[d].values().begin());

    auto tMomentum = std::chrono::steady_clock::now();
    pressureGradientCalc_->compute(boundPressure(), pressureGrad_);
    computeFaceBasedPressureGradient_();
    // The provisional flux must be built BEFORE the momentum predictor, so the
    // convection uses the pure velocity flux. computeProvisionalFaceFlux_() adds
    // the Rhie-Chow pressure term to faceVolFlux_, so passing it into the
    // assembly would couple pressure into the momentum operator beyond the
    // explicit -Omega*(dp/dx) source -- a mass/momentum inconsistency.
    computeProvisionalFaceFlux_();
    useProvisionalFlux_ = true;
    const std::vector<double>* provisionalFlux = &faceVolFlux_;
    for (std::size_t d = 0; d < Dim; ++d)
        solveMomentumComponent_(d, uStar_, velocityOld_, pressureGrad_, provisionalFlux);
    steadyMomentumSeconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - tMomentum).count();

    auto tPred = std::chrono::steady_clock::now();
    capturePredictedContinuity_();
    steadyPredictedContinuitySeconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - tPred).count();

    int nCorr = static_cast<int>(config_.pressureCorrectors);
    if (nCorr < 1) nCorr = 1;
    for (int corr = 0; corr < nCorr; ++corr) {
        if (corr > 0 && useProvisionalFlux_) {
            // Non-orthogonal corrector: rebuild the provisional Rhie-Chow flux
            // from the corrected velocity and pressure before the next solve.
            pressureGradientCalc_->compute(boundPressure(), pressureGrad_);
            computeFaceBasedPressureGradient_();
            computeProvisionalFaceFlux_();
        }
        auto tPressure = std::chrono::steady_clock::now();
        applyPressureCorrection_(uStar_, alphaP, /*applyLimiter=*/true, /*capturePreCorrector=*/false,
                                 provisionalFlux);
        steadyPressureSeconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - tPressure).count();

        uStar_ = boundVelocity();
    }

    if (turbulence_) {
        auto tTurb = std::chrono::steady_clock::now();
        nutOld_ = turbulence_->nut();
        turbulence_->solveIteration(boundVelocity(), config_.convectionScheme);
        steadyTurbulenceSeconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - tTurb).count();
        relaxNut_(nutOld_);
    }
}

template<std::size_t Dim>
void CFDSolver<Dim>::relaxNut_(const Field<Dim>& previous, double alpha) {
    auto& nut = turbulence_->nut();
    #pragma omp parallel for schedule(static)
    for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
        nut[i] = alpha * nut[i] + (1.0 - alpha) * previous[i];
    }
}

template <std::size_t Dim>
void CFDSolver<Dim>::printPressureSolverStats_() const {
    const auto& s = pressureCorrection_.pressureStats();
    std::cerr << "\n  Pressure linear solver statistics\n";
    std::cerr << "    solves:            " << s.solves << "\n";
    std::cerr << "    AMG solves:        " << s.amgSolves << "\n";
    std::cerr << "    AMG builds:        " << s.amgBuilds << "\n";
    std::cerr << "    Krylov iters:      " << s.krylovIters << "\n";
    std::cerr << "    AMG Krylov iters:  " << s.amgKrylovIters << "\n";
    if (s.solves > 0) {
        std::cerr << "    Krylov iters/solve:"
                  << "  " << (static_cast<double>(s.krylovIters) /
                              static_cast<double>(s.solves)) << "\n";
    }
    std::cerr << "    AMG build time:    " << s.buildSeconds << " s\n";
    std::cerr << "    AMG solve time:    " << s.amgSeconds << " s\n";
    if (pressureCorrection_.amgBuilt()) {
        std::cerr << "    pressure AMG levels: "
                  << pressureCorrection_.amgLevels() << " (";
        for (int size : pressureCorrection_.amgLevelSizes()) std::cerr << size << " ";
        std::cerr << ")\n";
    }
}

template<std::size_t Dim>
void CFDSolver<Dim>::printHeader_(bool isTransient) const {
    constexpr int wIter = 6;
    constexpr int wRes = 13;
    constexpr int wTime = 13;
    int nCols = 1; // iter/step
    if (isTransient) nCols += 2; // time, dt
    nCols += 1; // continuity
    nCols += Dim; // velocities
    if (turbulence_) nCols += 2;
    if (config_.printResidentSet) nCols += 1; // resident set size
    int width = wIter + nCols * wRes;
    if (width < 78) width = 78;
    std::ostringstream hdr;
    hdr << std::string(width, '-') << "\n";
    if (isTransient) {
        hdr << std::setw(wIter) << "step"
            << std::setw(wTime) << "time"
            << std::setw(wTime) << "dt"
            << std::setw(wRes) << "continuity"
            << std::setw(wRes) << "x-mom"
            << std::setw(wRes) << "y-mom";
        if constexpr (Dim == 3) hdr << std::setw(wRes) << "z-mom";
        if (turbulence_) hdr << std::setw(wRes) << "k" << std::setw(wRes) << "omega";
        if (config_.printResidentSet) hdr << std::setw(wRes) << "mem(MiB)";
        hdr << "\n" << std::string(width, '-') ;
    } else {
        hdr << std::setw(wIter) << "iter"
            << std::setw(wRes) << "continuity"
            << std::setw(wRes) << "x-mom"
            << std::setw(wRes) << "y-mom";
        if constexpr (Dim == 3) hdr << std::setw(wRes) << "z-mom";
        if (turbulence_) hdr << std::setw(wRes) << "k" << std::setw(wRes) << "omega";
        if (config_.printResidentSet) hdr << std::setw(wRes) << "mem(MiB)";
        if (config_.printIterationTime) hdr << std::setw(wTime) << "time/iter";
        hdr << "\n" << std::string(width, '-') ;
    }
    std::cout << hdr.str() << std::endl;
}

template<std::size_t Dim>
void CFDSolver<Dim>::printFinalStats_(bool isTransient) const {
    double maxU = 0.0, maxV = 0.0;
    for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
        if (mesh_.isBoundary(i)) continue;
        maxU = std::max(maxU, std::abs((boundVelocity())[0][i]));
        maxV = std::max(maxV, std::abs((boundVelocity())[1][i]));
    }
    // The verdict comes first: "max|u|" alone says nothing about whether the
    // run met its stop criterion. Report the graded quantities (scaled, same as
    // the iteration table) next to the un-normalized values.
    std::cerr << "  [final] status: " << statusText()
              << "  (iterations=" << lastIterations_
              << ", continuity=" << lastContResid_
              << " normalized, " << lastContResidRaw_ << " raw, "
              << lastContResidPost_ << " mass imbalance after correction)\n";
    std::cerr << "  [final] scaled residuals: continuity=" << lastContResid_
              << " momentum=" << lastMomResid_
              << " (u=" << lastMomResidU_ << " v=" << lastMomResidV_;
    if constexpr (Dim == 3) std::cerr << " w=" << lastMomResidW_;
    std::cerr << ") velocityChange=" << lastMomChange_;
    if (!isTransient) {
        std::cerr << " tolerance="
                  << (config_.stoppingPolicy.steady.effectiveTolerances.empty()
                          ? config_.stopping.defaultTolerance
                          : config_.stoppingPolicy.steady.effectiveTolerances.front());
    }
    std::cerr << "\n";
    if (aborted_) {
        std::cerr << "  [final] ABORTED: " << failureText() << "\n";
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
    if (turbulence_ && turbulence_->maxYPlus() > 5.0) {
        std::cerr << "  WARNING: max y+ = " << turbulence_->maxYPlus()
                  << ". This is a low-Re k-omega SST formulation with no wall\n"
                     "         function, so the first-cell wall shear is only meaningful for\n"
                      "         y+ ~ 1. Refine the near-wall mesh (or accept that the wall\n"
                      "         shear is mesh-dependent).\n";
    }
    if (!isTransient && config_.printTotalTime && steadyTotalSeconds_ > 0.0) {
        std::cerr << "  [timing] steady SIMPLE total=" << steadyTotalSeconds_ << " s"
                  << " momentum=" << steadyMomentumSeconds_ << " s"
                  << " predictedContinuity=" << steadyPredictedContinuitySeconds_ << " s"
                  << " pressure=" << steadyPressureSeconds_ << " s"
                  << " residuals=" << steadyResidualSeconds_ << " s"
                  << " turbulence=" << steadyTurbulenceSeconds_ << " s\n";
        std::cerr << "  [timing] momentum: assembly=" << steadyMomentumAssemblySeconds_
                  << " s dispatch=" << linearSolverDispatcher_.dispatchSeconds()
                  << " s\n";
        const auto& ps = pressureCorrection_.pressureStats();
        std::cerr << "  [timing] AMG: momentum build=" << linearSolverDispatcher_.buildSeconds()
                  << " s solve=" << linearSolverDispatcher_.amgSeconds()
                  << " s builds=" << linearSolverDispatcher_.amgBuilds()
                  << " | pressure build=" << ps.buildSeconds
                  << " s solve=" << ps.amgSeconds
                  << " s builds=" << ps.amgBuilds
                  << " krylov=" << ps.amgKrylovIters << "\n";
        if (turbulence_) {
            std::cerr << "  [timing] AMG: turbulence build="
                      << turbulence_->turbulenceBuildSeconds()
                      << " s solve=" << turbulence_->turbulenceSolveSeconds()
                      << " s builds=" << turbulence_->turbulenceBuilds() << "\n";
        }
    }
}

template<std::size_t Dim>
const char* CFDSolver<Dim>::statusText() const {
    switch (status_) {
        case Status::Converged:   return "converged";
        case Status::Completed:   return "completed (reached the end time)";
        case Status::Aborted:     return "aborted (diverged or non-finite residual)";
        case Status::Cancelled:   return "cancelled";
        case Status::NotConverged: break;
    }
    return "not converged (iteration limit reached)";
}

template<std::size_t Dim>
const char* CFDSolver<Dim>::failureText() const {
    switch (failure_) {
        case Failure::None:                  return "";
        case Failure::NonFiniteContinuity:   return "non-finite continuity residual";
        case Failure::DivergingContinuity:   return "continuity residual kept increasing";
        case Failure::NonFiniteTurbulence:   return "non-finite k/omega residual";
        case Failure::MomentumSolveFailed: return "momentum linear solve broke down";
        case Failure::PressureSolveFailed: return "pressure linear solve broke down";
    }
    return "";
}

template<std::size_t Dim>
void CFDSolver<Dim>::fail_(Failure reason) {
    if (failure_ == Failure::None) {
        failure_ = reason;
        // One line per run, at the point the numerics actually broke: the
        // individual check sites used to print their own warnings, so the same
        // failure could be reported twice with different wording.
        std::cerr << "  WARNING: " << failureText() << ", aborting\n";
    }
    aborted_ = true;
}

template<std::size_t Dim>
double CFDSolver<Dim>::finalContinuityResidual() const { return lastContResidRaw_; }

template<std::size_t Dim>
double CFDSolver<Dim>::finalNormalizedContinuityResidual() const { return lastContResid_; }

template<std::size_t Dim>
double CFDSolver<Dim>::finalMassImbalance() const { return lastContResidPost_; }

template<std::size_t Dim>
double CFDSolver<Dim>::finalMomentumResidual() const { return lastMomResid_; }

template<std::size_t Dim>
SSTModel<Dim>* CFDSolver<Dim>::turbulence() { return turbulence_.get(); }

template<std::size_t Dim>
const SSTModel<Dim>* CFDSolver<Dim>::turbulence() const { return turbulence_.get(); }

template<std::size_t Dim>
const Field<Dim>& CFDSolver<Dim>::zeroSource_() const {
    if (!zeroSourceInitialized_ || zeroSourceCache_.size() != mesh_.nCells()) {
        zeroSourceCache_ = Field<Dim>(mesh_, 0.0);
        zeroSourceInitialized_ = true;
    }
    return zeroSourceCache_;
}

template<std::size_t Dim>
void CFDSolver<Dim>::applyPressureCorrection_(VectorField<Dim>& uStar, double alphaP,
                                           bool applyLimiter, bool capturePreCorrector,
                                           const std::vector<double>* faceVolFlux) {
    // PISO measures the continuity imbalance on u~ BEFORE the pressure solve
    // (the residual-reduction test). The post-correction value is ~0 by
    // construction and cannot drive that test.
    if (capturePreCorrector) {
        ResidualResult pre = computeResiduals(
            mesh_, uStar, velocityOld_, diag_, divBuffer_,
            bcDiscretization().dirichletMask(), bcDiscretization().dirichletVals(),
            &bcDiscretization().faceBCs(), &dCoeff_,
            &boundPressure(),
            &pressureGrad_,
            &pressureCorrection_.pressureMask(),
            useProvisionalFlux_ ? &faceVolFlux_ : nullptr,
            useProvisionalFlux_ ? &pPrime_ : nullptr);
        lastPisoPreCorrectorResidual_ = pre.continuity;
    }

    pressureCorrection_.solve(uStar, dCoeff_, boundPressure(), pPrime_,
                              bcDiscretization(), faceVolFlux, cancelFlag());
    if (pressureCorrection_.lastSolveProducedNonFinite())
        fail_(Failure::PressureSolveFailed);
    // The p' limiter is on for the steady and transient paths and off for
    // the PISO correctors: scaling p' would silently impose alphaP < 1 and
    // break the continuity the corrector just enforced.
    if (applyLimiter) limitPressureCorrection_(pPrime_);
    pressureBeforeCorrection_ = boundPressure();
    pressureGradBefore_ = pressureCorrection_.pressureGradient();
    // Standard SIMPLE: full velocity correction u = u* - d grad p' (divergence-free);
    // pressure under-relaxation p += alphaP p' only.
    correctVelocity(mesh_, uStar, pPrime_, dCoeff_,
                    bcDiscretization().dirichletMask(), bcDiscretization().dirichletVals(),
                    dpdx_, boundVelocity(),
                    &bcDiscretization().faceBCs(),
                    config_.velocityCorrectionMaxUnlimitedSpeed,
                    config_.velocityCorrectionMaxGrowthRate,
                    pressureGradientCalc_.get());
    #pragma omp parallel for schedule(static)
    for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
        (boundPressure())[i] += alphaP * pPrime_[i];
    }

    // Pressure-outlet owner cells are regular unknowns: the pressure correction
    // equation already enforces p' = 0 at the outlet face via the outlet-ring
    // diagonal (PressureCorrection.cpp), and the owner cell pressure evolves
    // naturally through P += alphaP * p'. Overwriting the outlet cell pressure
    // with the boundary value would discard the correction and corrupt mass
    // conservation at the outlet.
}

template<std::size_t Dim>
bool CFDSolver<Dim>::limitPressureCorrection_(Field<Dim>& pPrime) {
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
void CFDSolver<Dim>::saveCheckpoint_(std::size_t iter) {
    saveCheckpoint_(iter, nullptr);
}

template<std::size_t Dim>
void CFDSolver<Dim>::saveCheckpoint_(std::size_t iter, const TransientBlock<Dim>* transient) {
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
                         boundVelocity(), boundPressure(),
                         kPtr, omegaPtr, nutPtr, transient);
    }
}

template<std::size_t Dim>
bool CFDSolver<Dim>::updateStoppingPolicy(const cfd::ResolvedStoppingPolicy& policy) {
    config_.stoppingPolicy = policy;
    convergenceMonitor_ = std::make_unique<cfd::ConvergenceMonitor>(
        config_.stoppingPolicy.steady);
    return true;
}

template<std::size_t Dim>
int CFDSolver<Dim>::lastIterations() const { return lastIterations_; }

template<std::size_t Dim>
void CFDSolver<Dim>::solveMomentumComponent_(std::size_t d, VectorField<Dim>& uStar,
                                           const VectorField<Dim>& velocityOld,
                                           const std::array<Field<Dim>, Dim>& pressureGrad,
                                           const std::vector<double>* faceVolFlux,
                                           const TransientParams<Dim>& tp) {
    // Assemble in place into the per-component snapshot: the operator pattern
    // is reused across iterations, and the residual/PISO code reads the same
    // object instead of a copy.
    SparseMatrix& momA = momAsnap_[d];
    Eigen::VectorXd& momB = momBsnap_[d];

    const auto tAsm0 = std::chrono::steady_clock::now();
    const Field<Dim>* nutPtr = turbulence_ ? &turbulence_->nut() : nullptr;

    gradientCalc_->compute((boundVelocity())[d], velGrad_);
    const std::array<Field<Dim>, Dim>* velGradPtr = &velGrad_;

    fvm::MomentumBCs momBCs;
    momBCs.slipWallMask = &bcDiscretization().slipWallMask(d);
    momBCs.dirichletMaskAll = &bcDiscretization().dirichletMask();
    momBCs.dirichletValsAll = &bcDiscretization().dirichletVals();
    momBCs.faceBC = &bcDiscretization().faceBCs();

    momentum_.assembleWithConvection(d, boundVelocity(), pressureGrad,
                                     zeroSource_(),
                                     bcDiscretization().dirichletMask(d), bcDiscretization().dirichletVals(d),
                                     momA, momB, momDiag_,
                                     config_.convectionScheme,
                                     nutPtr, velGradPtr,
                                     faceVolFlux,
                                     momBCs, tp);

    diag_[d] = momDiag_;

    // PISO: no momentum under-relaxation (alphaU=1). PIMPLE/SIMPLE use config.alphaU.
    const double alphaU = tp.hasTime ? (isPiso_() ? 1.0 : config_.alphaU) : config_.alphaU;
    momentum_.computeDCoeffAndRelax(momA, momB, bcDiscretization().dirichletMask(d),
                                    velocityOld[d], alphaU,
                                    dCoeff_[d], config_.simplec);

    momentumWarmStart_.resize(static_cast<Eigen::Index>(mesh_.nCells()));
    const double* velPtr = (boundVelocity())[d].ptr();
    for (std::size_t i = 0; i < mesh_.nCells(); ++i)
        momentumWarmStart_(static_cast<Eigen::Index>(i)) = velPtr[i];

    steadyMomentumAssemblySeconds_ += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - tAsm0).count();
    const auto tSol0 = std::chrono::steady_clock::now();
    auto result = linearSolverDispatcher_.solve(d, momA, momB, momentumWarmStart_,
                                                &cancelRequested_);
    if (result.failed || !result.x.allFinite()) fail_(Failure::MomentumSolveFailed);

    for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
        double val = result.x(static_cast<Eigen::Index>(i));
        if (std::isnan(val) || std::isinf(val)) val = (boundVelocity())[d][i];
        if (bcDiscretization().dirichletMask(d)[i]) val = bcDiscretization().dirichletVals(d)[i];
        uStar[d][i] = val;
    }
}

template<std::size_t Dim>
void CFDSolver<Dim>::updateMaxInletSpeed_() {
    // Must be re-evaluated whenever BCs change. Includes moving walls so a
    // lid-driven cavity does not fall back to an arbitrary 1.0 (which the
    // velocity clamp could then use to override the lid velocity itself).
    maxInletSpeed_ = bc_.maxPrescribedSpeed(1.0);
    if (maxInletSpeed_ < minDivisor) maxInletSpeed_ = 1.0;
}

template<std::size_t Dim>
void CFDSolver<Dim>::printIterationRow_(bool isTransient, int iter, double time, double dt) {
    if (headerCounter_ % 25 == 0) printHeader_(isTransient);
    ++headerCounter_;

    // Steady-only: the per-equation AMG diagnosis. Transient rows carry the
    // time/dt columns and are printed by the driver loop; the diagnosis
    // lives here.
    const auto verbosity = cfd::AmgSettings::Verbosity::Diagnosis;
    if (!isTransient && config_.momentumAmg.verbosity == verbosity) {
        for (std::size_t d = 0; d < Dim; ++d) {
            const char* name = (d == 0) ? "x-momentum"
                               : (d == 1) ? "y-momentum"
                               : "z-momentum";
            if (!linearSolverDispatcher_.amgBuilt(d)) continue;
            std::cout << name << " equation:\n";
            std::cout << "  tol.   " << std::scientific << std::setprecision(4)
                      << config_.momentumAmg.tolerance << "\n";
            const auto& hist = linearSolverDispatcher_.amgLastResidualHistory(d);
            for (std::size_t c = 0; c < hist.size(); ++c) {
                std::cout << "    " << std::setw(2) << c << " "
                          << std::scientific << std::setprecision(4)
                          << hist[c] << "\n";
            }
            std::cout << "  iters  " << linearSolverDispatcher_.amgLastIterations(d)
                      << "  rel.resid " << std::scientific << std::setprecision(4)
                      << linearSolverDispatcher_.amgLastResidual(d) << "\n";
            const auto& sizes = linearSolverDispatcher_.amgLevelSizes(d);
            std::cout << "  multigrid levels:\n";
            std::cout << "  level min    max    mean   total\n";
            for (int l = 0; l < static_cast<int>(sizes.size()); ++l) {
                const int sz = sizes[static_cast<std::size_t>(l)];
                std::cout << "   " << std::setw(2) << l << " "
                          << std::setw(6) << sz << " "
                          << std::setw(6) << sz << " "
                          << std::setw(7) << sz << " "
                          << std::setw(6) << sz << "\n";
            }
        }
    }
    if (!isTransient && config_.pressureAmg.verbosity == verbosity &&
        pressureCorrection_.amgBuilt()) {
        std::cout << "pressure correction equation:\n";
        std::cout << "  tol.   " << std::scientific << std::setprecision(4)
                  << config_.pressureAmg.tolerance << "\n";
        const auto& hist = pressureCorrection_.amgLastResidualHistory();
        for (std::size_t c = 0; c < hist.size(); ++c) {
            std::cout << "    " << std::setw(2) << c << " "
                      << std::scientific << std::setprecision(4)
                      << hist[c] << "\n";
        }
        std::cout << "  iters  " << pressureCorrection_.lastSolveIterations()
                  << "  rel.resid " << std::scientific << std::setprecision(4)
                  << pressureCorrection_.lastSolveResidual() << "\n";
        const auto& sizes = pressureCorrection_.amgLevelSizes();
        std::cout << "  multigrid levels:\n";
        std::cout << "  level min    max    mean   total\n";
        for (int l = 0; l < static_cast<int>(sizes.size()); ++l) {
            const int sz = sizes[static_cast<std::size_t>(l)];
            std::cout << "   " << std::setw(2) << l << " "
                      << std::setw(6) << sz << " "
                      << std::setw(6) << sz << " "
                      << std::setw(7) << sz << " "
                      << std::setw(6) << sz << "\n";
        }
    }

    constexpr int wIter = 6;
    constexpr int wRes = 13;
    constexpr int wTime = 13;
    const double iterTime = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_iterStart).count();
    std::ostringstream line;
    line << std::setw(wIter) << iter;
    // Transient rows: step, then time and dt right after the step number.
    if (isTransient)
        line << std::setw(wTime) << time << std::setw(wTime) << dt;
    line << std::scientific << std::setprecision(4)
         << std::setw(wRes) << lastContResid_
         << std::setw(wRes) << lastMomChangeU_
         << std::setw(wRes) << lastMomChangeV_;
    if constexpr (Dim == 3) line << std::setw(wRes) << lastMomChangeW_;
    if (turbulence_) line << std::setw(wRes) << lastKResid_
                           << std::setw(wRes) << lastOmegaResid_;
    // Resident-set size, steady only (time/iter below).
    if (config_.printResidentSet && !isTransient)
        line << std::fixed << std::setprecision(1) << std::setw(wRes) << residentSetMiB();
    // Time stays last so it never splits the residual columns. Steady-only.
    if (config_.printIterationTime && !isTransient)
        line << std::fixed << std::setprecision(4) << std::setw(wTime) << iterTime;
    std::cout << line.str() << std::endl;
    // One reverse-flow line per printed solver row, steady or transient.
    const std::size_t reverseFlow = pressureCorrection_.lastReverseFlowFaces();
    if (reverseFlow > 0) {
        std::cerr << "  WARNING: " << reverseFlow
                  << " pressure outlet face(s) with reverse flow\n";
    }
    if (std::getenv("SCIRENDER_TRACE")) {
        double mu = 0.0, mv = 0.0, mw = 0.0, mp = 0.0, mpp = 0.0;
        for (std::size_t i = 0; i < mesh_.nCells(); ++i) {
            if (mesh_.isBoundary(i)) continue;
            mu = std::max(mu, std::abs((boundVelocity())[0][i]));
            mv = std::max(mv, std::abs((boundVelocity())[1][i]));
            if constexpr (Dim == 3) mw = std::max(mw, std::abs((boundVelocity())[2][i]));
            mp = std::max(mp, std::abs((boundPressure())[i]));
            mpp = std::max(mpp, std::abs(pPrime_[i]));
        }
        std::cerr << "  [trace] max|u|=" << mu << " max|v|=" << mv << " max|w|=" << mw
                  << " max|p|=" << mp << " max|p'|=" << mpp << "\n";
    }
}

template class CFDSolver<2>;
template class CFDSolver<3>;

} // namespace fvm
