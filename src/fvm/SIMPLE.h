#pragma once

#include "cfd/cfd_types.h"
#include "cfd/convergence_policy.h"
#include "mesh/Mesh.h"
#include "mesh/BoundaryUtils.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/Cancellable.h"
#include "fvm/SIMPLETypes.h"
#include "fvm/SIMPLEBoundaryConditions.h"
#include "fvm/Momentum.h"
#include "fvm/Divergence.h"
#include "fvm/SSTModel.h"
#include "fvm/Gradient.h"
#include "fvm/PressureCorrection.h"
#include "fvm/LinearSolverDispatcher.h"
#include "fvm/AMG.h"
#include "fvm/Residual.h"
#include "fvm/VelocityCorrection.h"
#include "io/Checkpoint.h"


#include <Eigen/Sparse>
#include <Eigen/IterativeLinearSolvers>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fvm {

template<std::size_t Dim>
class SIMPLE : public Cancellable {
public:
    using TransientConfig = cfd::TransientSettings;

struct InletTurbulence {
    std::string patch;
    double k = 1e-3;
    double omega = 1.0;
};

struct Config : public cfd::CaseSettings {
    Config& operator=(const cfd::CaseSettings& s) {
        static_cast<cfd::CaseSettings&>(*this) = s;
        return *this;
    }
    // Freestream turbulence. Not part of CaseSettings: the session sets these
    // from Initialize(), and enableTurbulence() copies them in.
    double kInf = 1e-3;
    double omegaInf = 1.0;
    SolverType solverType = SolverType::SIMPLE; // explicit: SIMPLE (steady/transient), PISO, PIMPLE
    ConvectionScheme convectionScheme = ConvectionScheme::Upwind;
    GradientScheme gradientScheme = GradientScheme::LeastSquares;
    bool pressureUseAMG = true;
    bool pressureUseAMGPreconditioner = true;
    std::size_t pressureAmgThreshold = 0;
    // Override CaseSettings defaults where the solver needs different values:
    int pressureCorrectors = 3;           // CaseSettings: 1
    bool limitPressureCorrection = true;   // CaseSettings: false
    double pressureCorrectionLimit = 1e6;  // CaseSettings: 5.0
    std::size_t pressureRefCell = SIZE_MAX; // CaseSettings: 0
    double turbulenceAmgMaxResidual = 1e-8; // CaseSettings: 0.1
    std::vector<InletTurbulence> inletTurbulence;

    std::function<void(int, double, double)> iterationCallback;
        std::function<void(int, double, const VectorField<Dim>&, const Field<Dim>&)> renderCallback;
        std::function<void(int, double, const VectorField<Dim>&, const Field<Dim>&)> transientCallback;
        CheckpointConfig checkpoint;
        TransientConfig transient;
        // Resolved stopping policy (populated by solver_config_bridge). The
        // iteration loop consults this instead of raw optional fields.
        cfd::ResolvedStoppingPolicy stoppingPolicy;

        // A configuration that mirrors the  129x129 cavity run:
        // one pressure corrector, AMG for both equations, a V cycle with CG
        // acceleration for the pressure system and a variable (flex) cycle with
        // Gauss-Seidel smoothing for momentum, and loose linear tolerances (0.1)
        // that leave the outer loop to converge the solution. Use it as a
        // starting point and override the physics settings (rho, nu, BCs) as
        // needed.
        static Config cavityLike() {
            Config c;
            c.pressureCorrectors = 1;
            c.pressureUseAMG = true;
            c.pressureUseAMGPreconditioner = true;
            c.pressureAmgThreshold = 0;
            c.pressureAmgPreconditionerCycles = 1;
            c.pressureAmg.preSweeps = 1;
            c.pressureAmg.postSweeps = 1;
            c.pressureAmg.coarsening = cfd::AmgSettings::Coarsening::SmoothedAggregation;
            c.pressureAmg.smoother = cfd::AmgSettings::Smoother::GaussSeidel;
            c.pressureAmg.coarseEnough = 2;
            c.pressureAmg.cycleType = cfd::AmgSettings::CycleType::VCycle;
            c.pressureAmg.acceleration = cfd::AmgSettings::Acceleration::ConjugateGradient;
            return c;
        }
    };

