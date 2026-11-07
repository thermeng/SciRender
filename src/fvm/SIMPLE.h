#pragma once

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
    struct TransientConfig {
        bool enabled = false;
        double dt = 0.001;
        double tEnd = 1.0;
        TimeScheme scheme = TimeScheme::Euler;
        bool adaptive = false;
        double cfl = 0.5;
        double maxDt = 0.01;
        double minDt = 1e-6;
        int writeInterval = 1;
        int maxOuterCorrectors = 3;
        // Number of inner PISO passes per outer corrector. Each pass rebuilds
        // the explicit momentum residual from the velocity the previous pass
        // produced and projects it again, so this knob is not a no-op.
        int nonOrthogonalCorrectors = 1;
        std::string pvdFile = "";
        bool piso = false; // DEPRECATED legacy: use Config::solverType==PISO instead. Kept for compat only - will be removed
    };

struct InletTurbulence {
    std::string patch;
    double k = 1e-3;
    double omega = 1.0;
};

struct Config {
    SolverType solverType = SolverType::SIMPLE; // explicit: SIMPLE (steady/transient), PISO, PIMPLE
        std::size_t maxIterations = 100;
        double tolerance = 1e-4;
        // Per-equation residual tolerances (std::nullopt = inherit from `tolerance`).
        std::optional<double> contTol;
        std::optional<double> momUTol;
        std::optional<double> momVTol;
        std::optional<double> momWTol;
        std::optional<double> turbKTol;
        std::optional<double> turbOmegaTol;
        // Whether each equation participates in the convergence gate.
        bool gateContinuity = true;
        bool gateMomU = true;
        bool gateMomV = true;
        bool gateMomW = true;
        bool gateTurbK = true;
        bool gateTurbOmega = true;
        // Fixed iteration cap override (std::nullopt = use maxIterations).
        std::optional<std::size_t> fixedIterations;
        // Ignore the residual tolerance and run exactly maxIterations iterations.
        bool runFixedIterations = false;
        // Consecutive iterations the residual criterion must hold before the run
        // is declared converged.
        int convergenceHoldIterations = 3;
        // Report residuals the way  (and Fluent) do: the continuity
        // residual is the mass imbalance of the MOMENTUM PREDICTOR - the field
        // the pressure correction is about to project - and each equation's
        // residual is divided by its maximum over the first
        // `residualNormalizationIterations` iterations. With this on the first
        // table row is ~1 and the values can be read directly against a
        //  residual plot. With it off the printed values are the raw
        // (physical) mass imbalance and linear momentum residual.
        bool normalizeResiduals = true;
        std::size_t residualNormalizationIterations = 5;
        double alphaP = 0.3;
        double alphaU = 0.7;
        std::size_t pressureRefCell = SIZE_MAX;
        ConvectionScheme convectionScheme = ConvectionScheme::Upwind;
        // Gradient operator used for the convective reconstruction.
        GradientScheme gradientScheme = GradientScheme::LeastSquares;
        bool rhieChow = true;
        bool simplec = false; // kept as requested: SIMPLE vs SIMPLEC via this flag
        // Number of pressure correctors per SIMPLE iteration (>=1).
        int pressureCorrectors = 3;
        // Bound on |p'| as a multiple of rho*U_ref^2. See limitPressureCorrection_.
        // Set high enough that the limiter rarely fires for well-posed flows;
        // the limiter is a safety net, not a convergence aid.
        bool limitPressureCorrection = true;
        double pressureCorrectionLimit = 1e6;
        std::size_t printFrequency = 1;
        std::size_t residualHistoryWindow = 20;
        bool turbulenceEnabled = false;
    bool wallFunction = false;
    double kInf = 1e-3;
    double omegaInf = 1.0;
    double alphaK = 0.7;
    double alphaOmega = 0.7;
    std::vector<InletTurbulence> inletTurbulence;

    // Velocity correction damping limits.
    double velocityCorrectionMaxUnlimitedSpeed = 20.0;
    double velocityCorrectionMaxGrowthRate = 0.15;

