#pragma once
#include <string>
#include <vector>
#include <cstddef>
#include <optional>

#include "fvm/Types.h"
#include "cfd/stopping_criteria.h"

namespace cfd {

enum class BCType { VelocityInlet, PressureOutlet, Outlet, WallNoSlip, SlipWall, Symmetry, MovingWall };
enum class VelocitySpec { Components, Normal };

struct PatchBC {
    std::string patchName;
    BCType type = BCType::WallNoSlip;
    double u = 0.0, v = 0.0, w = 0.0;
    VelocitySpec velocitySpec = VelocitySpec::Components;
    double normalVelocity = 0.0;   // signed; + = inflow along -outward normal
    double pressure = 0.0;
    bool suppressBackflow = false;
    double backflowU = 0.0, backflowV = 0.0, backflowW = 0.0;
    double backflowK = 0.0, backflowOmega = 0.0;
    // Turbulence inlet values (used for velocity inlets). When
    // turbulenceIntensity > 0 the transport quantities are derived from it and
    // turbulentViscosityRatio (k = 1.5 (I U)^2, omega = k / (ratio * nu));
    // otherwise the stored kInlet/omegaInlet are used directly.
    double kInlet = 1e-3, omegaInlet = 1.0;
    double turbulenceIntensity = 0.05;      // 0..1
    double turbulentViscosityRatio = 1.0;   // nut / nu
};

// Normal mode is only meaningful for a velocity inlet.
inline bool usesNormalVelocity(const PatchBC& bc) {
    return bc.type == BCType::VelocityInlet &&
           bc.velocitySpec == VelocitySpec::Normal;
}

// Single source of truth for AMG linear solver configuration.
// Used by the UI, case file I/O, and the solver (fvm::AMGHierarchy) directly.
// Plain enums so the UI layer never has to include Eigen.
//
// The property names in comments are the AMG linear solver labels
// this dialog mirrors.
struct AmgSettings {
    // --- Solver ----------------------------------------------------------
    enum class Verbosity { None, Low, High, Diagnosis };
    enum class CycleType { VCycle, WCycle, FCycle, Flexible };
    enum class Acceleration { None, ConjugateGradient, BiCGStab, GMRES };
    enum class Smoother { Jacobi, GaussSeidel, ILU };

    int maxIterations = 30;     // Maximum cycle
    double tolerance = 0.1;     // Convergence tolerance
    Verbosity verbosity = Verbosity::None;
    CycleType cycleType = CycleType::VCycle;
    Acceleration acceleration = Acceleration::ConjugateGradient;
    Smoother smoother = Smoother::GaussSeidel;

    // --- Smoothing / hierarchy -------------------------------------------
    int preSweeps = 1;           // Pre-smooths
    int postSweeps = 1;          // Post-smooths
    int maxLevels = 50;          // Maximum number of levels

    // --- Expert -----------------------------------------------------------
    double stagnationFactor = 1.05;
    int stagnationMinIter = 10;
    double stagnationLooseFactor = 5.0;
    // Flexible-cycle tolerances:
    //   flexAlpha = β (restriction): if R_i > β*R_{i-1} (reduction too slow),
    //     visit the next coarser level. Default 0.7.
    //   flexBeta  = α (termination): a level is converged when R_i < α*R_0
    //     (residual below 10% of the level's initial residual). Default 0.1.
    // (The names are historical; flexAlpha is β, flexBeta is α.)
    double flexAlpha = 0.7;
    double flexBeta = 0.1;
    int flexMaxSweeps = 2;
    int flexMaxFineRelaxations = 30;
    int flexMaxCoarseRelaxations = 50;

    // --- Smoothed Aggregation prolongation -------------------------------
    // When true, the tentative piecewise-constant prolongation T is smoothed
    // before use: P = (I - omega * D^-1 * A)^degree * T (Jacobi smoothing).
    // This spreads each coarse basis function to neighbors of its aggregate,
    // letting the coarse grid represent smooth error modes that piecewise-
    // constant injection cannot. The near-nullspace constraint P*B = T*B is
    // preserved (constant mode interpolated exactly).
    bool smoothedProlongation = false;
    double saOmega = 2.0 / 3.0;
    int saDegree = 1;

