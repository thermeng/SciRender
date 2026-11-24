#include "fvm/LinearSolverDispatcher.h"

#include "fvm/Gmres.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <iomanip>

namespace fvm {

template<std::size_t Dim>
LinearSolverDispatcher<Dim>::LinearSolverDispatcher(const Mesh<Dim>& mesh, const Config& config)
    : mesh_(mesh) {
    setConfig(config);
}

template<std::size_t Dim>
void LinearSolverDispatcher<Dim>::setConfig(const Config& config) {
    config_ = config;
    for (std::size_t d = 0; d < Dim; ++d) {
        amg_[d] = AMGHierarchy(config_.amgConfig, static_cast<int>(Dim));
        amgPrec_[d].setAMG(nullptr);
        amgSolver_[d].setMaxIterations(5000);
        amgSolver_[d].setTolerance(config_.amgMaxResidual);
        cgAmgSolver_[d].setMaxIterations(5000);
        cgAmgSolver_[d].setTolerance(config_.amgMaxResidual);
        amgBuilt_[d] = false;
        amgPatternAnalyzed_[d] = false;
        amgPathLogged_[d] = false;
    }
}

template<std::size_t Dim>
typename LinearSolverDispatcher<Dim>::Result
LinearSolverDispatcher<Dim>::solve(std::size_t component, const SparseMatrix& A,
                                    const Eigen::VectorXd& b,
                                    const Eigen::VectorXd& warmStart,
                                    const std::atomic<bool>* cancelRequested) {
    const auto tDispatch0 = std::chrono::steady_clock::now();
    ++stats_.solves;

    const std::size_t n = b.size();

    // A non-finite right-hand side is already a broken system: every path
    // below would either repair it to a zero solution (AMG) or return NaN.
    // Report the failure so SIMPLE aborts with a reason instead of continuing
    // on a silently repaired field.
    if (!b.allFinite()) return {Eigen::VectorXd::Zero(n), true, 0};

    // STAR parity: Acceleration Method = None means the AMG hierarchy *is*
    // the solver (its own Max Cycle / Convergence Tolerance drive it), not a
    // preconditioner. The explicit amgAsSolver switch forces the same path.
    const bool standaloneAmg =
        config_.amgAsSolver ||
        config_.amgConfig.acceleration == cfd::AmgSettings::Acceleration::None;
    if (!amgPathLogged_[component]) {
        const auto acc = config_.amgConfig.acceleration;
        std::cerr << "  " << label_(component) << ": using ";
        if (standaloneAmg) {
            std::cerr << "AMG cycles.";
        } else if (acc == cfd::AmgSettings::Acceleration::GMRES) {
            std::cerr << "AMG-preconditioned GMRES.";
        } else if (config_.preferCG) {
            std::cerr << "AMG-preconditioned CG.";
        } else {
            std::cerr << "AMG-preconditioned BiCGSTAB.";
        }
        std::cerr << "\n";
        amgPathLogged_[component] = true;
    }

    const std::size_t hash = detail::sparsityHash(A);
    const bool needBuild = !amgBuilt_[component] || hash != amgPatternHash_[component];

    if (needBuild) {
        amgPatternAnalyzed_[component] = false;
        const auto t0 = std::chrono::steady_clock::now();
        amg_[component] = AMGHierarchy(config_.amgConfig, static_cast<int>(Dim));
        amg_[component].build(A, hash);
        amgPatternHash_[component] = hash;
        amgBuilt_[component] = amg_[component].isBuilt();
        amgValueUpdates_[component] = 0;
        ++stats_.amgBuilds;
        stats_.buildSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (amg_[component].isBuilt() && hasPositiveOffDiagonals_(A)) {
            std::cerr << "  " << label_(component)
                      << ": WARNING: matrix has positive off-diagonals; AMG robustness may suffer\n";
        }
    } else {
        // Cached aggregation: the maps depend only on the sparsity pattern,
        // so the level operators are refreshed from the current values by an
        // O(nnz) restriction - no re-coarsening, no strength matrix, no
        // interpolation weights. That keeps the standalone residual test valid
        // without paying for a full AMG setup every iteration.
        const auto tRefresh = std::chrono::steady_clock::now();
        amg_[component].build(A, hash);
        stats_.buildSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - tRefresh).count();
    }

    // A hierarchy may have been built before the requested verbosity was
    // applied (e.g. during initialization), so print on the first solve that
    // actually asks for it, not only on a rebuild.
    if (!amgHierarchyLogged_[component] && amg_[component].isBuilt() &&
        config_.amgConfig.verbosity >= cfd::AmgSettings::Verbosity::High) {
        amg_[component].printHierarchy(label_(component).c_str(), std::cerr);
        amgHierarchyLogged_[component] = true;
    }

    if (!amgBuilt_[component]) {
        std::cerr << "  " << label_(component) << ": AMG hierarchy failed to build\n";
        return {Eigen::VectorXd::Zero(b.size()), true, 0};
    }

