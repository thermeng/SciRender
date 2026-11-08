// Hypre-only AMGHierarchy — amgcl removed
#include "fvm/AMG.h"
#include "fvm/HypreSolver.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace fvm {

using cfd::AmgSettings;

void AMGHierarchy::build(const SparseMatrix& A) {
    levels_.clear(); cgrid_.clear(); built_ = false; amgHandle_.reset(); hypreMat_.reset(); hypreSolver_.reset();
    fineRows_.resize(0, 0); fineRowsReady_ = false; lazyBuild_ = false; hypreHierarchyReady_ = false;
    n_ = 0; nlevels_ = 0; levelSizes_.clear(); report_.clear();
    systemMatrix_ = A;
    if (A.rows() == 0 || A.cols() == 0) { buildTimeSec_ = 0.0; return; }
    n_ = static_cast<int>(A.rows());
    auto buildStart = std::chrono::steady_clock::now();
#ifdef FVM_HAVE_HYPRE
    auto mat = std::make_shared<typename HypreSolver::HypreMat>();
    auto solv = std::make_shared<typename HypreSolver::HypreSolverImpl>();
    std::vector<double> sc;
    if (HypreSolver::buildSolver(A, config_, *mat, *solv, sc)) {
        hypreMat_ = mat; hypreSolver_ = solv; hypreScale_ = sc; useHypreActive_ = true;
        // Report the real hierarchy: level count and per-level unknowns come
        // from BoomerAMG itself, not a hardcoded single level.
        std::vector<int> cgrid;
        if (!HypreSolver::hierarchyLevelSizes(*solv, n_, levelSizes_, cgrid))
            levelSizes_.assign(1, n_);
        cgrid_ = cgrid;
        nlevels_ = static_cast<int>(levelSizes_.size());
        report_ = "Hypre BoomerAMG (IJ)\n";
        for (int l = 0; l < nlevels_; ++l) {
            report_ += "  level " + std::to_string(l) + ": " +
                       std::to_string(levelSizes_[static_cast<std::size_t>(l)]) +
                       " unknowns\n";
        }
        // Build per-level prolongation/restriction/operator from cgrid_ for the
        // recursive Flexible cycle. For non-Flexible cycles these are unused.
        if (config_.cycleType == AmgSettings::CycleType::Flexible)
            buildLevelOperators_();
        if (config_.verbosity >= AmgSettings::Verbosity::High) {
            std::cerr << "  AMG hierarchy (" << n_ << " unknowns, Hypre)\n" << report_;
        }
        if (config_.verbosity == AmgSettings::Verbosity::Diagnosis) {
            std::cerr << "  AMG diagnosis: " << n_ << " unknowns, Hypre\n"
                      << "    coarsening=smoothed aggregation, smoother="
                      << ((config_.smoother == AmgSettings::Smoother::Jacobi) ? "Jacobi" : "Gauss-Seidel")
                      << ", cycle=" << (config_.cycleType == AmgSettings::CycleType::WCycle ? "W" : config_.cycleType == AmgSettings::CycleType::FCycle ? "F" : config_.cycleType == AmgSettings::CycleType::Flexible ? "Flexible" : "V")
                      << ", coarse solve=direct (<= " << config_.coarseEnough << " equations)\n";
        }
        buildTimeSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - buildStart).count();
        built_ = true;
        lazyBuild_ = false;
        hypreHierarchyReady_ = true;
        return;
    }
    std::cerr << "  AMG: Hypre build failed\n";
#endif
    buildTimeSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - buildStart).count();
    built_ = false;
}

bool AMGHierarchy::applyCycle(const Eigen::VectorXd& b, Eigen::VectorXd& x) const {
    if (!config_.flexStruct.empty() && config_.flexStruct[0] == static_cast<int>(AmgSettings::FlexMove::Terminate)) return true;
    if (!built_ || !useHypreActive_) return false;
    // Flexible cycles are residual-gated: fine sweep first, coarse correction
    // only when the restriction tolerance asks for it.
    if (config_.cycleType == AmgSettings::CycleType::Flexible) return applyFlexibleCycle_(b, x);
    if (!hypreMat_ || !hypreSolver_) return false;
    auto mat = std::static_pointer_cast<HypreSolver::HypreMat>(hypreMat_);
    auto solv = std::static_pointer_cast<HypreSolver::HypreSolverImpl>(hypreSolver_);
    if (!mat || !solv) return false;
    return HypreSolver::applyCycle(*mat, *solv, b, x, hypreScale_);
}