        // --- Pressure-correction linear solver ---------------------------
        // The pressure Poisson system is symmetric, so AMG is a good fit.
        //  always uses it, so AMG is on by default and
        // pressureAmgThreshold = 0 means "no size cut-off".
        bool pressureUseAMG = true;
        // true  -> AMG used as a preconditioner inside a Krylov solver.
        // false -> a standalone AMG solve (then pressureAmgConfig's
        //          maxIterations/tolerance below decide convergence).
        bool pressureUseAMGPreconditioner = true;
        // 0 = always build and use AMG ( behaviour).
        std::size_t pressureAmgThreshold = 0;
        int pressureAmgPreconditionerCycles = 1;
        AMGHierarchy::Config pressureAmgConfig = []{
            AMGHierarchy::Config c;
            c.strengthThreshold = 0.16;
            c.truncThreshold = 0.2;
            c.preSweeps = 1;
            c.postSweeps = 2;
            c.maxIterations = 100;
            c.verbosity = AMGHierarchy::Config::Verbosity::High;
            return c;
        }();
        // Momentum linear-solver multigrid configuration, forwarded to the
        // LinearSolverDispatcher. Tests use this to silence AMG output
        // (verbosity = None). The dispatcher's other knobs (useAMG, threshold,
        // cycles, etc.) use internal defaults and are not exposed here.
        AMGHierarchy::Config momentumAmgConfig = []{
            AMGHierarchy::Config c;
            c.strengthThreshold = 0.16;
            c.truncThreshold = 0.2;
            c.preSweeps = 1;
            c.postSweeps = 2;
            c.maxIterations = 100;
            c.verbosity = AMGHierarchy::Config::Verbosity::High;
            c.acceleration = AMGHierarchy::Config::Acceleration::None;
            return c;
        }();
        // Number of AMG cycles per preconditioner application for momentum.
        int momentumAmgCycles = 1;
        // Relative residual above which the momentum AMG result is rejected
        // and the solve is redone with ILU.
        double momentumAmgMaxResidual = 0.1;
        // Apply the AMG hierarchy directly as the momentum solver instead of
        // using it as a preconditioner inside a Krylov solver.
        bool momentumAmgAsSolver = false;
        // Print momentum linear-solver statistics at the end of the run.
        bool momentumSolverStats = false;
        // Turbulence (k/omega) linear-solver multigrid configuration, forwarded
        // to SSTModel's LinearSolverDispatcher instances. Defaults use low
        // verbosity so turbulence solver output doesn't drown out the momentum
        // and pressure diagnostics.
        AMGHierarchy::Config turbulenceAmgConfig = []{
            AMGHierarchy::Config c;
            c.strengthThreshold = 0.16;
            c.truncThreshold = 0.2;
            c.preSweeps = 1;
            c.postSweeps = 2;
            c.maxIterations = 100;
            c.verbosity = AMGHierarchy::Config::Verbosity::None;
            c.acceleration = AMGHierarchy::Config::Acceleration::None;
            return c;
        }();
        // Number of AMG cycles per preconditioner application for turbulence.
        int turbulenceAmgCycles = 1;
        // Relative residual above which the turbulence AMG result is rejected
        // and the solve is redone with ILU.
        double turbulenceAmgMaxResidual = 1e-8;

        std::function<void(int, double, double)> iterationCallback;
        std::function<void(int, double, const VectorField<Dim>&, const Field<Dim>&)> renderCallback;
        std::function<void(int, double, const VectorField<Dim>&, const Field<Dim>&)> transientCallback;
        CheckpointConfig checkpoint;
        TransientConfig transient;

        // A configuration that mirrors the  129x129 cavity run:
        // one pressure corrector, AMG for both equations, a V cycle with CG
        // acceleration for the pressure system and a variable (flex) cycle with
        // Gauss-Seidel smoothing for momentum, and loose linear tolerances (0.1)
        // that leave the outer loop to converge the solution. Use it as a
        // starting point and override the physics settings (rho, nu, BCs) as
        // needed.
        static Config starCcmPlusLike() {
            Config c;
            c.pressureCorrectors = 1;
            c.pressureUseAMG = true;
            c.pressureUseAMGPreconditioner = true;
            c.pressureAmgThreshold = 0;
            c.pressureAmgPreconditionerCycles = 1;
            c.pressureAmgConfig.preSweeps = 1;
            c.pressureAmgConfig.postSweeps = 5;
            c.pressureAmgConfig.coarsening = AMGHierarchy::Config::Coarsening::SmoothedAggregation;
            c.pressureAmgConfig.smoother = AMGHierarchy::Config::Smoother::GaussSeidel;
            c.pressureAmgConfig.coarseEnough = 2;
            c.pressureAmgConfig.cycleType = AMGHierarchy::Config::CycleType::VCycle;
            c.pressureAmgConfig.acceleration = AMGHierarchy::Config::Acceleration::ConjugateGradient;
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
    // True when every gated equation's scaled residual is below its own
    // tolerance (inheriting `tolerance` when the per-equation value is nullopt).
    bool residualsConverged_() const;
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
    // Normalization references: max raw residual over the first N iterations,
    // exactly the /Fluent convention (iteration 1 is ~1).
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
    std::size_t divergenceCount_ = 0;
    double initialMassImbalance_ = 0.0;
    bool initialMassImbalanceSet_ = false;

    // Momentum snapshots kept for the PISO correctors and per-direction
    // residual evaluation. momA_/momB_ are now locals in the component
    // assemblers; these hold the last-assembled system per component.
    mutable std::array<SparseMatrix, Dim> momAsnap_;
    mutable std::array<Eigen::VectorXd, Dim> momBsnap_;
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


