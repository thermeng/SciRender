#include "fvm/LinearSolverDispatcher.h"

#include "fvm/Gmres.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <iomanip>

namespace fvm {

template<std::size_t Dim>
LinearSolverDispatcher<Dim>::LinearSolverDispatcher(const Mesh<Dim>& mesh, const Config& config)
    : mesh_(mesh), config_(config) {
    for (std::size_t d = 0; d < Dim; ++d) {
        amg_[d].setConfig(config_.amgConfig);
        amgPrec_[d].setAMG(nullptr);
        amgSolver_[d].setMaxIterations(5000);
        amgSolver_[d].setTolerance(config_.amgMaxResidual);
    }
    iluSolver_.setMaxIterations(50000);
    iluSolver_.setTolerance(1e-8);
}

template<std::size_t Dim>
typename LinearSolverDispatcher<Dim>::Result
LinearSolverDispatcher<Dim>::solve(std::size_t component, const SparseMatrix& A,
                                    const Eigen::VectorXd& b,
                                    const Eigen::VectorXd& warmStart,
                                    const std::atomic<bool>* cancelRequested) {
    ++stats_.solves;

    const std::size_t n = b.size();

    // Threshold 0 means "always use AMG".
    const std::size_t momThreshold = config_.amgThreshold;
    const bool amgEligible =
        config_.useAMG &&
        !amgDisabledReason_[component] &&
        (momThreshold == 0 || n >= momThreshold);

    if (!amgEligible) {
        if (config_.useAMG && !amgDisabledReason_[component] &&
            momThreshold != 0 && n < momThreshold) {
            amgDisabledReason_[component] = true;
        }
        if (!amgPathLogged_[component]) {
            if (!config_.useAMG) {
                std::cerr << "  momentum[" << (component == 0 ? "U" : component == 1 ? "V" : "W")
                          << "]: AMG disabled by config; using BiCGSTAB+ILU.\n";
            } else if (momThreshold != 0 && n < momThreshold) {
                std::cerr << "  momentum[" << (component == 0 ? "U" : component == 1 ? "V" : "W")
                          << "]: AMG skipped (n=" << n << " < threshold " << momThreshold
                          << "); using BiCGSTAB+ILU.\n";
            }
            amgPathLogged_[component] = true;
        }
        std::size_t iters = 0;
        const auto t0 = std::chrono::steady_clock::now();
        Eigen::VectorXd x = solveILU_(component, A, b, iters);
        stats_.iluSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        ++stats_.iluSolves;
        stats_.krylovIters += iters;
        return {std::move(x), false, iters};
    }

    const bool standaloneAmg = config_.amgAsSolver;
    if (!amgPathLogged_[component]) {
        const auto acc = config_.amgConfig.acceleration;
        std::cerr << "  momentum[" << (component == 0 ? "U" : component == 1 ? "V" : "W")
                  << "]: using ";
        if (standaloneAmg) {
            std::cerr << "AMG cycles.";
        } else if (acc == AMGHierarchy::Config::Acceleration::GMRES) {
            std::cerr << "AMG-preconditioned GMRES.";
        } else {
            std::cerr << "AMG-preconditioned BiCGSTAB.";
        }
        std::cerr << "\n";
        amgPathLogged_[component] = true;
    }

    const std::size_t hash = sparsityHash_(A);
    const bool needBuild = !amgBuilt_[component] || hash != amgPatternHash_[component];

    if (needBuild) {
        amgPatternAnalyzed_[component] = false;
        const auto t0 = std::chrono::steady_clock::now();
        amg_[component].setConfig(config_.amgConfig);
        amg_[component].build(A);
        amgPatternHash_[component] = hash;
        amgBuilt_[component] = amg_[component].isBuilt();
        amgValueUpdates_[component] = 0;
        ++stats_.amgBuilds;
        stats_.buildSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (amg_[component].isBuilt() && hasPositiveOffDiagonals_(A)) {
            std::cerr << "  momentum[" << (component == 0 ? "U" : component == 1 ? "V" : "W")
                      << "]: WARNING: matrix has positive off-diagonals; AMG robustness may suffer\n";
        }
    } else if (++amgValueUpdates_[component] >= std::max<std::size_t>(1, config_.amgRebuildInterval)) {
        amgValueUpdates_[component] = 0;
        amg_[component].updateFineValues(A);
    }

    if (!amgBuilt_[component]) {
        std::size_t iters = 0;
        Eigen::VectorXd x = solveILU_(component, A, b, iters);
        ++stats_.iluSolves;
        stats_.krylovIters += iters;
        return {std::move(x), false, iters};
    }

    const auto momAccel = config_.amgConfig.acceleration;

    if (standaloneAmg) {
        const auto t0 = std::chrono::steady_clock::now();
        Eigen::VectorXd x = warmStart;
        const bool ok = amg_[component].solve(b, x, cancelRequested);
        stats_.amgSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        stats_.krylovIters += static_cast<std::size_t>(amg_[component].lastIterations());

        if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
            return {Eigen::VectorXd::Zero(b.size()), true, 0};
        }

        if (!ok && (!x.allFinite() || !std::isfinite(amg_[component].lastResidual()))) {
            std::size_t iters = 0;
            x = solveILU_(component, A, b, iters);
            stats_.iluSolves++;
            stats_.krylovIters += iters;
            ++stats_.amgRejected;
            if (!x.allFinite()) {
                return {Eigen::VectorXd::Zero(b.size()), true, 0};
            }
            return {std::move(x), false, iters};
        }

        if (!x.allFinite()) {
            return {Eigen::VectorXd::Zero(b.size()), true, 0};
        }
        ++stats_.amgSolves;
        ++stats_.amgStandaloneSolves;
        return {std::move(x), false, static_cast<std::size_t>(amg_[component].lastIterations())};
    }