    const auto momAccel = config_.amgConfig.acceleration;
    const double effTol = config_.amgMaxResidual;

    if (standaloneAmg) {
        const auto t0 = std::chrono::steady_clock::now();
        Eigen::VectorXd x = warmStart;
        const bool ok = amg_[component].solve(b, x, cancelRequested);
        stats_.amgSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        stats_.amgCycleIters += static_cast<std::size_t>(amg_[component].lastIterations());

        // Verbose per-component diagnostic (STAR-CCM+ style: residual history,
        // level table, memory) for the standalone AMG path.
        if (config_.solverStats && amg_[component].isBuilt()) {
            amg_[component].printVerbose(label_(component).c_str(), std::cerr);
        }

        if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
            return {Eigen::VectorXd::Zero(b.size()), true, 0};
        }

        if (!ok || !x.allFinite() || !std::isfinite(amg_[component].lastResidual())) {
            std::cerr << "  " << label_(component)
                      << ": AMG solve failed (ok=" << ok << " xFinite=" << x.allFinite()
                      << " resid=" << amg_[component].lastResidual()
                      << " xMax=" << (x.size() ? x.cwiseAbs().maxCoeff() : 0.0)
                      << " bMax=" << (b.size() ? b.cwiseAbs().maxCoeff() : 0.0) << ")\n";
            return {Eigen::VectorXd::Zero(b.size()), true, 0};
        }
        ++stats_.amgSolves;
        ++stats_.amgStandaloneSolves;
        stats_.dispatchSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - tDispatch0).count();
        return {std::move(x), false, 0};
    }

    amgPrec_[component].setAMG(&amg_[component]);
    amgPrec_[component].setCycles(config_.amgCycles);
    if (!amgPatternAnalyzed_[component]) {
        amgSolver_[component].setMaxIterations(5000);
        amgSolver_[component].setTolerance(effTol);
        amgSolver_[component].analyzePattern(A);
        cgAmgSolver_[component].setMaxIterations(5000);
        cgAmgSolver_[component].setTolerance(effTol);
        cgAmgSolver_[component].analyzePattern(A);
        amgPatternAnalyzed_[component] = true;
    }

    const auto t0 = std::chrono::steady_clock::now();
    Eigen::VectorXd x = warmStart;
    if (momAccel == cfd::AmgSettings::Acceleration::GMRES) {
        const GmresResult gres = gmresRestarted(
            b, x,
            [this, &A](const Eigen::VectorXd& v) { return A * v; },
            [this, component](const Eigen::VectorXd& v) { return amgPrec_[component].solve(v); },
            effTol, 5000, 30,
            cancelRequested ? cancelRequested : nullptr);
        stats_.krylovIters += static_cast<std::size_t>(gres.iterations);
        if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
            return {Eigen::VectorXd::Zero(b.size()), true, 0};
        }
    } else if (config_.preferCG) {
        cgAmgSolver_[component].preconditioner().setAMG(&amg_[component]);
        cgAmgSolver_[component].preconditioner().setCycles(config_.amgCycles);
        x = cgAmgSolver_[component].solve(b);
        stats_.krylovIters += static_cast<std::size_t>(cgAmgSolver_[component].iterations());
        if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
            return {Eigen::VectorXd::Zero(b.size()), true, 0};
        }
    } else {
        const int maxIt = 5000;
        const double bnorm = b.norm();
        const double tol = (bnorm > 0.0 ? bnorm : 1.0) * effTol;
        Eigen::VectorXd r = b - A * x;
        Eigen::VectorXd r0 = r;
        Eigen::VectorXd v = Eigen::VectorXd::Zero(b.size());
        Eigen::VectorXd p = Eigen::VectorXd::Zero(b.size());
        double rho = 1.0, alpha = 1.0, omega = 1.0;
        int iters = 0;
        for (iters = 0; iters < maxIt; ++iters) {
            if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
                return {Eigen::VectorXd::Zero(b.size()), true, 0};
            }
            const double rhoNew = r0.dot(r);
            if (rhoNew == 0.0 || !std::isfinite(rhoNew)) break;
            const double beta = (rhoNew / rho) * (alpha / omega);
            p = r + beta * (p - omega * v);
            Eigen::VectorXd phat = amgPrec_[component].solve(p);
            if (!phat.allFinite()) break;
            v = A * phat;
            const double denom = r0.dot(v);
            if (denom == 0.0 || !std::isfinite(denom)) break;
            const double alphaNew = rhoNew / denom;
            if (!std::isfinite(alphaNew)) break;
            Eigen::VectorXd s = r - alphaNew * v;
            if (s.norm() < tol) {
                x += alphaNew * phat;
                r = s;
                rho = rhoNew; alpha = alphaNew; omega = 1.0;
                break;
            }
            Eigen::VectorXd shat = amgPrec_[component].solve(s);
            if (!shat.allFinite()) break;
            Eigen::VectorXd t = A * shat;
            const double tt = t.dot(t);
            if (tt == 0.0 || !std::isfinite(tt)) break;
            const double omegaNew = t.dot(s) / tt;
            if (omegaNew == 0.0 || !std::isfinite(omegaNew)) break;
            x += alphaNew * phat + omegaNew * shat;
            r = s - omegaNew * t;
            rho = rhoNew; alpha = alphaNew; omega = omegaNew;
            if (r.norm() < tol) break;
        }
        stats_.krylovIters += static_cast<std::size_t>(iters + 1);
        if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
            return {Eigen::VectorXd::Zero(b.size()), true, 0};
        }
    }
    stats_.amgSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // Verbose per-component diagnostic (residual history + level table + memory),
    // STAR-CCM+ style. Most meaningful for the standalone AMG path; the CG path
    // prints the last preconditioner cycle's history.
    if (config_.solverStats && amg_[component].isBuilt()) {
        amg_[component].printVerbose(label_(component).c_str(), std::cerr);
    }

    const double bnorm = b.norm();
    double rnorm = (A * x - b).norm();
    double rel = bnorm > 0.0 ? rnorm / bnorm : rnorm;

    const bool bad =
        !std::isfinite(rel) ||
        !x.allFinite() ||
        !std::isfinite(rnorm);

    if (bad) {
        std::cerr << "  " << label_(component)
                  << ": AMG-preconditioned Krylov solve failed (resid=" << rel << ")\n";
        return {Eigen::VectorXd::Zero(b.size()), true, 0};
    }

    ++stats_.amgSolves;
    return {std::move(x), false, 0};
}

