#pragma once
#include <string>
#include <vector>
#include <cstddef>
#include <optional>

namespace cfd {

enum class BCType { VelocityInlet, PressureOutlet, Outlet, WallNoSlip, SlipWall, Symmetry, MovingWall };

struct PatchBC {
    std::string patchName;
    BCType type = BCType::WallNoSlip;
    double u = 0.0, v = 0.0, w = 0.0;
    double pressure = 0.0;
    bool suppressBackflow = false;
    double backflowU = 0.0, backflowV = 0.0, backflowW = 0.0;
    double backflowK = 0.0, backflowOmega = 0.0;
    // Turbulence inlet values (used for velocity inlets).
    double kInlet = 1e-3, omegaInlet = 1.0;
};

// UI-facing mirror of fvm::AMGHierarchy::Config. Kept as a plain struct with
// plain enums so the UI layer never has to include Eigen; the single
// translation to the solver type lives in cfd/amg_settings_bridge.h. Field
// names and defaults are intentionally identical to AMGHierarchy::Config so the
// two stay easy to compare.
//
// The property names in comments are the  AMG linear solver labels
// this dialog mirrors.
struct AmgSettings {
    // --- Solver ----------------------------------------------------------
    enum class Coarsening { RugeStuben, SmoothedAggregation };
    enum class Verbosity { None, Low, High, Diagnosis };
    enum class CycleType { VCycle, WCycle, FCycle, Flexible };
    enum class Acceleration { None, ConjugateGradient, BiCGStab, GMRES };
    enum class Smoother { Jacobi, GaussSeidel, ILU };
    enum class ProportionalScaling { Automatic, Disabled };
    enum class GroupSizeControl { Automatic, Manual };

    Coarsening coarsening = Coarsening::SmoothedAggregation;
    int maxIterations = 30;     // Maximum cycle
    double tolerance = 0.1;     // Convergence tolerance
    double epsilon = 0.0;       // Epsilon
    Verbosity verbosity = Verbosity::None;
    CycleType cycleType = CycleType::VCycle;
    Acceleration acceleration = Acceleration::ConjugateGradient;
    Smoother smoother = Smoother::GaussSeidel;
    ProportionalScaling scaling = ProportionalScaling::Automatic;
    GroupSizeControl groupSizeControl = GroupSizeControl::Automatic;
    int blockSize = 1;           // Group size (Manual)

    // --- Smoothing / hierarchy -------------------------------------------
    int preSweeps = 1;           // Pre-smooths
    int postSweeps = 1;          // Post-smooths
    int maxLevels = 50;          // Maximum number of levels
    bool directCoarse = true;    // Enable the direct solver
    int coarseEnough = 2;        // Maximum direct solver equations
    int minCoarseSize = 10;

    // --- Variable / flexible cycle ---------------------------------------
    double flexCoarsenRatio = 0.5;    // Residual ratio to go coarser
    double flexLevelTolerance = 0.1;  // Extended tolerance

    // --- Expert -----------------------------------------------------------
    double strengthThreshold = 0.25;
    double truncThreshold = 0.2;
    double stagnationFactor = 1.05;
    int stagnationMinIter = 10;
    double stagnationLooseFactor = 5.0;
    // Non-symmetric operator (momentum). Forces aggregation coarsening, which
    // is the robust choice for a nonsymmetric matrix.
    bool nonsymmetric = false;

    static AmgSettings pressureDefaults() {
        AmgSettings s;
        s.maxIterations = 30;
        s.tolerance = 0.1;
        s.epsilon = 0.0;
        s.cycleType = CycleType::VCycle;
        s.preSweeps = 1;
        s.postSweeps = 1;
        s.maxLevels = 50;
        s.smoother = Smoother::GaussSeidel;
        s.acceleration = Acceleration::ConjugateGradient;
        s.scaling = ProportionalScaling::Automatic;
        s.strengthThreshold = 0.25;
        s.truncThreshold = 0.2;
        return s;
    }
    static AmgSettings momentumDefaults() {
        AmgSettings s;
        s.maxIterations = 30;
        s.tolerance = 0.1;
        s.epsilon = 0.0;
        s.preSweeps = 1;
        s.postSweeps = 1;
        s.maxLevels = 50;
        s.nonsymmetric = true;
        s.cycleType = CycleType::Flexible;
        s.smoother = Smoother::GaussSeidel;
        s.acceleration = Acceleration::None;
        s.scaling = ProportionalScaling::Automatic;
        s.strengthThreshold = 0.25;
        s.truncThreshold = 0.5;
        return s;
    }
};

// UI-facing mirror of fvm::SIMPLE::TransientConfig, plus the mode switch that
// decides whether run() calls solve() or solveTransient(). Only the transient
// path reads these; the steady path uses tolerance/maxIterations instead.
//
// Defaults are identical to TransientConfig's initialisers so the UI, the
// solver and "Restore defaults" cannot disagree.
struct TransientSettings {
    // The solver's TimeScheme::Euler is implicit, i.e. backward Euler. Named
    // for what it is so the UI does not have to explain "Euler".
    enum class TimeScheme { BackwardEuler, BDF2 };