    amgPrec_[component].setAMG(&amg_[component]);
    amgPrec_[component].setCycles(config_.amgCycles);
    if (!amgPatternAnalyzed_[component]) {
        amgSolver_[component].setMaxIterations(5000);
        amgSolver_[component].setTolerance(config_.amgMaxResidual);
        amgSolver_[component].analyzePattern(A);
        amgPatternAnalyzed_[component] = true;
    }

    const auto t0 = std::chrono::steady_clock::now();
    Eigen::VectorXd x = warmStart;
    if (momAccel == AMGHierarchy::Config::Acceleration::GMRES) {
        const GmresResult gres = gmresRestarted(
            b, x,
            [this, &A](const Eigen::VectorXd& v) { return A * v; },
            [this, component](const Eigen::VectorXd& v) { return amgPrec_[component].solve(v); },
            config_.amgMaxResidual, 5000, 30,
            cancelRequested ? cancelRequested : nullptr);
        stats_.krylovIters += static_cast<std::size_t>(gres.iterations);
        if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
            return {Eigen::VectorXd::Zero(b.size()), true, 0};
        }
    } else {
        const int maxIt = 5000;
        const double bnorm = b.norm();
        const double tol = (bnorm > 0.0 ? bnorm : 1.0) * config_.amgMaxResidual;
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

    const double bnorm = b.norm();
    double rnorm = (A * x - b).norm();
    double rel = bnorm > 0.0 ? rnorm / bnorm : rnorm;

    bool bad =
        !std::isfinite(rel) ||
        !x.allFinite() ||
        !std::isfinite(rnorm);

    if (bad && !(cancelRequested && cancelRequested->load(std::memory_order_acquire))) {
        std::size_t iters = 0;
        x = solveILU_(component, A, b, iters);
        stats_.iluSolves++;
        stats_.krylovIters += iters;
        if (x.allFinite()) {
            rnorm = (A * x - b).norm();
            rel = bnorm > 0.0 ? rnorm / bnorm : rnorm;
            bad = !std::isfinite(rel) || !x.allFinite() || !std::isfinite(rnorm);
        }
    }

    if (bad) {
        if (!x.allFinite() || !std::isfinite(rel) || !std::isfinite(rnorm)) {
            return {Eigen::VectorXd::Zero(b.size()), true, 0};
        }
        if (!x.allFinite()) {
            x = Eigen::VectorXd::Zero(b.size());
        }
        return {std::move(x), false, 0};
    }

    ++stats_.amgSolves;
    return {std::move(x), false, 0};
}