bool AMGHierarchy::ensureHypreBuilt_() const {
    if (hypreHierarchyReady_) return true;
#ifdef FVM_HAVE_HYPRE
    auto buildStart = std::chrono::steady_clock::now();
    auto mat = std::make_shared<typename HypreSolver::HypreMat>();
    auto solv = std::make_shared<typename HypreSolver::HypreSolverImpl>();
    std::vector<double> sc;
    if (HypreSolver::buildSolver(systemMatrix_, config_, *mat, *solv, sc)) {
        hypreMat_ = mat; hypreSolver_ = solv; hypreScale_ = sc;
        levelSizes_.clear();
        std::vector<int> cgrid;
        if (!HypreSolver::hierarchyLevelSizes(*solv, n_, levelSizes_, cgrid))
            levelSizes_.assign(1, n_);
        cgrid_ = cgrid;
        nlevels_ = static_cast<int>(levelSizes_.size());
        buildLevelOperators_();
        report_ = "Hypre BoomerAMG (IJ)\n";
        for (int l = 0; l < nlevels_; ++l) {
            report_ += "  level " + std::to_string(l) + ": " +
                       std::to_string(levelSizes_[static_cast<std::size_t>(l)]) +
                       " unknowns\n";
        }
        buildTimeSec_ += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - buildStart).count();
        hypreHierarchyReady_ = true;
        return true;
    }
    std::cerr << "  AMG: Hypre build failed (deferred hierarchy)\n";
#endif
    return false;
}