    using VelocityBCType = ::fvm::VelocityBCType;
    using VelocityBC = ::fvm::VelocityBC;
    using VelocityPatch = ::fvm::VelocityPatch;
    using FaceVelocityBC = ::fvm::FaceVelocityBC;

    void setVelocityBCOnFacesWithSlip(const std::vector<std::size_t>& faceIndices,
                                      const std::string& typeStr,
                                      double u, double v, double w,
                                      bool slip,
                                      double pressure = 0.0);

    SIMPLE(const Mesh<Dim>& mesh, double rho, double nu, const Config& config = Config{});

    void setVelocityBC(std::size_t d, int sign, VelocityBCType type,
                       double val1 = 0.0, double val2 = 0.0, double val3 = 0.0);
    void setMin(std::size_t d, VelocityBCType type, double u = 0.0, double v = 0.0, double w = 0.0);
    void setMax(std::size_t d, VelocityBCType type, double u = 0.0, double v = 0.0, double w = 0.0);

    void addVelocityPatch(const std::string& name, std::size_t d, int sign,
                          VelocityBCType type, double u = 0.0, double v = 0.0, double w = 0.0);

    const VelocityPatch* findVelocityPatch(const std::string& name) const;
    const std::vector<VelocityPatch>& velocityPatches() const;

    void setVelocityBCOnFaces(const std::vector<std::size_t>& faceIndices,
                               VelocityBCType type,
                               double u = 0.0, double v = 0.0, double w = 0.0);

    void setPressureOutlet(std::size_t d, int sign, double pressure);
    void setPressureOutletOnFaces(const std::vector<std::size_t>& faceIndices, double pressure);
    void setPressureOutletOnFacesWithBackflow(const std::vector<std::size_t>& faceIndices, double pressure,
                                              bool suppressBackflow, double u, double v, double w,
                                              double k, double omega);

    void enableTurbulence(double kInf = 1e-3, double omegaInf = 1.0,
                          double alphaK = 0.7, double alphaOmega = 0.7,
                          bool wallFunction = false);

    void solve(VectorField<Dim>& velocity, Field<Dim>& pressure);
    void solveTransient(VectorField<Dim>& velocity, Field<Dim>& pressure);

    // Why the run ended.
    //
    // A run that reaches maxIterations without meeting the tolerance is NOT a
    // converged solution, and the difference has to be visible: the caller
    // (the application, a test) must be able to tell a steady state from a
    // field that merely stopped changing because the iteration budget ran
    // out. Before this existed every run reported "complete", so a diverged
    // or unconverged result was presented exactly like a converged one.
    enum class Status {
        NotConverged,  // the iteration budget ran out before the tolerance
        Converged,     // the tolerance was met (and held) before the budget
        Completed,     // a transient run reached its end time
        Aborted,       // NaN/Inf residual or a detected divergence
        Cancelled      // the user cancelled the run
    };
    // Why an aborted run stopped. `None` for every other status, so a caller
    // can tell "ran out of iterations" from "the numerics broke" without
    // scraping the log output.
    enum class Failure {
        None,
        NonFiniteContinuity,   // the continuity residual became NaN/Inf
        DivergingContinuity,   // the continuity residual grew for too long
        NonFiniteTurbulence,   // the k/omega transport produced NaN/Inf
        MomentumSolveFailed,   // a momentum linear solve broke down
        PressureSolveFailed    // a pressure linear solve broke down
    };
    Status status() const { return status_; }
    bool converged() const { return status_ == Status::Converged; }
    Failure failure() const { return failure_; }
    // A short human-readable description of the status, for the UI and the
    // solver's own summary.
    const char* statusText() const;
    // Human-readable description of failure(); empty for Failure::None.
    const char* failureText() const;

    void bindVelocity(VectorField<Dim>& velocity);
    void bindPressure(Field<Dim>& pressure);
    VectorField<Dim>& boundVelocity();
    const VectorField<Dim>& boundVelocity() const;
    Field<Dim>& boundPressure();
    const Field<Dim>& boundPressure() const;

