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
#include <string>
#include <vector>

namespace fvm {

// LinearSolverDispatcher<Dim> owns all momentum linear-solver dispatch:
// AMG hierarchies (per component), warm-start handling.
//
// The SIMPLE orchestrator no longer needs to know about AMG rebuild intervals,
// sparsity hashes, or preconditioner cycles — it simply asks the dispatcher to
// solve A x = b for a given momentum component.
//
// This is a deep module: its interface (solve() + stats) is much smaller than
// its implementation (~500 lines of AMG hierarchy bookkeeping, BiCGSTAB loops,
// GMRES acceleration, and warm-start logic).
template<std::size_t Dim>
class LinearSolverDispatcher {
public:
    struct Config {
        bool useAMG = true;
        bool amgAsSolver = false;
        std::size_t amgRebuildInterval = 5;
        int amgCycles = 2;
        double amgMaxResidual = 1e-8;
        bool solverStats = false;
        // When true, the Krylov step uses CG instead of BiCGSTAB. Only valid
        // for SPD operators (the compact Rhie-Chow pressure matrix).
        bool preferCG = false;
        cfd::AmgSettings amgConfig;
        // Diagnostic name per component, used by every solver log line. The
        // defaults name the momentum components because that is the only
        // multi-component equation; pressure and turbulence override their
        // single entry.
        std::array<std::string, Dim> labels = defaultLabels();

        static std::array<std::string, Dim> defaultLabels() {
            constexpr const char* kNames[] = {"momentum[U]", "momentum[V]", "momentum[W]"};
            std::array<std::string, Dim> l;
            for (std::size_t d = 0; d < Dim; ++d)
                l[d] = (d < 3) ? kNames[d] : "momentum";
            return l;
        }
    };

    struct Result {
        Eigen::VectorXd x;
        bool failed = false;
        std::size_t krylovIters = 0;
    };

    LinearSolverDispatcher(const Mesh<Dim>& mesh, const Config& config);

    // Replace the whole solver configuration. Any hierarchy built under the
    // previous configuration is discarded and rebuilt on the next solve, so a
    // changed backend/coarsening/tolerance actually takes effect instead of
    // being masked by a warm hierarchy.
    void setConfig(const Config& config);

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
    std::size_t amgBuilds() const { return stats_.amgBuilds; }
    std::size_t krylovIters() const { return stats_.krylovIters; }
    std::size_t amgCycleIters() const { return stats_.amgCycleIters; }
    double amgSeconds() const { return stats_.amgSeconds; }
    double buildSeconds() const { return stats_.buildSeconds; }
    double dispatchSeconds() const { return stats_.dispatchSeconds; }

    void printStats() const;

    // AMG level info per component (for printing in final stats)
    bool amgBuilt(std::size_t d) const;
    int amgLevels(std::size_t d) const;
    std::vector<int> amgLevelSizes(std::size_t d) const;
    // Diagnostics of the last AMG solve for component d (cycles performed and
    // final relative residual |b-Ax|/|b|). Valid after the most recent solve().
    int amgLastIterations(std::size_t d) const;
    double amgLastResidual(std::size_t d) const;
    // Per-cycle relative residuals from the last AMG solve for component d.
    const std::vector<double>& amgLastResidualHistory(std::size_t d) const;

private:
    const Mesh<Dim>& mesh_;
    Config config_;

    // Per-component AMG hierarchies and preconditioners
    mutable std::array<AMGHierarchy, Dim> amg_;
    mutable std::array<AMGPreconditioner, Dim> amgPrec_;
    mutable std::array<Eigen::BiCGSTAB<SparseMatrix, AMGPreconditioner>, Dim> amgSolver_;
    mutable std::array<Eigen::ConjugateGradient<SparseMatrix, Eigen::Lower|Eigen::Upper,
                                                 AMGPreconditioner>, Dim> cgAmgSolver_;
    mutable std::array<bool, Dim> amgBuilt_{};
    mutable std::array<std::size_t, Dim> amgPatternHash_{};
    mutable std::array<std::size_t, Dim> amgValueUpdates_{};
    mutable std::array<bool, Dim> amgPatternAnalyzed_{};
    mutable std::array<bool, Dim> amgPathLogged_{};

    // Stats
    struct Stats {
        std::size_t solves = 0;
        std::size_t amgSolves = 0;
        std::size_t amgStandaloneSolves = 0;
        std::size_t amgBuilds = 0;
        std::size_t krylovIters = 0;
        std::size_t amgCycleIters = 0;
        double amgSeconds = 0.0;
        double buildSeconds = 0.0;
        double dispatchSeconds = 0.0;
    };
    mutable Stats stats_{};

    static bool hasPositiveOffDiagonals_(const SparseMatrix& A);

    // Diagnostic name used in log lines for this dispatcher's equation.
    const std::string& label_(std::size_t component) const {
        return config_.labels[component];
    }
};

extern template class LinearSolverDispatcher<2>;
extern template class LinearSolverDispatcher<3>;

} // namespace fvm