    // false = steady: the transient controls are not shown and not consulted.
    bool enabled = false;
    // Initial step size. Always clamped into [minDt, maxDt], which applies to
    // the fixed-step case too, not just adaptive.
    double dt = 0.001;
    // Must be > 0: the solver loops "while (time < tEnd)", so 0 runs no steps.
    double tEnd = 1.0;
    TimeScheme timeScheme = TimeScheme::BackwardEuler;
    bool adaptiveCfl = false;
    double cfl = 0.5; // only consulted when adaptiveCfl is on
    double minDt = 1e-6;
    double maxDt = 0.01;
    // Output cadence in time steps; 0 disables file output entirely.
    int writeInterval = 1;
    // Non-orthogonal correctors per pressure solve (PISO).
    int nonOrthogonalCorrectors = 1;
    // Pressure correctors per step (PISO: 1 predictor + this many correctors).
    int maxOuterCorrectors = 3;
    // Base name or full path for the .pvd/.vtu output. Empty = "transient".
    std::string pvdFile;
};

struct CaseSettings {
    double rho = 1.0, nu = 0.01;
    double tolerance = 1e-3;
    std::size_t maxIterations = 500;
    // Per-equation residual tolerances (std::nullopt = inherit from `tolerance`).
    // When set, an equation is gated by its own tolerance; when nullopt it falls
    // back to the global `tolerance`. A value with its checkbox unchecked in the
    // UI means "do not gate on this equation at all" (see gate* flags below).
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
    // When set, the solver runs exactly this many iterations and the residual
    // tolerance gate is bypassed (equivalent to runFixedIterations).
    std::optional<std::size_t> fixedIterations;
    // Ignore the residual tolerance and run exactly maxIterations iterations.
    bool runFixedIterations = false;
    // Consecutive iterations the residual criterion must hold before the run
    // is declared converged.
    int convergenceHoldIterations = 3;
    // /Fluent-style residual reporting: continuity is measured on the
    // momentum predictor (before the pressure correction) and every residual is
    // normalized by its maximum over the first `residualNormalizationIterations`
    // iterations. With this on, the monitor curve starts at ~1 and is directly
    // comparable to a  residual plot.
    bool normalizeResiduals = true;
    std::size_t residualNormalizationIterations = 5;
    double alphaU = 0.7, alphaP = 0.3;
    bool simplec = false;
    int scheme = 0; // 0 upwind, 1 central, 2 second-order upwind
    // 0 least squares, 1 green-gauss cell based, 2 green-gauss node based
    int gradientScheme = 0;
    bool rhieChow = true;
    bool turbulence = false;
    bool wallFunction = false;
    double kInf = 1e-3, omegaInf = 1.0;
    double alphaK = 0.7, alphaOmega = 0.7;

    // Advanced SIMPLE settings
    std::size_t pressureRefCell = 0; // 0 = auto (last cell)
    int pressureCorrectors = 1;
    bool limitPressureCorrection = false;
    double pressureCorrectionLimit = 5.0;
    std::size_t printFrequency = 1;
    std::size_t residualHistoryWindow = 20;

    // Number of OpenMP threads used by the solver (matrix assembly, AMG,
    // residuals). 0 = auto (OpenMP runtime default / OMP_NUM_THREADS).
    int numThreads = 1;
    // Pressure-correction linear solver (see fvm::PressureCorrection::Config)
    bool pressureUseAMG = true;
    bool pressureUseAMGPreconditioner = true;
    // 0 = always use AMG ( behaviour).
    std::size_t pressureAmgThreshold = 0;
    // 0 = legacy mesh-scaled tolerance; >0 = |r|/|b| ( uses ~0.1).
    double pressureSolveTolerance = 0.0;
    int pressureAmgCycles = 1;
    AmgSettings pressureAmg = AmgSettings::pressureDefaults();

    // Momentum linear solver (see fvm::SIMPLE::Config)
    bool useAMGMomentum = true;
    // 0 = always use AMG ( behaviour).
    std::size_t momentumAmgThreshold = 0;
    // Apply the AMG hierarchy directly as the momentum solver ().
    bool momentumAmgAsSolver = false;
    int momentumAmgCycles = 2;
    double momentumAmgMaxResidual = 1e-8;
    bool momentumSolverStats = false;
    AmgSettings momentumAmg = AmgSettings::momentumDefaults();

    // Steady vs transient. See TransientSettings above.
    TransientSettings transient;
};

} // namespace cfd