    // Everything needed to resume a run from a saved case. The solver's
    // initializeSolve_ applies it *after* the turbulence model exists, so the
    // restored k/omega/nut are not overwritten by the kInf/omegaInf defaults.
    struct RestartState {
        // Current fields (optional: a case may carry settings but no result).
        bool hasVelocity = false;
        std::array<std::vector<double>, Dim> velocity{};
        std::vector<double> pressure;

        // Current turbulence fields (optional).
        bool hasTurbulence = false;
        std::vector<double> k;
        std::vector<double> omega;
        std::vector<double> nut;

        // Transient state.
        bool hasTransient = false;
        double time = 0.0;
        double dt = 0.0;
        double dtPrev = 0.0;
        int step = 0;

        // BDF2 history.
        bool hasVelocityHistory = false;
        std::array<std::vector<double>, Dim> velocityNM1{};
        bool hasTurbulenceHistory = false;
        std::vector<double> kNM1;
        std::vector<double> omegaNM1;
    };

    // Queued for the next solve. initializeSolve_ consumes and clears it, so a
    // later Initialize/Run can never re-apply a stale mid-run state.
    void setRestartState(RestartState state) { restartState_ = std::move(state); }
    bool hasRestartState() const { return restartState_.has_value(); }

    const Field<Dim>& pressure() const;
    double currentTime() const { return time_; }
    double currentDt() const { return dt_; }
    double currentDtPrev() const { return dtPrev_; }
    int currentStep() const { return timeStep_; }
    const VectorField<Dim>& velocityNM1() const { return velocityNM1_; }
    bool hasTransientState() const { return hasRestoredTransientState_ || timeStep_ > 0 || time_ > 0.0; }
    const Field<Dim>& kNM1() const { return kNM1_; }
    const Field<Dim>& omegaNM1() const { return omegaNM1_; }
    int lastIterations() const;
    // Apply a resolved stopping policy at run start, and again whenever the
    // user edits stopping criteria while the run is paused at a settled
    // boundary. Replaces the current policy and resets the hold streak.
    bool updateStoppingPolicy(const cfd::ResolvedStoppingPolicy& policy);
    // Raw (un-normalized) continuity residual: the mass imbalance of the
    // momentum predictor, sum|div|*V / sum|flux|. This is the quantity the
    // pressure correction has to remove, not the post-correction divergence
    // (which is a linear-solver residual and is ~1e-15).
    double finalContinuityResidual() const;
    // The residual as printed in the iteration table: divided by the maximum
    // over the first `residualNormalizationIterations` iterations when
    // Config::normalizeResiduals is on.
    double finalNormalizedContinuityResidual() const;
    // Post-correction divergence diagnostic: how much mass imbalance is left
    // AFTER the pressure correction. Near machine precision by construction,
    // so it is NOT a convergence measure.
    double finalMassImbalance() const;
    double finalMomentumResidual() const;
    // Per-direction momentum linear residuals, in the same order as the
    // "x-velocity"/"y-velocity" columns of the iteration table. The summary
    // "momentum residual" is the max of these.
    std::array<double, Dim> finalMomentumResiduals() const {
        std::array<double, Dim> r{};
        r[0] = lastMomResidU_;
        if constexpr (Dim >= 2) r[1] = lastMomResidV_;
        if constexpr (Dim >= 3) r[2] = lastMomResidW_;
        return r;
    }

    // Momentum AMG diagnostics (for tests / profiling).
    std::size_t momentumAmgSolves() const { return linearSolverDispatcher_.amgSolves(); }
    std::size_t momentumAmgStandaloneSolves() const {
        return linearSolverDispatcher_.amgStandaloneSolves();
    }
    std::size_t momentumAmgRejected() const { return linearSolverDispatcher_.amgRejected(); }
    std::size_t momentumAmgBuilds() const { return linearSolverDispatcher_.amgBuilds(); }
    // Per-outer-iteration relative velocity change. Diagnostic only: it tracks
    // iteration progress rather than equation satisfaction.
    double finalMomentumChange() const { return lastMomChange_; }
    SSTModel<Dim>* turbulence();
    const SSTModel<Dim>* turbulence() const;

private:
    const Field<Dim>& zeroSource_() const;