void AMGHierarchy::buildLevelOperators_() const {
    levels_.clear();
    levels_.resize(static_cast<std::size_t>(nlevels_));

    // Guard: cgrid_ must be populated by the Hypre hierarchy query before we
    // index into it. If it is empty there is nothing to build.
    if (static_cast<int>(cgrid_.size()) != n_) return;

    // Level 0 operator: row-major copy for efficient Gauss-Seidel row access.
    {
        Eigen::SparseMatrix<double, Eigen::RowMajor> arm0 = systemMatrix_;
        levels_[0].ARM = arm0;
        levels_[0].ARM.makeCompressed();
        levels_[0].nCoarse = 0; // updated below if there is a next level
    }

    // Build per-level P (prolongation, coarse->fine), R (restriction, fine->coarse),
    // ARM (coarse operator), cfSplit, nCoarse from cgrid_.
    for (int L = 0; L < nlevels_ - 1; ++L) {
        const int nFine = levelSizes_[static_cast<std::size_t>(L)];
        const int nCoarse = levelSizes_[static_cast<std::size_t>(L + 1)];

        // Local index mappings: original fine id -> local index at each level.
        std::vector<int> fineNodes, coarseNodes;
        std::vector<int> fineLocal(n_, -1), coarseLocal(n_, -1);
        for (int i = 0; i < n_; ++i) {
            if (cgrid_[static_cast<std::size_t>(i)] >= L) {
                fineLocal[static_cast<std::size_t>(i)] = static_cast<int>(fineNodes.size());
                fineNodes.push_back(i);
            }
            if (cgrid_[static_cast<std::size_t>(i)] >= L + 1) {
                coarseLocal[static_cast<std::size_t>(i)] = static_cast<int>(coarseNodes.size());
                coarseNodes.push_back(i);
            }
        }

        // cfSplit: 1 = C-node (survives to L+1), 0 = F-node (eliminated).
        auto& lev = levels_[static_cast<std::size_t>(L)];
        lev.cfSplit.assign(static_cast<std::size_t>(nFine), 0);
        lev.nCoarse = nCoarse;
        for (int i = 0; i < nFine; ++i) {
            if (cgrid_[static_cast<std::size_t>(fineNodes[static_cast<std::size_t>(i)])] >= L + 1) {
                lev.cfSplit[static_cast<std::size_t>(i)] = 1;
            }
        }

        // Strength of connection on level L matrix. strongNeighbors is keyed
        // by local level-L index (0..nFine-1), NOT by original node ID.
        const Eigen::SparseMatrix<double, Eigen::RowMajor>& A_L = lev.ARM;
        std::vector<std::vector<int>> strongNeighbors;
        strengthOfConnection(A_L, strongNeighbors);

        // Build P (prolongation, coarse->fine): nFine x nCoarse.
        // Fine node i that survives: P(i, i) = 1 (identity).
        // Fine node i eliminated: P(i, parent(i)) = 1 where parent is strongest coarse neighbor.
        std::vector<Eigen::Triplet<double>> triplets;
        triplets.reserve(static_cast<std::size_t>(nFine));
        for (int i = 0; i < nFine; ++i) {
            const int origId = fineNodes[static_cast<std::size_t>(i)];
            if (cgrid_[static_cast<std::size_t>(origId)] >= L + 1) {
                triplets.emplace_back(i, coarseLocal[static_cast<std::size_t>(origId)], 1.0);
            } else {
                int bestParent = -1;
                double bestVal = 0.0;
                for (int nbLocal : strongNeighbors[static_cast<std::size_t>(i)]) {
                    // nbLocal is a local level-L index; map back to original node ID.
                    const int nbOrig = fineNodes[static_cast<std::size_t>(nbLocal)];
                    if (cgrid_[static_cast<std::size_t>(nbOrig)] >= L + 1) {
                        const double val = std::abs(A_L.coeff(i, nbLocal));
                        if (val > bestVal) { bestVal = val; bestParent = nbOrig; }
                    }
                }
                if (bestParent >= 0) {
                    triplets.emplace_back(i, coarseLocal[static_cast<std::size_t>(bestParent)], 1.0);
                } else {
                    // Fallback: no strong coarse neighbor; map to coarse node 0.
                    triplets.emplace_back(i, nCoarse > 0 ? 0 : i, 1.0);
                }
            }
        }

        lev.P.resize(static_cast<Eigen::Index>(nFine), static_cast<Eigen::Index>(nCoarse));
        lev.P.setFromTriplets(triplets.begin(), triplets.end());
        lev.P.makeCompressed();

        // R = P^T: nCoarse x nFine (restriction, fine->coarse).
        lev.R = lev.P.transpose();
        lev.R.makeCompressed();

        // ARM[L+1] = R * A_L * P = P^T * A_L * P (Galerkin product).
        Eigen::SparseMatrix<double> APC = A_L * lev.P;
        levels_[static_cast<std::size_t>(L + 1)].ARM = lev.P.transpose() * APC;
        levels_[static_cast<std::size_t>(L + 1)].ARM.makeCompressed();
    }

    // Coarsest level has no next level.
    if (nlevels_ > 0) {
        levels_[static_cast<std::size_t>(nlevels_ - 1)].nCoarse = 0;
    }
}

bool AMGHierarchy::applyFlexibleCycle_(const Eigen::VectorXd& b, Eigen::VectorXd& x) const {
    if (!built_ || !useHypreActive_) return false;
    if (config_.cycleType != AmgSettings::CycleType::Flexible) return false;
    // Ensure the full hierarchy is available (covers deferred builds).
    if (!hypreHierarchyReady_ && !ensureHypreBuilt_()) return false;
    // Recursive flexible cycle: alpha/beta gates at every level.
    flexCycle(0, b, x);
    return x.allFinite();
}

