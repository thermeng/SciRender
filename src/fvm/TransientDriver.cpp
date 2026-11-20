#include "fvm/TransientDriver.h"

#include "fvm/SIMPLE.h"
#include "fvm/SIMPLETypes.h"
#include "fvm/VelocityCorrection.h"
#include "fvm/Residual.h"
#include "fvm/SSTConstants.h"
#include "io/Checkpoint.h"
#include "core/pvd_writer.h"

#include <iomanip>
#include <cmath>
#include <utility>

namespace fvm {

using cfd::ResolvedStoppingPolicy;
using cfd::ResidualEquation;

// True PISO predictor. The momentum equation is assembled and solved once
// per time step; the pressure-free right-hand side b0 is frozen for the
// correctors so each one can reconstruct H(u) cheaply.
template<std::size_t Dim>
void TransientDriver<Dim>::solveMomentumTransient_(VectorField<Dim>& uStar, const VectorField<Dim>& velocityOld) {
    // The pressure source in the transient momentum equation must use the same
    // least-squares operator as the pressure correction. The Green-Gauss
    // gradient used to be used here, which made the predictor solve a
    // different pressure term than the corrector cancelled; the mismatch is
    // largest on skewed/non-uniform meshes.
    solver_.pressureGradientCalc_->compute(solver_.boundPressure(), solver_.pressureGrad_);
    if constexpr (Dim == 2) {
        solver_.solveMomentumComponent_(0, uStar, velocityOld, solver_.pressureGrad_, momentumTp_(0));
        solver_.solveMomentumComponent_(1, uStar, velocityOld, solver_.pressureGrad_, momentumTp_(1));
    } else {
        solver_.solveMomentumComponent_(0, uStar, velocityOld, solver_.pressureGrad_, momentumTp_(0));
        solver_.solveMomentumComponent_(1, uStar, velocityOld, solver_.pressureGrad_, momentumTp_(1));
        solver_.solveMomentumComponent_(2, uStar, velocityOld, solver_.pressureGrad_, momentumTp_(2));
    }
}

template<std::size_t Dim>
TransientParams<Dim> TransientDriver<Dim>::momentumTp_(std::size_t d) const {
    TransientParams<Dim> tp;
    tp.hasTime = true;
    tp.dt = dt_;
    tp.dtPrev = dtPrev_;
    tp.timeScheme = solver_.config_.transient.timeScheme;
    tp.oldTime = &velocityN_[d];
    // BDF2 needs two levels of history. dtPrev_ == 0 additionally forces
    // explicit Euler for this step (handled inside the assembler), which is
    // what a restart from a fresh field must do.
    if (solver_.timeStep_ > 1 && dtPrev_ > 0.0 && solver_.hasVelocityHistory_)
        tp.oldTimePrev = &velocityNM1_[d];
    if (solver_.timeStep_ == 1 && tp.timeScheme == TimeScheme::BDF2) tp.timeScheme = TimeScheme::Euler;
    if (!solver_.hasVelocityHistory_ && tp.timeScheme == TimeScheme::BDF2) tp.timeScheme = TimeScheme::Euler;
    return tp;
}

template<std::size_t Dim>
void TransientDriver<Dim>::performPISOPredictor_(VectorField<Dim>& velocity, Field<Dim>& pressure) {
    (void)velocity;
    (void)pressure;
    for (std::size_t d = 0; d < Dim; ++d)
        std::copy((solver_.boundVelocity())[d].values().begin(),
                  (solver_.boundVelocity())[d].values().end(),
                  solver_.velocityOld_[d].values().begin());
    solveMomentumTransient_(solver_.uStar_, solver_.velocityOld_);
    solver_.capturePredictedContinuity_();

    // The corrector reconstructs the explicit momentum residual at the
    // CURRENT velocity. For the first corrector that current velocity is the
    // predictor u*, not the old time level: copying it here makes
    // updatePisoPredictor_ see a zero residual and reproduce u*, exactly as
    // PISO's first corrector should.
    solver_.boundVelocity() = solver_.uStar_;

    // b0 = b + V grad(p^n): the momentum right-hand side with the pressure
    // source removed. b is the snapshot the linear solve just used, so this
    // is the exact same operator/source pair.
    const std::size_t n = solver_.mesh_.nCells();
    const double* vol = solver_.mesh_.cellVolumePtr();
    for (std::size_t d = 0; d < Dim; ++d) {
        solver_.momB0_[d].resize(static_cast<Eigen::Index>(n));
        for (std::size_t i = 0; i < n; ++i) {
            solver_.momB0_[d](static_cast<Eigen::Index>(i)) =
                solver_.momBsnap_[d](static_cast<Eigen::Index>(i)) + vol[i] * solver_.pressureGrad_[d][i];
        }
    }
}

// Rebuild u~ = H(u)/aP - d grad(p) from the current velocity and pressure.
// H(u) is the momentum operator with the off-diagonal terms evaluated
// explicitly at u and the pressure source removed:
//
//   aP u~ = b0 - sum_off a_nb u_nb - V grad(p)
//         = b0 - A u + aP u - V grad(p)
//
// so u~ = u + (b0 - A u - V grad(p)) / aP. With aP = V/dCoeff this is the
// HbyA form used by PISO: the first corrector (u = u*, p = p^n) reproduces
// u*, while later correctors see the non-zero momentum residual left by the
// previous projection.
template<std::size_t Dim>
void TransientDriver<Dim>::updatePisoPredictor_() {
    solver_.pressureGradientCalc_->compute(solver_.boundPressure(), solver_.pressureGrad_);

    const auto& u = solver_.boundVelocity();
    const std::size_t n = solver_.mesh_.nCells();
    const double* vol = solver_.mesh_.cellVolumePtr();
    for (std::size_t d = 0; d < Dim; ++d) {
        if (solver_.momB0_[d].size() != static_cast<Eigen::Index>(n) ||
            solver_.momAsnap_[d].rows() != static_cast<Eigen::Index>(n)) {
            // Defensive: no frozen predictor (should not happen in the PISO
            // path). Fall back to the current velocity.
            solver_.uStar_[d] = u[d];
            continue;
        }
        Eigen::Map<const Eigen::VectorXd> uvec(
            u[d].values().data(), static_cast<Eigen::Index>(n));
        const Eigen::VectorXd Au = solver_.momAsnap_[d] * uvec;
        #pragma omp parallel for schedule(static)
        for (std::size_t i = 0; i < n; ++i) {
            const Eigen::Index ii = static_cast<Eigen::Index>(i);
            double corr = 0.0;
            const double dc = solver_.dCoeff_[d][i];
            if (dc > 0.0) {
                const double aP = vol[i] / dc;
                const double residual = solver_.momB0_[d](ii) - Au(ii) - vol[i] * solver_.pressureGrad_[d][i];
                corr = residual / aP;
            }
            double val = u[d][i] + corr;
            if (!std::isfinite(val)) val = u[d][i];
            solver_.uStar_[d][i] = val;
        }
        const auto& dirMask = solver_.bcDiscretization().dirichletMask(d);
        const auto& dirVals = solver_.bcDiscretization().dirichletVals(d);
        for (std::size_t i = 0; i < n; ++i) {
            if (dirMask[i]) solver_.uStar_[d][i] = dirVals[i];
        }
    }
}

template<std::size_t Dim>
void TransientDriver<Dim>::performPISOCorrector_(VectorField<Dim>& velocity, Field<Dim>& pressure) {
    (void)velocity;
    (void)pressure;
    const double alphaP = 1.0; // PISO: no under-relaxation
    // nonOrthogonalCorrectors is the number of inner PISO passes per outer
    // corrector. Each pass reconstructs the momentum residual at the velocity
    // the previous pass produced and projects it again; this is not a no-op
    // even though the compact face-normal Laplacian is exact for the
    // orthogonal part of the operator.
    const int nInner = std::max(1, solver_.config_.transient.nonOrthogonalCorrectors);
    for (int inner = 0; inner < nInner; ++inner) {
        updatePisoPredictor_();

        // The imbalance this corrector is about to remove. Captured on u~
        // before the pressure solve, with the same Rhie-Chow flux the matrix
        // uses. The post-correction value is ~0 by construction and cannot
        // drive the PISO reduction test. The mask is only populated once the
        // first solve has run, so fall back to the uncorrected divergence
        // until then (same rule as capturePredictedContinuity_).
        solver_.applyPressureCorrection_(solver_.uStar_, /*alphaP=*/1.0,
                                         /*applyLimiter=*/false, /*capturePreCorrector=*/true);
        // Do NOT copy boundVelocity() back into uStar_: the next inner pass
        // must reconstruct u~ from the corrected velocity, not reuse the
        // previous projection's input.
    }
}


template<std::size_t Dim>
void TransientDriver<Dim>::saveTransientCheckpoint_(int step, double time) {
    // Ship the full transient state (time, dt, dt/dtPrev ratio and the BDF2
    // history) so a restart can resume mid-run instead of replaying from t=0.
    TransientBlock<Dim> tb;
    tb.time = time;
    tb.dt = dt_;
    tb.dtPrev = dtPrev_;
    tb.step = static_cast<std::size_t>(step);
    tb.velocityN = &velocityN_;
    tb.velocityNM1 = &velocityNM1_;
    tb.kN = solver_.turbulence_ ? &kN_ : nullptr;
    tb.kNM1 = solver_.turbulence_ ? &kNM1_ : nullptr;
    tb.omegaN = solver_.turbulence_ ? &omegaN_ : nullptr;
    tb.omegaNM1 = solver_.turbulence_ ? &omegaNM1_ : nullptr;
    tb.pvdEntries = pvdEntries_;
    solver_.saveCheckpoint_(static_cast<std::size_t>(step), &tb);
}

template<std::size_t Dim>
void TransientDriver<Dim>::performTransientIteration_(VectorField<Dim>& velocity, Field<Dim>& pressure) {
    // PISO: no pressure under-relaxation. PIMPLE keeps SIMPLE relaxation.
    const double alphaP = solver_.isPiso_() ? 1.0 : solver_.config_.alphaP;

    for (std::size_t d = 0; d < Dim; ++d)
        std::copy((solver_.boundVelocity())[d].values().begin(),
                  (solver_.boundVelocity())[d].values().end(),
                  solver_.velocityOld_[d].values().begin());
    solveMomentumTransient_(solver_.uStar_, solver_.velocityOld_);
    solver_.capturePredictedContinuity_();
    solver_.applyPressureCorrection_(solver_.uStar_, alphaP, /*applyLimiter=*/true, /*capturePreCorrector=*/false);
    if (solver_.turbulence_) {
        solver_.nutOld_ = solver_.turbulence_->nut();
        // Transient turbulence
        if (solver_.config_.transient.enabled && dt_ > 0) {
            TimeScheme ts = solver_.config_.transient.timeScheme;
            if (solver_.timeStep_ == 1 && ts == TimeScheme::BDF2) ts = TimeScheme::Euler;
            // Two levels of history are needed for BDF2; dtPrev_ == 0 (fresh
            // start or restart) makes the assembler fall back to Euler.
            const Field<Dim>* kPrevPrev =
                (solver_.timeStep_ > 1 && dtPrev_ > 0.0) ? &kNM1_ : nullptr;
            const Field<Dim>* oPrevPrev =
                (solver_.timeStep_ > 1 && dtPrev_ > 0.0) ? &omegaNM1_ : nullptr;
            // kN_/omegaN_ hold the previous time level, unchanged across the
            // outer correctors of this step.
            solver_.turbulence_->solveIterationTransient(solver_.boundVelocity(),
                                                         solver_.config_.convectionScheme, dt_, dtPrev_, ts,
                                                         kN_, omegaN_, kPrevPrev, oPrevPrev);
        } else {
            solver_.turbulence_->solveIteration(solver_.boundVelocity(), solver_.config_.convectionScheme);
        }
        double alphaNut = nutUnderRelax;
        auto& nut = solver_.turbulence_->nut();
        #pragma omp parallel for schedule(static)
        for (std::size_t i = 0; i < solver_.mesh_.nCells(); ++i) {
            nut[i] = alphaNut * nut[i] + (1.0 - alphaNut) * solver_.nutOld_[i];
        }
    }
}

template<std::size_t Dim>
double TransientDriver<Dim>::computeAdaptiveDt_() const {
    // Cell size for the CFL estimate must be the *controlling* (smallest) cell
    // dimension, not the geometric mean. sqrt(V)/cbrt(V) over-estimates it on
    // stretched or anisotropic cells, so the advertised `cfl` value would be
    // quietly exceeded exactly where PISO needs it most. We use the
    // hydraulic size 2V/sum(face areas), which is <= the smallest cell
    // dimension, and still blend 70% average / 30% minimum so a single tiny
    // cell (e.g. in a cylinder boundary layer) does not collapse dt.
    const CFDSolver<Dim>& s = solver_;
    const std::size_t n = s.mesh_.nCells();
    const std::size_t nF = s.mesh_.nFaces();

    std::vector<double> areaSum(n, 0.0);
    for (std::size_t f = 0; f < nF; ++f) {
        const double a = s.mesh_.faceArea(f);
        if (!(a > 0.0)) continue;
        areaSum[s.mesh_.owner(f)] += a;
        const std::size_t nb = s.mesh_.neighbor(f);
        if (nb != std::size_t(-1)) areaSum[nb] += a;
    }

    double minDx = std::numeric_limits<double>::max();
    double sumDx = 0.0;
    std::size_t count = 0;
    double maxVel = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (s.mesh_.isBoundary(i)) continue;
        double dx = (areaSum[i] > 0.0)
            ? 2.0 * s.mesh_.cellVolume(i) / areaSum[i]
            : ((Dim == 2) ? std::sqrt(s.mesh_.cellVolume(i)) : std::cbrt(s.mesh_.cellVolume(i)));
        if (dx < minDx) minDx = dx;
        sumDx += dx;
        ++count;
        double vel2 = 0.0;
        for (std::size_t d = 0; d < Dim; ++d) vel2 += (s.boundVelocity())[d][i] * (s.boundVelocity())[d][i];
        double vm = std::sqrt(vel2);
        if (vm > maxVel) maxVel = vm;
    }
    if (count == 0) return std::min(s.config_.transient.maxDt, std::max(s.config_.transient.dt, s.config_.transient.minDt));
    double avgDx = sumDx / static_cast<double>(count);
    // Blend: 70% avg, 30% min to avoid single tiny cell dominating; for uniform meshes min~avg
    double charDx = 0.7 * avgDx + 0.3 * minDx;
    if (charDx <= 1e-12) charDx = minDx;
    if (maxVel < 1e-12) return std::min(s.config_.transient.maxDt, std::max(s.config_.transient.dt, s.config_.transient.minDt));
    // Clamp maxVel to at least the prescribed driving speed so a stalled field
    // cannot let dt grow without bound.
    double velRef = std::max(maxVel, s.maxInletSpeed_);
    if (velRef < 1e-12) velRef = maxVel;
    double dtCFL = s.config_.transient.cfl * charDx / velRef;
    dtCFL = std::max(s.config_.transient.minDt, std::min(dtCFL, s.config_.transient.maxDt));