    void solveMomentum_(VectorField<Dim>& uStar, const VectorField<Dim>& velocityOld);
    void solveMomentumComponent_(std::size_t d, VectorField<Dim>& uStar,
                                 const VectorField<Dim>& velocityOld,
                                 const std::array<Field<Dim>, Dim>& pressureGrad);
    void solveMomentumTransient_(VectorField<Dim>& uStar, const VectorField<Dim>& velocityOld);
    void solveMomentumComponentTransient_(std::size_t d, VectorField<Dim>& uStar,
                                         const VectorField<Dim>& velocityOld,
                                         const std::array<Field<Dim>, Dim>& pressureGrad);

    void initializeSolve_(VectorField<Dim>& velocity, Field<Dim>& pressure);
    // Rebuild the pressure-correction linear-solver configuration from
    // Config. Called at construction and again whenever the transient path
    // forces Rhie-Chow on, so the two sites cannot drift apart.
    void applyPressureSolverConfig_();
    void performIteration_(VectorField<Dim>& velocity, Field<Dim>& pressure, std::size_t iter);
    void performTransientIteration_(VectorField<Dim>& velocity, Field<Dim>& pressure);
    // True PISO: one momentum predictor, then pressure correctors that
    // reconstruct the explicit momentum residual H(u) from the corrected
    // velocity before each projection. The momentum operator and its
    // pressure-free right-hand side are frozen for the time step.
    void performPISOPredictor_(VectorField<Dim>& velocity, Field<Dim>& pressure);
    void performPISOCorrector_(VectorField<Dim>& velocity, Field<Dim>& pressure);
    // Rebuild the corrector predictor u~ from the current velocity and
    // pressure: u~ = H(u)/aP - d grad(p), i.e. the momentum equation with
    // the off-diagonal terms treated explicitly at the current velocity.
    // Writes u~ into uStar_. This is what makes successive PISO correctors
    // act on a non-zero momentum residual instead of repeating the same
    // projection.
    void updatePisoPredictor_();
    bool isPiso_() const;
    // Pin every pressure outlet to its prescribed reference pressure. The
    // face-based list is exact per face; the cell list is the fallback for
    // meshes whose outlets are described by cells only. All three pressure
    // correction paths (steady corrector, transient iteration, PISO corrector)
    // must apply the same rule, so they share this helper.
    void applyPressureOutletReference_(Field<Dim>& pressure) const;
    void updateResiduals_();
    // Capture the mass imbalance of the momentum predictor u* (the field the
    // pressure correction is about to project). Called by every predictor path
    // right after the momentum solve, before the correctors touch the field.
    void capturePredictedContinuity_();
    // Normalised linear residual |A u - b| / sum|b| of the momentum system
    // for component d, evaluated at the current velocity. Uses the
    // per-direction snapshot, not the shared (last-component) momA_/momB_.
    double momentumResidual_(std::size_t d) const;
    void printPressureSolverStats_() const;
    // Returns true when the caller should stop solving. `checkTolerance` gates
    // the "residuals are below tolerance" clause: the steady path wants it, the
    // transient path does not — a converged corrector loop at one time step
    // says nothing about whether the transient simulation is finished, so a
    // transient run must keep marching to tEnd (NaN/divergence and Cancel still
    // stop it in both modes).
    bool checkConvergence_(std::size_t iter, bool checkTolerance = true,
                           bool checkCancel = true);
    // Resolve config_.stopping into config_.stoppingPolicy for the current
    // dimension/turbulence/transient context. Called at the start of every run
    // so tests and direct Config construction work without going through the
    // bridge; mid-run edits go through updateStoppingPolicy().
    void applyStoppingPolicy_();
    bool aborted() const { return aborted_; }
    void saveCheckpoint_(std::size_t iter);
    void saveCheckpoint_(std::size_t iter, const TransientBlock<Dim>* transient);
    void saveTransientCheckpoint_(int step, double time);
    void printIterationOutput_(std::size_t iter);
    void printTransientOutput_(int step, double time);
    void printFinalStats_() const;
    void printHeader_(bool isTransient) const;
    mutable int headerCounter_ = 0; // for repeating header every N lines
    double computeAdaptiveDt_() const;
    // Refresh the reference driving speed. Must be re-run after any BC is set,
    // otherwise the clamp/limiter scales are derived from a stale value.
    void updateMaxInletSpeed_();
    void updatePVDFile_();