Eigen::VectorXd AMGHierarchy::flexCycle(int level, const Eigen::VectorXd& b,
                                         Eigen::VectorXd& x) const {
    // Defensive: per-level operators must be built before accessing them.
    if (static_cast<std::size_t>(level) >= levels_.size())
        return Eigen::VectorXd();
    const auto& lev = levels_[static_cast<std::size_t>(level)];
    if (lev.ARM.rows() == 0 || lev.ARM.cols() == 0)
        return Eigen::VectorXd();

    const int n = static_cast<int>(levelSizes_[static_cast<std::size_t>(level)]);
    if (n == 0 || x.size() != static_cast<Eigen::Index>(n)) return Eigen::VectorXd();

    const Eigen::SparseMatrix<double, Eigen::RowMajor>& A =
        levels_[static_cast<std::size_t>(level)].ARM;
    const Eigen::VectorXd xStart = x;

    const double r0 = (b - A * x).norm();
    if (!std::isfinite(r0) || r0 == 0.0) return Eigen::VectorXd::Zero(n);

    const double alpha = config_.flexAlpha;
    const double beta = config_.flexBeta;
    // fine levels use flexMaxFineRelaxations, coarse levels use
    // flexMaxCoarseRelaxations; fall back to flexMaxSweeps if either is 0.
    const int maxSweeps =
        (level == 0 && config_.flexMaxFineRelaxations > 0)
            ? config_.flexMaxFineRelaxations
        : (level > 0 && config_.flexMaxCoarseRelaxations > 0)
            ? config_.flexMaxCoarseRelaxations
            : (config_.flexMaxSweeps > 0 ? config_.flexMaxSweeps : 1);

    double rPrev = r0;
    for (int sweep = 0; sweep < maxSweeps; ++sweep) {
        gaussSeidel(A, b, x, 1, true);
        const double res = (b - A * x).norm();
        if (!std::isfinite(res)) { x.setZero(); return Eigen::VectorXd::Zero(n); }

        // Alpha: residual reduced to alpha * r0 -> converged at this level.
        if (res < alpha * r0) return x - xStart;

        // Beta: residual not dropping -> go coarser.
        if (level < nlevels_ - 1) {
            const auto& nextLev = levels_[static_cast<std::size_t>(level + 1)];
            if (nextLev.ARM.rows() == 0 || lev.R.rows() == 0 || lev.P.cols() == 0)
                return x - xStart; // cannot coarsen: return partial correction

            const Eigen::VectorXd r = b - A * x;
            const Eigen::VectorXd rCoarse = lev.R * r;

            Eigen::VectorXd xCoarse =
                Eigen::VectorXd::Zero(levelSizes_[static_cast<std::size_t>(level + 1)]);
            const Eigen::VectorXd correctionCoarse =
                flexCycle(level + 1, rCoarse, xCoarse);

            x += lev.P * correctionCoarse;

            return x - xStart;
        }

        rPrev = res;
    }

    // Max sweeps reached without alpha/beta trigger: return partial correction.
    return x - xStart;
}

void AMGHierarchy::fineGaussSeidel_(const Eigen::VectorXd& b, Eigen::VectorXd& x,
                                    int sweeps) const {
    if (!fineRowsReady_) { fineRows_ = systemMatrix_; fineRowsReady_ = true; }
    gaussSeidel(fineRows_, b, x, sweeps, true);
}

bool AMGHierarchy::solveFlexible_(const Eigen::VectorXd& b, Eigen::VectorXd& x,
                                  const std::atomic<bool>* cancel) const {
    auto solveStart = std::chrono::steady_clock::now();
    const int n = n_;
    if ((int)x.size() != n) { x.resize(n); x.setZero(); }
    double bnorm = b.norm();
    if (!std::isfinite(bnorm) || bnorm < 1e-30) { x.setZero(); return true; }
    const int maxIt = std::max(1, config_.maxIterations);
    const double tol = std::max(1e-16, config_.tolerance);
    const double r0 = (b - systemMatrix_ * x).norm();
    double prev = std::numeric_limits<double>::max();
    lastResidualHistory_.clear();
    // STAR prints an "Iteration Residuals" table starting at cycle 0 (the
    // residual of the incoming guess); mirror that baseline.
    lastResidualHistory_.push_back(r0 / bnorm);
    lastIterations_ = 0; lastResidual_ = r0 / bnorm;
    for (int iter = 0; iter < maxIt; ++iter) {
        if (cancel && cancel->load()) return false;
        if (!applyFlexibleCycle_(b, x)) return false;
        Eigen::VectorXd r = b - systemMatrix_ * x;
        double res = r.norm();
        double rel = res / bnorm;
        lastResidualHistory_.push_back(rel);
        lastIterations_ = iter + 1; lastResidual_ = rel;
        if (rel < tol || (config_.epsilon > 0 && res < config_.epsilon)) {
            solveTimeSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now()-solveStart).count();
            return true;
        }
        if (res > prev * config_.stagnationFactor && iter >= config_.stagnationMinIter) {
            bool ok = rel < tol * config_.stagnationLooseFactor;
            solveTimeSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now()-solveStart).count();
            return ok;
        }
        prev = res;
    }
    Eigen::VectorXd r = b - systemMatrix_ * x;
    lastResidual_ = r.norm() / bnorm;
    solveTimeSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now()-solveStart).count();
    return lastResidual_ < tol * config_.stagnationLooseFactor;
}

