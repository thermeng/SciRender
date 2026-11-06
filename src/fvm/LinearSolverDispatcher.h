#pragma once

#include "fvm/AMG.h"
#include "fvm/Types.h"
#include "mesh/Mesh.h"

#include <Eigen/Sparse>
#include <Eigen/IterativeLinearSolvers>

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace fvm {

// LinearSolverDispatcher<Dim> owns all momentum linear-solver dispatch:
// AMG hierarchies (per component), BiCGSTAB+ILU fallback, warm-start handling,
// and the AMG-vs-ILU decision logic.
//
// The SIMPLE orchestrator no longer needs to know about AMG rebuild intervals,
// sparsity hashes, or preconditioner cycles — it simply asks the dispatcher to
// solve A x = b for a given momentum component.
//
// This is a deep module: its interface (solve() + stats) is much smaller than
// its implementation (~500 lines of AMG hierarchy bookkeeping, BiCGSTAB loops,
// GMRES acceleration, ILU fallback, and warm-start logic).
template<std::size_t Dim>
class LinearSolverDispatcher {
public:
    struct Config {
        bool useAMG = true;
        std::size_t amgThreshold = 0;
        bool amgAsSolver = false;
        std::size_t amgRebuildInterval = 5;
        int amgCycles = 2;
        double amgMaxResidual = 1e-8;
        bool solverStats = false;
        AMGHierarchy::Config amgConfig;
    };

    struct Result {
        Eigen::VectorXd x;
        bool failed = false;
        std::size_t krylovIters = 0;
    };

    LinearSolverDispatcher(const Mesh<Dim>& mesh, const Config& config);

    void setAmgConfig(const AMGHierarchy::Config& cfg) { config_.amgConfig = cfg; }

    // Solve A x = b for momentum component d. The solve uses amgMaxResidual
    // from the config as the |r|/|b| convergence tolerance. The solution is
    // warm-started from `warmStart`.
    Result solve(std::size_t component, const SparseMatrix& A, const Eigen::VectorXd& b,
                 const Eigen::VectorXd& warmStart,
                 const std::atomic<bool>* cancelRequested = nullptr);

    // --- Statistics (delegated by the SIMPLE orchestrator) ---
    std::size_t solves() const { return stats_.solves; }
    std::size_t amgSolves() const { return stats_.amgSolves; }
    std::size_t amgStandaloneSolves() const { return stats_.amgStandaloneSolves; }
    std::size_t iluSolves() const { return stats_.iluSolves; }
    std::size_t amgBuilds() const { return stats_.amgBuilds; }
    std::size_t amgRejected() const { return stats_.amgRejected; }
    std::size_t krylovIters() const { return stats_.krylovIters; }
    std::size_t amgKrylovIters() const { return stats_.amgKrylovIters; }
    double iluSeconds() const { return stats_.iluSeconds; }
    double amgSeconds() const { return stats_.amgSeconds; }
    double buildSeconds() const { return stats_.buildSeconds; }

    void printStats() const;

    // AMG level info per component (for printing in final stats)
    bool amgBuilt(std::size_t d) const;
    int amgLevels(std::size_t d) const;
    std::vector<int> amgLevelSizes(std::size_t d) const;

private:
    const Mesh<Dim>& mesh_;
    Config config_;

    // Per-component AMG hierarchies and preconditioners
    mutable std::array<AMGHierarchy, Dim> amg_;
    mutable std::array<AMGPreconditioner, Dim> amgPrec_;
    mutable std::array<Eigen::BiCGSTAB<SparseMatrix, AMGPreconditioner>, Dim> amgSolver_;
    mutable std::array<bool, Dim> amgBuilt_{};
    mutable std::array<std::size_t, Dim> amgPatternHash_{};
    mutable std::array<std::size_t, Dim> amgValueUpdates_{};
    mutable std::array<bool, Dim> amgPatternAnalyzed_{};
    mutable std::array<bool, Dim> amgDisabledReason_{};
    mutable std::array<bool, Dim> amgPathLogged_{};

    // BiCGSTAB+ILU solver (shared across components)
    mutable Eigen::BiCGSTAB<SparseMatrix, Eigen::IncompleteLUT<double>> iluSolver_;
    mutable std::array<bool, Dim> iluPatternAnalyzed_{};

    // Stats
    struct Stats {
        std::size_t solves = 0;
        std::size_t amgSolves = 0;
        std::size_t amgStandaloneSolves = 0;
        std::size_t iluSolves = 0;
        std::size_t amgBuilds = 0;
        std::size_t amgRejected = 0;
        std::size_t krylovIters = 0;
        std::size_t amgKrylovIters = 0;
        double iluSeconds = 0.0;
        double amgSeconds = 0.0;
        double buildSeconds = 0.0;
    };
    mutable Stats stats_{};

    static bool hasPositiveOffDiagonals_(const SparseMatrix& A);
    Eigen::VectorXd solveILU_(std::size_t d, const SparseMatrix& A, const Eigen::VectorXd& b,
                              std::size_t& itersOut);
};

extern template class LinearSolverDispatcher<2>;
extern template class LinearSolverDispatcher<3>;

} // namespace fvm