    // Scale p' down if it exceeds limit * rho * U_ref^2. This is NOT a no-op:
    // it rescales p' uniformly, so it silently imposes an effective
    // alphaP = alphaU = scale < 1 and breaks the PISO pressure/velocity
    // coupling (continuity is no longer satisfied at the end of the corrector).
    // Gated by config_.limitPressureCorrection so the trade-off is explicit,
    // and counted in pPrimeLimiterHits_ so it can be reported.
    bool limitPressureCorrection_(Field<Dim>& pPrime);

    const Mesh<Dim>& mesh_;
    double rho_, nu_;
    Config config_;

    Momentum<Dim> momentum_;
    PressureCorrection<Dim> pressureCorrection_;

    Field<Dim> pPrime_;
    Field<Dim> divBuffer_;
    Field<Dim> pressureBeforeCorrection_;
    std::array<Field<Dim>, Dim> pressureGradBefore_{};
    VectorField<Dim> uStar_;
    std::array<Field<Dim>, Dim> dpdx_{};
    std::array<Field<Dim>, Dim> diag_{};
    std::array<std::vector<double>, Dim> dCoeff_{};

    std::unique_ptr<Gradient<Dim>> gradientCalc_;
    // Least-squares pressure gradient used by the transient momentum
    // predictor, the PISO corrector and the pressure correction's Rhie-Chow
    // term. It is deliberately independent of gradientCalc_ (which follows
    // Config::gradientScheme and is used for the convective reconstruction):
    // the pressure operator must be the same one the pressure matrix and the
    // velocity correction are built from.
    std::unique_ptr<Gradient<Dim>> pressureGradientCalc_;
    // Least-squares gradient of p' for the velocity correction. Reused across
    // correctors instead of constructing a Gradient (and its workspace) per call.
    std::unique_ptr<Gradient<Dim>> pPrimeGradientCalc_;

    std::optional<std::reference_wrapper<VectorField<Dim>>> velocity_;
    std::optional<std::reference_wrapper<Field<Dim>>> pressure_;

    SIMPLEBoundaryConditions<Dim> bc_;
    BCDiscretization bcDiscretization_;

    int lastIterations_ = 0;
    double lastContResid_ = 0.0;      // reported (normalized when enabled)
    double lastContResidRaw_ = 0.0;   // pre-correction mass imbalance
    double lastContResidPost_ = 0.0;  // post-correction mass imbalance
    // Mass imbalance of u* captured right after the momentum predictor and
    // consumed by updateResiduals_(). Measuring it here is what makes the
    // continuity residual a nonlinear convergence measure: after the pressure
    // corrector the discrete divergence is zero to linear-solver accuracy
    // (with more than one corrector: to machine precision), which is why the
    // old post-correction value sat at ~1e-15 for the whole run.
    double predictedContinuity_ = 0.0;
    double contResidRef_ = 0.0;
    std::size_t contResidRefSamples_ = 0;
    std::array<double, Dim> momResidRaw_{};
    std::array<double, Dim> momResidRef_{};
    std::size_t momResidRefSamples_ = 0;
    double lastMomResid_ = 0.0;
    double lastMomResidU_ = 0.0;
    double lastMomResidV_ = 0.0;
    double lastMomResidW_ = 0.0;
    // Per-outer-iteration relative velocity change. Diagnostic only: it tracks
    // iteration progress, not how well the momentum equation is satisfied.
    double lastMomChange_ = 0.0;
    std::unique_ptr<SSTModel<Dim>> turbulence_;
    bool aborted_ = false;
    // First hard failure of the run; see Failure. A later failure never
    // overwrites it, so the reported cause is the one that actually happened
    // first rather than whichever check ran last.
    Failure failure_ = Failure::None;
    void fail_(Failure reason);
    double adaptiveAlphaP_ = 0.0;
    double maxInletSpeed_ = 0.0;
    double prevContResid_ = 0.0;
    std::size_t stallCount_ = 0;
    // Number of consecutive iterations the residual criterion has held.
    int convergedStreak_ = 0;
    // Convergence monitor built from the resolved stopping policy at run start
    // (and reset whenever the policy is updated mid-run).
    std::unique_ptr<cfd::ConvergenceMonitor> convergenceMonitor_;
    std::size_t startIteration_ = 0;
    // Restart payload queued by setRestartState(), consumed by initializeSolve_.
    std::optional<RestartState> restartState_;
    std::vector<double> residualHistory_;
    // Steady-path wall-time accumulators (seconds).
    double steadyTotalSeconds_ = 0.0;
    double steadyMomentumSeconds_ = 0.0;
    double steadyPredictedContinuitySeconds_ = 0.0;
    double steadyPressureSeconds_ = 0.0;
    double steadyVelocityCorrectionSeconds_ = 0.0;
    double steadyResidualSeconds_ = 0.0;
    // Wall-clock time at the start of the current outer iteration, used for
    // the "time/iter" column of the iteration table under Diagnosis verbosity.
    std::chrono::steady_clock::time_point m_iterStart;
    std::size_t divergenceCount_ = 0;
    double initialMassImbalance_ = 0.0;
    bool initialMassImbalanceSet_ = false;

