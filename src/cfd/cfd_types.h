#pragma once
#include <string>
#include <vector>
#include <cstddef>

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
};

// UI-facing mirror of fvm::AMGHierarchy::Config. Kept as a plain struct with
// plain enums so the UI layer never has to include Eigen; the single
// translation to the solver type lives in cfd/amg_settings_bridge.h. Field
// names and defaults are intentionally identical to AMGHierarchy::Config so the
// two stay easy to compare.
struct AmgSettings {
    enum class CoarseningType { RS, PMIS };
    enum class InterpolationType { Direct, Extended };
    enum class CycleType { V, F };
    enum class SmootherType { SymmetricGaussSeidel, ForwardGaussSeidel, ForwardSOR };

    CoarseningType coarseningType = CoarseningType::RS;
    InterpolationType interpolationType = InterpolationType::Extended;
    CycleType cycleType = CycleType::F;
    SmootherType smootherType = SmootherType::SymmetricGaussSeidel;
    // Relaxation for ForwardSOR. 1.0 degenerates to plain Gauss-Seidel.
    double smootherOmega = 1.0;
    double strengthThreshold = 0.25;
    double truncThreshold = 0.2;
    int maxLevels = 30;
    int minCoarseSize = 10;
    int preSweeps = 2;
    int postSweeps = 2;
    // Only used by a standalone AMG solve, i.e. when AMG is configured as the
    // pressure solver rather than as a Krylov preconditioner.
    int maxIterations = 100;
    double tolerance = 1e-8;
    // Hardening options (address silent-failure observations)
    int rngSeed = 42; // 0 = nondeterministic (random_device)
    double stagnationFactor = 1.05;
    int stagnationMinIter = 10;
    double stagnationLooseFactor = 5.0;
    double galerkinDropTol = 1e-12;

    // Defaults the solver applies per hierarchy, so "Reset" lands on the
    // values the application ships with. Pressure uses a V-cycle with direct
    // interpolation; momentum additionally needs a forward-only smoother
    // because its operator is nonsymmetric.
    static AmgSettings pressureDefaults() {
        AmgSettings s;
        s.interpolationType = InterpolationType::Direct;
        s.cycleType = CycleType::V;
        return s;
    }
    static AmgSettings momentumDefaults() {
        AmgSettings s;
        s.interpolationType = InterpolationType::Direct;
        s.cycleType = CycleType::V;
        s.smootherType = SmootherType::ForwardGaussSeidel;
        s.preSweeps = 1;
        s.postSweeps = 1;
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
    // Pressure correctors per step (PISO: 1 predictor + this many correctors).
    int maxOuterCorrectors = 3;
    // Base name or full path for the .pvd/.vtu output. Empty = "transient".
    std::string pvdFile;
};

struct CaseSettings {
    double rho = 1.0, nu = 0.01;
    double tolerance = 1e-3;
    std::size_t maxIterations = 100;
    double alphaU = 0.7, alphaP = 0.3;
    bool simplec = false;
    int scheme = 0; // 0 upwind, 1 central, 2 second-order upwind
    bool rhieChow = true;
    bool turbulence = false;
    double kInf = 1e-3, omegaInf = 1.0;
    double alphaK = 0.7, alphaOmega = 0.7;

    // Advanced SIMPLE settings
    std::size_t pressureRefCell = 0; // 0 = auto (last cell)
    int pressureCorrectors = 3;
    bool limitPressureCorrection = true;
    double pressureCorrectionLimit = 5.0;
    std::size_t printFrequency = 1;
    std::size_t residualHistoryWindow = 20;

    // Pressure-correction linear solver (see fvm::PressureCorrection::Config)
    bool pressureUseAMG = true;
    bool pressureUseAMGPreconditioner = true;
    std::size_t pressureAmgThreshold = 10000;
    AmgSettings pressureAmg = AmgSettings::pressureDefaults();

    // Momentum linear solver (see fvm::SIMPLE::Config)
    bool useAMGMomentum = false;
    std::size_t momentumAmgThreshold = 20000;
    int momentumAmgCycles = 1;
    double momentumAmgMaxResidual = 1e-8;
    bool momentumSolverStats = false;
    AmgSettings momentumAmg = AmgSettings::momentumDefaults();

    // Steady vs transient. See TransientSettings above.
    TransientSettings transient;
};

} // namespace cfd