bool AMGHierarchy::solve(const Eigen::VectorXd& b, Eigen::VectorXd& x, const std::atomic<bool>* cancel) const {
    if (!built_ || !useHypreActive_) return false;
    if (config_.cycleType == AmgSettings::CycleType::Flexible) return solveFlexible_(b, x, cancel);
    auto solveStart = std::chrono::steady_clock::now();
    const int n = n_;
    if ((int)x.size() != n) { x.resize(n); x.setZero(); }
    double bnorm = b.norm();
    if (!std::isfinite(bnorm) || bnorm < 1e-30) { x.setZero(); return true; }
    const int maxIt = std::max(1, config_.maxIterations);
    const double tol = std::max(1e-16, config_.tolerance);
    double prev = std::numeric_limits<double>::max();
    lastResidualHistory_.clear();
    // Cycle-0 baseline (see solveFlexible_).
    lastResidualHistory_.push_back((b - systemMatrix_ * x).norm() / bnorm);
    for (int iter = 0; iter < maxIt; ++iter) {
        if (cancel && cancel->load()) return false;
        if (!applyCycle(b, x)) return false;
        Eigen::VectorXd r = b - systemMatrix_ * x;
        double res = r.norm();
        double rel = res / bnorm;
        lastResidualHistory_.push_back(rel);
        lastIterations_ = iter + 1; lastResidual_ = rel;
        if (rel < tol || (config_.epsilon > 0 && res < config_.epsilon)) {
            solveTimeSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now()-solveStart).count();
            return true;
        }
        if (res > prev * config_.stagnationFactor && iter >= config_.stagnationMinIter) {
            bool ok = rel < tol * config_.stagnationLooseFactor;
            solveTimeSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now()-solveStart).count();
            return ok;
        }
        prev = res;
    }
    Eigen::VectorXd r = b - systemMatrix_ * x;
    lastResidual_ = r.norm() / bnorm;
    solveTimeSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now()-solveStart).count();
    return lastResidual_ < tol * config_.stagnationLooseFactor;
}

void AMGHierarchy::updateFineValues(const SparseMatrix& A) { build(A); }

void AMGHierarchy::strengthOfConnection(const Eigen::SparseMatrix<double, Eigen::RowMajor>& A,
                                         std::vector<std::vector<int>>& strongNeighbors) const {
    strongNeighbors.assign(static_cast<std::size_t>(A.rows()), {});
    const double thresh = config_.strengthThreshold;
    for (Eigen::Index i = 0; i < A.outerSize(); ++i) {
        double maxOff = 0.0;
        for (Eigen::SparseMatrix<double, Eigen::RowMajor>::InnerIterator it(A, i); it; ++it) {
            if (it.col() != i) maxOff = std::max(maxOff, std::abs(it.value()));
        }
        if (maxOff == 0.0) continue;
        const double limit = thresh * maxOff;
        for (Eigen::SparseMatrix<double, Eigen::RowMajor>::InnerIterator it(A, i); it; ++it) {
            if (it.col() != i && std::abs(it.value()) >= limit) {
                strongNeighbors[static_cast<std::size_t>(i)].push_back(static_cast<int>(it.col()));
            }
        }
    }
}