    // Momentum snapshots kept for the PISO correctors and per-direction
    // residual evaluation. The component assemblers build the operator and
    // right-hand side IN PLACE in these, so no per-iteration copy is needed.
    mutable std::array<SparseMatrix, Dim> momAsnap_;
    mutable std::array<Eigen::VectorXd, Dim> momBsnap_;
    // Warm start for the momentum solve, reused across components/iterations.
    mutable Eigen::VectorXd momentumWarmStart_;
    // Pressure-free momentum RHS (b + V grad p at the predictor) per
    // direction. Frozen with momAsnap_/momBsnap_ for the PISO correctors.
    std::array<Eigen::VectorXd, Dim> momB0_;
    // Temporary output of computeDCoeffAndRelax; copied into diag_[d].
    mutable Field<Dim> momDiag_;
    // Reusable per-component temporaries (avoid reallocation each iteration).
    mutable std::vector<Triplet> momTriplets_;
    mutable std::array<Field<Dim>, Dim> velGrad_;
    mutable std::array<Field<Dim>, Dim> pressureGrad_;

    // Linear solver dispatch: AMG hierarchies, BiCGSTAB+ILU fallback,
    // warm-start handling, and the AMG-vs-ILU decision. The SIMPLE orchestrator
    // no longer knows about sparsity hashes, rebuild intervals, or preconditioner
    // cycles — it simply asks the dispatcher to solve A x = b.
    LinearSolverDispatcher<Dim> linearSolverDispatcher_;

    VectorField<Dim> velocityOld_;
    Field<Dim> nutOld_;

    // Transient state
    double time_ = 0.0;
    double dt_ = 0.0;
    // Previous step size. BDF2 coefficients are derived from dt_/dtPrev_; 0
    // means "no usable history" and forces explicit Euler for this step.
    double dtPrev_ = 0.0;
    int timeStep_ = 0;
    bool hasRestoredTransientState_ = false;
    bool hasVelocityHistory_ = false;
    bool restarting_ = false;
    VectorField<Dim> velocityN_;
    VectorField<Dim> velocityNM1_;
    Field<Dim> pressureN_;
    std::vector<std::string> pvdEntries_;
    std::string pvdBaseName_;
    // Turbulence transient storage
    Field<Dim> kN_;
    Field<Dim> omegaN_;
    Field<Dim> kNM1_;
    Field<Dim> omegaNM1_;
    // Set when the p' magnitude limiter had to clip, so the run can be flagged.
    std::size_t pPrimeLimiterHits_ = 0;
    // Why the run ended (see Status).
    Status status_ = Status::NotConverged;

    // Cached zero source to avoid O(Dn) alloc per iteration (was Field(mesh,0) per call)
    mutable Field<Dim> zeroSourceCache_;
    mutable bool zeroSourceInitialized_ = false;
};

extern template class SIMPLE<2>;
extern template class SIMPLE<3>;

} // namespace fvm