    // Diffusive limit. The convective CFL alone ignores the k/omega diffusion
    // time scale: with a large eddy viscosity an explicit-ish transport update
    // can go unstable even though the flow CFL is small.
    if (s.turbulence_) {
        double maxNut = 0.0;
        const auto& nut = s.turbulence_->nut();
        for (std::size_t i = 0; i < n; ++i) maxNut = std::max(maxNut, nut[i]);
        const double nuEff = s.nu_ + std::max(maxNut, 0.0);
        if (nuEff > 1e-30) {
            const double dtDiff = s.config_.transient.cfl * charDx * charDx / (2.0 * nuEff);
            dtCFL = std::min(dtCFL, dtDiff);
            dtCFL = std::max(s.config_.transient.minDt,
                             std::min(dtCFL, s.config_.transient.maxDt));
        }
    }
    return dtCFL;
}

template<std::size_t Dim>
void TransientDriver<Dim>::run(VectorField<Dim>& velocity, Field<Dim>& pressure) {
    CFDSolver<Dim>& s = solver_;
    s.initializeSolve_(velocity, pressure);

    s.applyStoppingPolicy_();

    // --- Transient state -------------------------------------------------
    // initializeSolve_ may have loaded a checkpoint. If that checkpoint carried
    // transient state we resume at the stored time with the stored dt and
    // BDF2 history; otherwise we start a brand new run at t=0. Previously this
    // was hard-coded to t=0, so "restart" silently replayed the whole run on
    // top of a mid-run field and lost u^{n-1}.
    bool haveHistory = false;
    // Driver-owned state resets (mirrors the old initializeSolve_ else
    // branch); gated so a queued RestartState is not clobbered by the
    // fresh-run reset.
    if (!s.hasRestoredTransientState_) {
        time_ = 0.0;
        velocityN_ = velocity;
        velocityNM1_ = velocity;
        dtPrev_ = 0.0;
    }
    pvdEntries_.clear();
    if (s.restarting_) {
        TransientBlock<Dim> tb;
        tb.velocityN = &velocityN_;
        tb.velocityNM1 = &velocityNM1_;
        tb.kN = s.turbulence_ ? &kN_ : nullptr;
        tb.kNM1 = s.turbulence_ ? &kNM1_ : nullptr;
        tb.omegaN = s.turbulence_ ? &omegaN_ : nullptr;
        tb.omegaNM1 = s.turbulence_ ? &omegaNM1_ : nullptr;
        if (Checkpoint::load(s.config_.checkpoint.restartFrom, s.mesh_,
                             s.startIteration_, s.lastContResid_, s.lastMomResid_,
                             velocity, pressure,
                             s.turbulence_ ? &s.turbulence_->k() : nullptr,
                             s.turbulence_ ? &s.turbulence_->omega() : nullptr,
                             s.turbulence_ ? &s.turbulence_->nut() : nullptr,
                             &tb)) {
            haveHistory = true;
            time_ = tb.time;
            s.timeStep_ = static_cast<int>(tb.step); step_ = static_cast<int>(tb.step);
            dt_ = (tb.dt > 0.0) ? tb.dt : s.config_.transient.dt;
            dtPrev_ = tb.dtPrev;
            pvdEntries_ = std::move(tb.pvdEntries);
            if (s.turbulence_) s.turbulence_->clampFields();
            std::cout << "  [transient restart] resuming at t=" << time_
                      << " step=" << s.timeStep_ << " dt=" << dt_
                      << " dtPrev=" << dtPrev_ << "\n";
        } else {
            std::cerr << "  [transient] checkpoint had no transient state"
                      << " â€” restarting from t=0\n";
        }
    }

    if (s.hasRestoredTransientState_) {
        haveHistory = true;
        velocityN_ = velocity;
        if (velocityNM1_[0].size() == 0) velocityNM1_ = velocity;
        if (s.turbulence_) {
            kN_ = s.turbulence_->k();
            omegaN_ = s.turbulence_->omega();
            if (kNM1_.size() == 0) kNM1_ = kN_;
            if (omegaNM1_.size() == 0) omegaNM1_ = omegaN_;
        }
    }

    time_ = haveHistory ? time_ : 0.0;
    s.timeStep_ = haveHistory ? s.timeStep_ : 0;
    if (!haveHistory) {
        dtPrev_ = 0.0;
        dt_ = s.config_.transient.dt;
        if (dt_ <= 0) dt_ = 1e-3;
        velocityN_ = velocity;
        velocityNM1_ = velocity;
        pvdEntries_.clear();
    }

    // Clamp dt to the configured limits, keeping whatever we restored.
    dt_ = std::max(s.config_.transient.minDt, std::min(dt_, s.config_.transient.maxDt));
    dtPrev_ = std::max(s.config_.transient.minDt,
                       std::min(dtPrev_, s.config_.transient.maxDt));
    pressureN_ = pressure;
    // Determine base name for PVD from transient.pvdFile or default
    pvdBaseName_ = s.config_.transient.pvdFile;
    if (pvdBaseName_.empty()) pvdBaseName_ = "transient";
    // Remove extension if provided
    if (pvdBaseName_.size() > 4 && pvdBaseName_.substr(pvdBaseName_.size()-4) == ".pvd") {
        pvdBaseName_ = pvdBaseName_.substr(0, pvdBaseName_.size()-4);
    }
    if (s.turbulence_) {
        // On restart kN_/kNM1_ were restored from the checkpoint; only seed
        // them from the (also restored) field on a fresh run.
        if (!haveHistory) {
            kN_ = s.turbulence_->k();
            omegaN_ = s.turbulence_->omega();
            kNM1_ = kN_;
            omegaNM1_ = omegaN_;
        }
    } else {
        kN_ = Field<Dim>(s.mesh_, 0.0);
        omegaN_ = Field<Dim>(s.mesh_, 0.0);
        kNM1_ = Field<Dim>(s.mesh_, 0.0);
        omegaNM1_ = Field<Dim>(s.mesh_, 0.0);
    }
    int nSteps = 1;
    if (s.config_.stoppingPolicy.transientPolicy.tEnd > 0 && dt_ > 0) {
        nSteps = static_cast<int>(std::ceil(s.config_.stoppingPolicy.transientPolicy.tEnd / dt_));
        if (nSteps < 1) nSteps = 1;
    }
    // If adaptive, nSteps is just upper bound; we loop on time
    {
        std::string solverName = solverTypeToString(s.config_.solverType);
        // Legacy piso bool still honored for compat
        if (s.config_.transient.piso && s.config_.solverType==SolverType::SIMPLE) solverName="PISO";
        std::cout << "Transient " << solverName << ": dt=" << dt_ << " tEnd="
                  << s.config_.stoppingPolicy.transientPolicy.tEnd
                  << " scheme=" << (s.config_.transient.timeScheme==TimeScheme::BDF2?"BDF2":"Euler")
                  << " adaptive=" << (s.config_.transient.adaptiveCfl?"yes":"no")
                  << " nOuter="
                  << s.config_.stoppingPolicy.transientPolicy.maxOuterCorrectors
                  << (s.config_.simplec ? " SIMPLEC" : "") << "\n";
    }
    double tEnd = s.config_.stoppingPolicy.transientPolicy.tEnd;
    int step = s.timeStep_; step_ = step;
    const int firstStep = step;
    const std::size_t stepBudget = s.config_.stoppingPolicy.transientPolicy.maxTimeSteps;
    if (!haveHistory) time_ = 0.0;
    // PISO on a collocated grid requires Rhie-Chow. Force it (and therefore
    // the compact face-normal pressure operator) once, before the first solve,
    // so the pressure matrix pattern does not switch mid-run.
    if (s.isPiso_()) {
        s.config_.rhieChow = true;
        s.applyPressureSolverConfig_();
    }
    // For adaptive we loop while time < tEnd
    while (time_ < s.config_.stoppingPolicy.transientPolicy.tEnd - 1e-12) {
        // A diverged run (NaN/inf or repeated residual growth) must stop, not
        // keep integrating garbage to tEnd while writing output.
        if (s.aborted_) break;
        // Pause/step boundary: the state below (time_, timeStep_, velocityN_,
        // velocityNM1_) always describes the previous COMPLETED step, so a
        // Cancel/Save from here is a valid restart point.
        if (!s.waitIfPaused()) break;

        if (s.config_.transient.adaptiveCfl && step > 0) {
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
        if (dt_ < s.config_.transient.minDt) dt_ = std::min(s.config_.transient.minDt, tEnd - time_);
        time_ += dt_;
        ++step;
        s.timeStep_ = step; step_ = step;

        bool isPISO = s.isPiso_();
        if (isPISO && !s.config_.rhieChow) {
            s.config_.rhieChow = true;
            s.applyPressureSolverConfig_();
        }
        int nOuter = std::max(1, s.config_.stoppingPolicy.transientPolicy.maxOuterCorrectors);
        if (isPISO) {
            // True PISO: 1 momentum predictor + up to nOuter pressure correctors
            // (frozen dCoeff). No under-relaxation, no adaptive alphaP reduction
            // inside correctors.
            performPISOPredictor_(velocity, pressure);
            // PISO residual reduction: the predictor continuity is the reference
            // for this time step. Reset it every step, never carry it over.
            pisoInitialResidual_ = s.predictedContinuity_;
            const auto& tp = s.config_.stoppingPolicy.transientPolicy;
            const int minCorrectors =
                std::min(nOuter, std::max(1, tp.minOuterCorrectors));
            const double reduction = tp.residualReduction;
            for (int corr = 0; corr < nOuter; ++corr) {
                performPISOCorrector_(velocity, pressure);
                s.updateResiduals_();
                s.lastIterations_ = step * nOuter + corr + 1;

                // Run at least minCorrectors, then stop once the continuity
                // residual has fallen to `reduction` times the start-of-step
                // value. reduction == 0 disables the test (always run to the
                // cap). A zero initial residual is already converged.
                bool reductionMet = false;
                if (reduction > 0.0) {
                    if (!std::isfinite(pisoInitialResidual_)) {
                        reductionMet = false; // no usable reference: run to cap
                    } else if (pisoInitialResidual_ <= 0.0) {
                        reductionMet = true;
                    } else {
                        reductionMet = s.lastPisoPreCorrectorResidual_ <=
                                       reduction * pisoInitialResidual_;
                    }
                }
                if (corr + 1 >= minCorrectors && reductionMet) break;
            }
            // checkConvergence_ detects NaN/inf divergence; act on it instead of
            // discarding the result and marching on to tEnd. Cancellation is
            // deliberately NOT checked here: it is honoured at the step
            // boundary, so the step we are in finishes consistently.
            if (s.checkConvergence_(static_cast<std::size_t>(step), false, false)) break;
            // Turbulence updated once per time step after PISO correctors (frozen during correctors)
            if (s.turbulence_ && s.config_.transient.enabled && dt_ > 0) {
                // One turbulence iteration with the corrected velocity
                TimeScheme ts = s.config_.transient.timeScheme;
                if (s.timeStep_ == 1 && ts == TimeScheme::BDF2) ts = TimeScheme::Euler;
                const Field<Dim>* kPrevPrev =
                    (s.timeStep_ > 1 && dtPrev_ > 0.0) ? &kNM1_ : nullptr;
                const Field<Dim>* oPrevPrev =
                    (s.timeStep_ > 1 && dtPrev_ > 0.0) ? &omegaNM1_ : nullptr;
                Field<Dim> dummyNut = s.turbulence_->nut();
                // Use current corrected velocity
                s.turbulence_->solveIterationTransient(velocity, s.config_.convectionScheme, dt_, dtPrev_, ts,
                                                       kN_, omegaN_, kPrevPrev, oPrevPrev);
                double alphaNut = nutUnderRelax;
                auto& nut = s.turbulence_->nut();
                #pragma omp parallel for schedule(static)
                for (std::size_t i = 0; i < s.mesh_.nCells(); ++i) {
                    nut[i] = alphaNut * nut[i] + (1.0 - alphaNut) * dummyNut[i];
                }
            }
        } else {
            for (int outer = 0; outer < nOuter; ++outer) {
                performTransientIteration_(velocity, pressure);
                s.updateResiduals_();
                s.lastIterations_ = step * nOuter + outer + 1;
                {
                    // Per-equation corrector early exit (SIMPLE transient path).
                    bool converged = true;
                    for (std::size_t i = 0;
                         i < s.config_.stoppingPolicy.transientPolicy.activeEquations.size();
                         ++i) {
                        double t = s.config_.stoppingPolicy.transientPolicy
                                       .effectiveTolerances[i];
                        switch (s.config_.stoppingPolicy.transientPolicy
                                    .activeEquations[i]) {
                            case ResidualEquation::Continuity:
                                converged = converged && s.lastContResid_ < t;
                                break;
                            case ResidualEquation::MomentumX:
                                converged = converged && s.lastMomResidU_ < t;
                                break;
                            case ResidualEquation::MomentumY:
                                converged = converged && s.lastMomResidV_ < t;
                                break;
                            case ResidualEquation::MomentumW:
                                converged = converged && s.lastMomResidW_ < t;
                                break;
                            case ResidualEquation::TurbulenceK:
                                converged =
                                    converged && s.turbulence_ &&
                                    s.lastKResid_ < t;
                                break;
                            case ResidualEquation::TurbulenceOmega:
                                converged =
                                    converged && s.turbulence_ &&
                                    s.lastOmegaResid_ < t;
                                break;
                        }
                    }
                    if (converged) break;
                }
            }
            if (s.checkConvergence_(static_cast<std::size_t>(step), false, false)) break;
        }
        // Roll the BDF2 history forward. dtPrev_ records the step size that
        // produced velocityN_, which is what the next step's BDF2 coefficients
        // are derived from. This must not be skipped on cancel: the time and
        // step counters were already advanced above, so stopping earlier would
        // leave the saved state describing a step that never happened.
        velocityNM1_ = velocityN_;
        s.hasVelocityHistory_ = true;
        velocityN_ = velocity;
        pressureN_ = pressure;
        dtPrev_ = dt_;
        if (s.turbulence_) {
            kNM1_ = kN_;
            omegaNM1_ = omegaN_;
            kN_ = s.turbulence_->k();
            omegaN_ = s.turbulence_->omega();
        }
        solver_.printIterationRow_(true, step, time_, dt_);
        if (s.config_.iterationCallback) {
            s.config_.iterationCallback(step, s.lastContResid_, s.lastMomResid_);
        }
        saveTransientCheckpoint_(step, time_);
        // Render callback: every step, independent of disk cadence.
        if (s.config_.renderCallback) {
            s.config_.renderCallback(step, time_, velocity, pressure);
        } else if (s.config_.transientCallback) {
            s.config_.transientCallback(step, time_, velocity, pressure);
        }
        if (step >= 1000000) break; // safety
        if (stepBudget > 0 &&
            static_cast<std::size_t>(step - firstStep) >= stepBudget) break;
        if (!s.config_.transient.adaptiveCfl && step >= nSteps) break;
    }
    s.hasRestoredTransientState_ = false;
    if (s.aborted_) {
        s.status_ = CFDSolver<Dim>::Status::Aborted;
    } else if (s.cancelFlag()->load(std::memory_order_acquire)) {
        s.status_ = CFDSolver<Dim>::Status::Cancelled;
    } else if (time_ >= tEnd - 1e-12) {
        s.status_ = CFDSolver<Dim>::Status::Completed;
    } else {
        s.status_ = CFDSolver<Dim>::Status::NotConverged;
    }
    std::cout << "Transient solve " << s.statusText()
              << ": steps=" << step << " time=" << time_ << " dt=" << dt_ << "\n";
    if (s.pPrimeLimiterHits_ > 0) {
        std::cerr << "  WARNING: p' magnitude limiter fired " << s.pPrimeLimiterHits_
                  << " time(s). This imposes an effective alphaP=alphaU<1 and breaks the\n"
                     "           PISO pressure/velocity coupling (continuity is not exactly\n"
                     "           satisfied at the end of a corrector). Consider a smaller dt, more\n"
                     "           correctors, or raising simple.pressure_correction_limit.\n";
    }
    s.linearSolverDispatcher_.printStats();
    if (s.config_.pressureSolverStats) s.printPressureSolverStats_();
    s.printFinalStats_();
}

template class TransientDriver<2>;
template class TransientDriver<3>;

} // namespace fvm