    static AmgSettings pressureDefaults() {
        AmgSettings s;
        s.maxIterations = 30;
        s.tolerance = 0.1;
        s.cycleType = CycleType::VCycle;
        // Same flexible-cycle defaults as momentum/turbulence, so the shared
        // Multigrid panel shows one truthful set of values for every row.
        // Dormant while pressure uses a V-Cycle.
        s.flexMaxSweeps = 2;
        s.flexMaxFineRelaxations = 30;
        s.flexMaxCoarseRelaxations = 50;
        s.preSweeps = 1;
        s.postSweeps = 1;
        s.maxLevels = 50;
        s.smoother = Smoother::GaussSeidel;
        s.acceleration = Acceleration::ConjugateGradient;
        return s;
    }
    static AmgSettings momentumDefaults() {
        AmgSettings s;
        s.maxIterations = 30;
        s.tolerance = 0.1;
        s.preSweeps = 1;
        s.postSweeps = 1;
        s.maxLevels = 50;
        // GUI default: flexible cycle, 2 sweeps, 30/50 fine/coarse relaxations.
        s.cycleType = CycleType::Flexible;
        s.flexMaxSweeps = 2;
        s.flexMaxFineRelaxations = 30;
        s.flexMaxCoarseRelaxations = 50;
        s.smoother = Smoother::GaussSeidel;
        s.acceleration = Acceleration::None;
        return s;
    }
};

// Single source of truth for transient solver configuration.
// Used by the UI, case file I/O, and the solver (fvm::SIMPLE) directly.
// Plain enums so the UI layer never has to include fvm headers beyond Types.
//
// These defaults are the values the solver starts from, so the UI, the solver
// and "Restore defaults" cannot disagree.
struct TransientSettings {
    // The solver's TimeScheme::Euler is implicit, i.e. backward Euler.
    // false = steady: the transient controls are not shown and not consulted.
    bool enabled = false;
    // Initial step size. Always clamped into [minDt, maxDt], which applies to
    // the fixed-step case too, not just adaptive.
    double dt = 0.001;
    fvm::TimeScheme timeScheme = fvm::TimeScheme::Euler;
    bool adaptiveCfl = false;
    double cfl = 0.5; // only consulted when adaptiveCfl is on
    double minDt = 1e-6;
    double maxDt = 0.01;
    // Output cadence in time steps; 0 disables file output entirely.
    int writeInterval = 1;
    // Non-orthogonal correctors per pressure solve (PISO).
    int nonOrthogonalCorrectors = 1;
    // PISO legacy switch (DEPRECATED: use solverType==PISO instead).
    bool piso = false;
    // Base name or full path for the .pvd/.vtu output. Empty = "transient".
    std::string pvdFile;
};

struct CaseSettings {
    double rho = 1.0, nu = 0.01;
    // Selected material ("Air", "Water", a user-library name, or "Custom").
    // rho/nu above remain the solver's source of truth; when a library
    // material is selected they carry its properties. Empty or "Custom"
    // means hand-typed values (also the behaviour of pre-material case files).
    std::string materialName;
    StoppingCriteria stopping = defaultStoppingCriteria();
    bool normalizeResiduals = true;
    std::size_t residualNormalizationIterations = 5;
    double alphaU = 0.7, alphaP = 0.3;
    bool simplec = false;
    int scheme = 2;             // 0 upwind, 1 central, 2 second-order upwind
    int gradientScheme = 0;     // 0 least squares, 1 green-gauss cell based, 2 green-gauss node based
    bool turbulence = false;
    bool wallFunction = false; // legacy: false -> LowRe, true -> AllYPlus
    // 0 LowRe, 1 HighRe, 2 AllYPlus; -1 means derive from legacy wallFunction.
    int wallTreatment = -1;
    // Resolved value shared by the session and the solver.
    int resolvedWallTreatment() const {
        return wallTreatment >= 0 ? wallTreatment : (wallFunction ? 2 : 0);
    }
    double alphaK = 0.8, alphaOmega = 0.8;