template<std::size_t Dim>
Eigen::VectorXd LinearSolverDispatcher<Dim>::solveILU_(std::size_t d, const SparseMatrix& A,
                                                        const Eigen::VectorXd& b,
                                                        std::size_t& itersOut) {
    if (!iluPatternAnalyzed_[d]) {
        iluSolver_.setMaxIterations(50000);
        iluSolver_.setTolerance(1e-8);
        iluSolver_.analyzePattern(A);
        iluPatternAnalyzed_[d] = true;
    }
    iluSolver_.factorize(A);
    Eigen::VectorXd x = iluSolver_.solve(b);
    itersOut = static_cast<std::size_t>(iluSolver_.iterations());
    if (iluSolver_.info() != Eigen::Success) {
        std::cerr << "  WARNING: momentum-" << (d == 0 ? "U" : d == 1 ? "V" : "W")
                  << " BiCGSTAB failed (iters=" << iluSolver_.iterations()
                  << ", resid=" << iluSolver_.error() << ")\n";
        if (!std::isfinite(iluSolver_.error()) || !x.allFinite() ||
            iluSolver_.info() == Eigen::NumericalIssue ||
            iluSolver_.info() == Eigen::InvalidInput) {
            // Solver breakdown - caller checks x.allFinite()
        }
    }
    return x;
}

template<std::size_t Dim>
std::size_t LinearSolverDispatcher<Dim>::sparsityHash_(const SparseMatrix& A) {
    std::size_t h = 1469598103934665603ull;
    auto mix = [&h](std::size_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    for (Eigen::Index j = 0; j < A.outerSize(); ++j) {
        for (SparseMatrix::InnerIterator it(A, j); it; ++it) {
            mix(static_cast<std::size_t>(it.row()));
            mix(static_cast<std::size_t>(it.col()));
        }
    }
    return h;
}

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
    std::cerr << "\n  Momentum linear solver statistics\n";
    std::cerr << "    solves:            " << stats_.solves << "\n";
    std::cerr << "    AMG solves:        " << stats_.amgSolves << "\n";
    std::cerr << "    ILU solves:        " << stats_.iluSolves << "\n";
    std::cerr << "    AMG builds:        " << stats_.amgBuilds << "\n";
    std::cerr << "    AMG rejected:      " << stats_.amgRejected
              << "  (fell back to ILU)\n";
    std::cerr << "    Krylov iters:      " << stats_.krylovIters << "\n";
    if (stats_.solves > 0) {
        std::cerr << "    Krylov iters/solve:"
                  << "  " << (static_cast<double>(stats_.krylovIters) /
                              static_cast<double>(stats_.solves)) << "\n";
    }
    std::cerr << "    AMG build time:    " << stats_.buildSeconds << " s\n";
    std::cerr << "    AMG solve time:    " << stats_.amgSeconds << " s\n";
    std::cerr << "    ILU solve time:    " << stats_.iluSeconds << " s\n";
    for (std::size_t d = 0; d < Dim; ++d) {
        if (!amg_[d].isBuilt()) continue;
        std::cerr << "    momentum[" << (d == 0 ? "U" : d == 1 ? "V" : "W")
                  << "] AMG levels: " << amg_[d].numLevels() << " (";
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

template class LinearSolverDispatcher<2>;
template class LinearSolverDispatcher<3>;

} // namespace fvm