// solveILU_ removed: AMG is the only solver. If AMG fails, the caller aborts.

template<std::size_t Dim>
bool LinearSolverDispatcher<Dim>::hasPositiveOffDiagonals_(const SparseMatrix& A) {
    for (Eigen::Index k = 0; k < A.outerSize(); ++k) {
        for (SparseMatrix::InnerIterator it(A, k); it; ++it) {
            if (it.row() != it.col() && it.value() > 0.0) return true;
        }
    }
    return false;
}

template<std::size_t Dim>
void LinearSolverDispatcher<Dim>::printStats() const {
    if (!config_.solverStats) return;
    std::cerr << "\n  Linear solver statistics\n";
    std::cerr << "    AMG solves:        " << stats_.amgSolves << "\n";
    std::cerr << "    AMG builds:        " << stats_.amgBuilds << "\n";
    std::cerr << "    Krylov iters:      " << stats_.krylovIters << "\n";
    if (stats_.solves > 0) {
        std::cerr << "    Krylov iters/solve:"
                  << "  " << (static_cast<double>(stats_.krylovIters) /
                              static_cast<double>(stats_.solves)) << "\n";
    }
    if (stats_.amgCycleIters > 0) {
        std::cerr << "    AMG cycle iters:  " << stats_.amgCycleIters << "\n";
        if (stats_.amgStandaloneSolves > 0) {
            std::cerr << "    AMG cycle/solve:  "
                      << (static_cast<double>(stats_.amgCycleIters) /
                          static_cast<double>(stats_.amgStandaloneSolves))
                      << "\n";
        }
    }
    std::cerr << "    AMG build time:    " << stats_.buildSeconds << " s\n";
    std::cerr << "    AMG solve time:    " << stats_.amgSeconds << " s\n";
    for (std::size_t d = 0; d < Dim; ++d) {
        if (!amg_[d].isBuilt()) continue;
        std::cerr << "    " << label_(d) << " AMG levels: " << amg_[d].numLevels() << " (";
        for (int size : amg_[d].levelSizes()) std::cerr << size << " ";
        std::cerr << ")\n";
    }
}

template<std::size_t Dim>
bool LinearSolverDispatcher<Dim>::amgBuilt(std::size_t d) const {
    return amg_[d].isBuilt();
}

template<std::size_t Dim>
int LinearSolverDispatcher<Dim>::amgLevels(std::size_t d) const {
    return amg_[d].numLevels();
}

template<std::size_t Dim>
std::vector<int> LinearSolverDispatcher<Dim>::amgLevelSizes(std::size_t d) const {
    return amg_[d].levelSizes();
}

template<std::size_t Dim>
int LinearSolverDispatcher<Dim>::amgLastIterations(std::size_t d) const {
    return amg_[d].lastIterations();
}

template<std::size_t Dim>
double LinearSolverDispatcher<Dim>::amgLastResidual(std::size_t d) const {
    return amg_[d].lastResidual();
}

template<std::size_t Dim>
const std::vector<double>& LinearSolverDispatcher<Dim>::amgLastResidualHistory(std::size_t d) const {
    return amg_[d].lastResidualHistory();
}

template class LinearSolverDispatcher<2>;
template class LinearSolverDispatcher<3>;

} // namespace fvm