    // Gradient (slope) limiter for the second-order upwind reconstruction.
    // Mirrors fvm::SlopeLimiter::Kind / Aggregation / Span as plain ints so the
    // UI and case-file layers never need to include fvm headers.
    //   limiterKind      : 0 MinMod, 1 Venkatakrishnan, 2 Cubic
    //   limiterAggregation: 0 CellWide, 1 ComponentWise
    //   limiterSpan      : 0 FaceCentre, 1 CentroidLine
    //   limiterK         : Venkatakrishnan smoothing, eps = k*(hi-lo)
    //   cubicTransition  : Cubic transition point where limiter -> 1 (1..2)
    int limiterKind = 1;              // Venkatakrishnan (default; smoother than MinMod)
    int limiterAggregation = 0;       // CellWide (default)
    int limiterSpan = 0;              // FaceCentre (default)
    double limiterK = 0.1;            // Venkatakrishnan smoothing constant
    double cubicTransition = 1.5;     // Cubic transition point where limiter -> 1

    // Advanced SIMPLE settings
    std::size_t pressureRefCell = 0; // 0 = auto: boundary-adjacent cell near domain corner
    int pressureCorrectors = 1;
    bool limitPressureCorrection = false;
    double pressureCorrectionLimit = 5.0;
    std::size_t printFrequency = 1;
    std::size_t residualHistoryWindow = 20;
    // Append the process resident-set size to every transient row. Off by
    // default: it exists to watch a long run for memory drift, and samples the
    // OS once per time step, which is not part of normal solver output.
    bool printResidentSet = false;
    // Append the per-iteration wall time as the last column of the residual
    // table. Off by default: it is a diagnostic, not normal solver output.
    bool printIterationTime = false;
    // Print the per-phase [timing] breakdown at the end of a run. Off by
    // default: it exists to attribute a slow run, not for normal output.
    bool printTotalTime = false;

    // Velocity correction damping limits.
    // Per-iteration growth rate is capped at maxGrowthRate (e.g. 0.15 = 15%).
    // This limit always applies regardless of absolute speed: even at low
    // speeds, pressure correction can inject explosive growth (as seen at
    // Re=1000 where speed jumped 0.77 -> 1790 m/s in one iteration).
    // Reduction rate is unlimited.
    // maxUnlimitedSpeed is retained for API compatibility but no longer gates
    // the growth-rate limiter.
    double velocityCorrectionMaxUnlimitedSpeed = 20.0;
    double velocityCorrectionMaxGrowthRate = 0.15;

    // Number of OpenMP threads used by the solver (matrix assembly, AMG,
    // residuals). 0 = auto (OpenMP runtime default / OMP_NUM_THREADS).
    int numThreads = 1;
    // Pressure-correction linear solver (see fvm::PressureCorrection::Config)
    // AMG is always used; the AMG hierarchy tolerance gates convergence.
    int pressureAmgPreconditionerCycles = 1;
    AmgSettings pressureAmg = AmgSettings::pressureDefaults();

    // Momentum linear solver (see fvm::CFDSolver::Config)
    // AMG is always used; momentumAmgCycles controls cycles per application.
    bool momentumAmgAsSolver = false;
    int momentumAmgCycles = 1;
        double momentumAmgMaxResidual = 0.1;
    bool pressureSolverStats = false;
    bool momentumSolverStats = false;
    AmgSettings momentumAmg = AmgSettings::momentumDefaults();

    // Turbulence (k/omega) linear solver (see fvm::SSTModel::Config)
    int turbulenceAmgCycles = 1;
    double turbulenceAmgMaxResidual = 0.1;
    // Flexible also default for turbulence (per request); pressure stays V.
    AmgSettings turbulenceAmg = AmgSettings::momentumDefaults();

    // Steady vs transient. See TransientSettings above.
    TransientSettings transient;

    // Result Update Policy: how often the live visualization refreshes mid-run.
    // EveryNSteps : publish a frame every N solver steps (transient time steps
    //               or steady iterations). EveryNTime: publish every N seconds
    //               of solution time (transient only; steady has no advancing
    //               sim time). Both: publish when either trigger fires.
    // 1 = every step/iteration. The final state is always visualized when the
    // run ends or pauses at a settled boundary, so this only controls mid-run
    // refresh frequency, not whether the end is shown.
    enum class ResultUpdateMode { EveryNSteps, EveryNTime };
    ResultUpdateMode resultUpdateMode = ResultUpdateMode::EveryNSteps;
    int visualizationInterval = 1;           // steps (EveryNSteps / Both)
    double resultUpdateTimeInterval = 0.0;   // seconds (EveryNTime / Both)
};

} // namespace cfd