void AMGHierarchy::rsCoarsening(const std::vector<std::vector<int>>&, std::vector<int>&, int&) const {
    throw std::runtime_error("AMGHierarchy::rsCoarsening not implemented");
}
void AMGHierarchy::pmisCoarsening(const std::vector<std::vector<int>>&, std::vector<int>&, int&) const {
    throw std::runtime_error("AMGHierarchy::pmisCoarsening not implemented");
}
void AMGHierarchy::directInterpolation(const SparseMatrix&, const std::vector<std::vector<int>>&, const std::vector<int>&, int, SparseMatrix&) const {
    throw std::runtime_error("AMGHierarchy::directInterpolation not implemented");
}
void AMGHierarchy::extendedInterpolation(const SparseMatrix&, const std::vector<std::vector<int>>&, const std::vector<int>&, int, SparseMatrix&) const {
    throw std::runtime_error("AMGHierarchy::extendedInterpolation not implemented");
}
void AMGHierarchy::truncateInterpolation(SparseMatrix&) const {
    throw std::runtime_error("AMGHierarchy::truncateInterpolation not implemented");
}

SparseMatrix AMGHierarchy::galerkinProduct(const SparseMatrix& A, const SparseMatrix& P) const {
    // Compute R * A * P where R = P^T. A is ColMajor, P is ColMajor (n_fine x n_coarse).
    Eigen::SparseMatrix<double> APC = A * P;
    SparseMatrix result = P.transpose() * APC;
    result.prune(0.0);
    return result;
}

void AMGHierarchy::vCycle(int, const Eigen::VectorXd&, Eigen::VectorXd&) const {
    throw std::runtime_error("AMGHierarchy::vCycle not implemented");
}
void AMGHierarchy::fCycle(int, const Eigen::VectorXd&, Eigen::VectorXd&) const {
    throw std::runtime_error("AMGHierarchy::fCycle not implemented");
}
void AMGHierarchy::cycle(int, const Eigen::VectorXd&, Eigen::VectorXd&) const {
    throw std::runtime_error("AMGHierarchy::cycle not implemented");
}

void AMGHierarchy::gaussSeidel(const Eigen::SparseMatrix<double, Eigen::RowMajor>& ARM,
                               const Eigen::VectorXd& b, Eigen::VectorXd& x,
                               int numSweeps, bool forward) const {
    const int n = static_cast<int>(ARM.rows());
    for (int s = 0; s < numSweeps; ++s) {
        if (forward) {
            for (int i = 0; i < n; ++i) {
                double diag = 0.0, sum = 0.0;
                for (Eigen::SparseMatrix<double, Eigen::RowMajor>::InnerIterator it(ARM, i); it; ++it) {
                    if (it.col() == i) diag = it.value();
                    else sum += it.value() * x[it.col()];
                }
                if (std::abs(diag) < 1e-14 || !std::isfinite(diag)) continue;
                if (!std::isfinite(b[i]) || !std::isfinite(sum)) continue;
                x[i] = (b[i] - sum) / diag;
            }
        } else {
            for (int i = n - 1; i >= 0; --i) {
                double diag = 0.0, sum = 0.0;
                for (Eigen::SparseMatrix<double, Eigen::RowMajor>::InnerIterator it(ARM, i); it; ++it) {
                    if (it.col() == i) diag = it.value();
                    else sum += it.value() * x[it.col()];
                }
                if (std::abs(diag) < 1e-14 || !std::isfinite(diag)) continue;
                if (!std::isfinite(b[i]) || !std::isfinite(sum)) continue;
                x[i] = (b[i] - sum) / diag;
            }
        }
    }
}

void AMGHierarchy::smoothPre_(const Eigen::SparseMatrix<double, Eigen::RowMajor>& ARM,
                              const Eigen::VectorXd& b, Eigen::VectorXd& x, int numSweeps) const {
    gaussSeidel(ARM, b, x, numSweeps, true);
}

void AMGHierarchy::smoothPost_(const Eigen::SparseMatrix<double, Eigen::RowMajor>& ARM,
                               const Eigen::VectorXd& b, Eigen::VectorXd& x, int numSweeps) const {
    // For nonsymmetric operators, post-sweep is also forward so the composed
    // preconditioner remains a fixed linear operator.
    gaussSeidel(ARM, b, x, numSweeps, true);
}

} // namespace fvm
