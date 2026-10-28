#include "fvm/AMG.h"

#include <Eigen/Cholesky>
#include <Eigen/SparseLU>
#include <Eigen/LU>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <unordered_set>

namespace fvm {

using SparseMatrixRM = Eigen::SparseMatrix<double, Eigen::RowMajor>;

void AMGHierarchy::build(const SparseMatrix& A) {
    levels_.clear();
    if (A.rows() == 0 || A.cols() == 0) return;

    SparseMatrix currentA = A;

    for (int level = 0; level < config_.maxLevels; ++level) {
        const int n = static_cast<int>(currentA.rows());

        if (n <= config_.minCoarseSize) {
            AMGLevel coarseLevel;
            coarseLevel.ARM = currentA;
            levels_.push_back(std::move(coarseLevel));
            break;
        }

        std::vector<std::vector<int>> strongNeighbors;
        strengthOfConnection(currentA, strongNeighbors);

        std::vector<int> cfSplit;
        int nCoarse = 0;
        if (config_.coarseningType == CoarseningType::RS) {
            rsCoarsening(strongNeighbors, cfSplit, nCoarse);
        } else {
            pmisCoarsening(strongNeighbors, cfSplit, nCoarse);
        }

        if (nCoarse == 0 || nCoarse >= n || static_cast<double>(nCoarse) >= 0.9 * n) {
            AMGLevel coarseLevel;
            coarseLevel.ARM = currentA;
            levels_.push_back(std::move(coarseLevel));
            break;
        }

        SparseMatrix P;
        if (config_.interpolationType == InterpolationType::Extended) {
            extendedInterpolation(currentA, strongNeighbors, cfSplit, nCoarse, P);
        } else {
            directInterpolation(currentA, strongNeighbors, cfSplit, nCoarse, P);
        }

        truncateInterpolation(P);

        AMGLevel lvl;
        lvl.ARM = currentA;
        lvl.P = P;
        lvl.R = P.transpose();
        lvl.cfSplit = std::move(cfSplit);
        lvl.nCoarse = nCoarse;
        levels_.push_back(std::move(lvl));

        SparseMatrix Ac = galerkinProduct(currentA, P);
        currentA = Ac;
    }
}

void AMGHierarchy::strengthOfConnection(const SparseMatrix& A,
                                         std::vector<std::vector<int>>& strongNeighbors) const {
    const int n = static_cast<int>(A.rows());
    strongNeighbors.assign(n, {});

    const double theta = config_.strengthThreshold;
    // Use RowMajor view for correct row-wise strong connections (A is ColMajor)
    SparseMatrixRM AR = A;
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < n; ++i) {
        double maxOffDiag = 0.0;
        // First pass: find max without allocation
        for (SparseMatrixRM::InnerIterator it(AR, i); it; ++it) {
            if (static_cast<int>(it.col()) != i) {
                double absVal = std::abs(it.value());
                if (absVal > maxOffDiag) maxOffDiag = absVal;
            }
        }
        if (maxOffDiag == 0.0) continue;
        const double threshold = theta * maxOffDiag;
        // Use stack-local buffer and assign (not swap with thread_local) to avoid aliasing
        std::vector<int> strongBuf;
        strongBuf.reserve(8);
        for (SparseMatrixRM::InnerIterator it(AR, i); it; ++it) {
            int j = static_cast<int>(it.col());
            if (j == i) continue;
            if (std::abs(it.value()) >= threshold) strongBuf.push_back(j);
        }
        std::sort(strongBuf.begin(), strongBuf.end());
        strongNeighbors[i] = std::move(strongBuf);
    }
}

void AMGHierarchy::rsCoarsening(const std::vector<std::vector<int>>& strongNeighbors,
                                 std::vector<int>& cfSplit, int& nCoarse) const {
    const int n = static_cast<int>(strongNeighbors.size());
    cfSplit.assign(n, 0);

    std::vector<double> weights(n);
    std::mt19937 rng;
    if (config_.rngSeed == 0) {
        std::random_device rd;
        rng.seed(rd());
    } else {
        // Mix seed with problem size to escape pathological traps while keeping determinism
        std::seed_seq seq{static_cast<unsigned int>(config_.rngSeed),
                          static_cast<unsigned int>(n),
                          static_cast<unsigned int>(strongNeighbors.size())};
        rng.seed(seq);
    }
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    for (int i = 0; i < n; ++i) weights[i] = dist(rng);

    std::vector<int> state(n, 0);

    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < n; ++i) {
            if (state[i] == 1) continue;

            bool isMax = true;
            for (int j : strongNeighbors[i]) {
                if (j < 0 || j >= n) continue;
                if (weights[j] > weights[i]) {
                    isMax = false;
                    break;
                }
            }
            if (!isMax) continue;

            bool neighborIsMax = false;
            for (int j : strongNeighbors[i]) {
                if (j < 0 || j >= n) continue;
                if (weights[j] == weights[i] && j < i) {
                    neighborIsMax = true;
                    break;
                }
            }
            if (neighborIsMax) continue;

            cfSplit[i] = 1;
            state[i] = 1;
            for (int j : strongNeighbors[i]) {
                if (j >= 0 && j < n && state[j] == 0) {
                    state[j] = -1;
                }
            }
        }

        for (int i = 0; i < n; ++i) {
            if (state[i] == -1) state[i] = 0;
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < n; ++i) {
            if (cfSplit[i] == 1) continue;

            bool hasStrongC = false;
            for (int j : strongNeighbors[i]) {
                if (j >= 0 && j < n && cfSplit[j] == 1) {
                    hasStrongC = true;
                    break;
                }
            }
            if (!hasStrongC) {
                double bestWeight = -1.0;
                int bestNeighbor = -1;
                for (int j : strongNeighbors[i]) {
                    if (j >= 0 && j < n && weights[j] > bestWeight) {
                        bestWeight = weights[j];
                        bestNeighbor = j;
                    }
                }
                if (bestNeighbor >= 0 && cfSplit[bestNeighbor] == 0) {
                    cfSplit[bestNeighbor] = 1;
                    changed = true;
                } else {
                    cfSplit[i] = 1;
                    changed = true;
                }
            }
        }
    }

    nCoarse = 0;
    for (int i = 0; i < n; ++i) {
        if (cfSplit[i] == 1) ++nCoarse;
    }
}

void AMGHierarchy::pmisCoarsening(const std::vector<std::vector<int>>& strongNeighbors,
                                   std::vector<int>& cfSplit, int& nCoarse) const {
    const int n = static_cast<int>(strongNeighbors.size());
    cfSplit.assign(n, 0);

    std::vector<double> weights(n);
    std::mt19937 rng;
    if (config_.rngSeed == 0) {
        std::random_device rd;
        rng.seed(rd());
    } else {
        std::seed_seq seq{static_cast<unsigned int>(config_.rngSeed),
                          static_cast<unsigned int>(n * 1315423911u + 2654435761u),
                          static_cast<unsigned int>(strongNeighbors.size())};
        rng.seed(seq);
    }
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    for (int i = 0; i < n; ++i) weights[i] = dist(rng);

    std::vector<int> color(n, 0);

    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < n; ++i) {
            if (color[i] != 0) continue;

            bool isMax = true;
            for (int j : strongNeighbors[i]) {
                if (j < 0 || j >= n) continue;
                if (weights[j] > weights[i]) {
                    isMax = false;
                    break;
                }
            }
            if (!isMax) continue;

            bool neighborIsMax = false;
            for (int j : strongNeighbors[i]) {
                if (j < 0 || j >= n) continue;
                if (weights[j] == weights[i] && j < i) {
                    neighborIsMax = true;
                    break;
                }
            }
            if (neighborIsMax) continue;

            cfSplit[i] = 1;
            color[i] = 1;
            for (int j : strongNeighbors[i]) {
                if (j >= 0 && j < n && color[j] == 0) {
                    color[j] = -1;
                }
            }
        }

        for (int i = 0; i < n; ++i) {
            if (color[i] == -1) color[i] = 0;
        }
    }

    for (int i = 0; i < n; ++i) {
        if (cfSplit[i] == 1) continue;

        bool hasStrongC = false;
        for (int j : strongNeighbors[i]) {
            if (j >= 0 && j < n && cfSplit[j] == 1) {
                hasStrongC = true;
                break;
            }
        }
        if (!hasStrongC) {
            double bestWeight = -1.0;
            int bestNeighbor = -1;
            for (int j : strongNeighbors[i]) {
                if (j >= 0 && j < n && weights[j] > bestWeight) {
                    bestWeight = weights[j];
                    bestNeighbor = j;
                }
            }
            if (bestNeighbor >= 0) {
                cfSplit[bestNeighbor] = 1;
            } else {
                cfSplit[i] = 1;
            }
        }
    }

    nCoarse = 0;
    for (int i = 0; i < n; ++i) {
        if (cfSplit[i] == 1) ++nCoarse;
    }
}

void AMGHierarchy::directInterpolation(const SparseMatrix& A,
                                        const std::vector<std::vector<int>>& strongNeighbors,
                                        const std::vector<int>& cfSplit, int nCoarse,
                                        SparseMatrix& P) const {
    const int n = static_cast<int>(A.rows());
    P.resize(n, nCoarse);
    P.reserve(Eigen::VectorXi::Constant(n, 4));

    std::vector<int> coarseIndex(n, -1);
    int idx = 0;
    for (int i = 0; i < n; ++i) {
        if (cfSplit[i] == 1) {
            coarseIndex[i] = idx++;
        }
    }

    thread_local std::vector<Triplet> triplets;
    triplets.clear();
    triplets.reserve(n * 4);
    SparseMatrixRM AR = A;

    auto isStrong = [&](int i, int j) -> bool {
        const auto& s = strongNeighbors[i];
        return std::binary_search(s.begin(), s.end(), j);
    };

    for (int i = 0; i < n; ++i) {
        if (cfSplit[i] == 1) {
            triplets.emplace_back(i, coarseIndex[i], 1.0);
        } else {
            std::vector<int> cStrong;
            cStrong.reserve(8);
            double aii = 0.0;
            double sumWeak = 0.0;
            // Collect row values once to avoid second scan for aij
            std::vector<std::pair<int,double>> rowVals;
            rowVals.reserve(16);
            for (SparseMatrixRM::InnerIterator it(AR, i); it; ++it) {
                int j = static_cast<int>(it.col());
                double v = it.value();
                rowVals.emplace_back(j, v);
                if (j == i) aii = v;
                else if (cfSplit[j] == 1 && isStrong(i, j)) cStrong.push_back(j);
                else if (j != i && cfSplit[j] != 1) sumWeak += v;
            }

            if (cStrong.empty()) {
                // Try weak C neighbor fallback before arbitrary col 0
                int fallback = -1;
                double bestAbs = 0.0;
                for (auto& [j, v] : rowVals) {
                    if (j != i && cfSplit[j] == 1 && std::abs(v) > bestAbs) {
                        bestAbs = std::abs(v);
                        fallback = j;
                    }
                }
                if (fallback >= 0) triplets.emplace_back(i, coarseIndex[fallback], 1.0);
                else triplets.emplace_back(i, 0, 1.0);
            } else {
                double denom = aii;
                if (std::abs(sumWeak) > 1e-30) denom -= sumWeak;
                if (std::abs(denom) < 1e-30) {
                    int fallback = cStrong[0];
                    triplets.emplace_back(i, coarseIndex[fallback], 1.0);
                } else {
                    for (int j : cStrong) {
                        double aij = 0.0;
                        for (auto& [col, val] : rowVals) if (col == j) { aij = val; break; }
                        double val = -aij / denom;
                        if (std::abs(val) > 1e-15) triplets.emplace_back(i, coarseIndex[j], val);
                    }
                }
            }
        }
    }

    P.setFromTriplets(triplets.begin(), triplets.end());
    P.makeCompressed();
}

void AMGHierarchy::extendedInterpolation(const SparseMatrix& A,
                                          const std::vector<std::vector<int>>& strongNeighbors,
                                          const std::vector<int>& cfSplit, int nCoarse,
                                          SparseMatrix& P) const {
    const int n = static_cast<int>(A.rows());
    P.resize(n, nCoarse);
    P.reserve(Eigen::VectorXi::Constant(n, 8));

    std::vector<int> coarseIndex(n, -1);
    int idx = 0;
    for (int i = 0; i < n; ++i) if (cfSplit[i] == 1) coarseIndex[i] = idx++;

    thread_local std::vector<Triplet> triplets;
    triplets.clear();
    triplets.reserve(n * 8);
    SparseMatrixRM AR = A;

    auto isStrong = [&](int i, int j) -> bool {
        const auto& s = strongNeighbors[i];
        return std::binary_search(s.begin(), s.end(), j);
    };

    // Cache row values for fast aij lookup (degree small)
    std::vector<std::vector<std::pair<int,double>>> rowCache(n);
    // Lazy build: fill only when needed for i or k that is F, but we can build on demand
    // Pre-build for all rows that are F-adjacent to avoid repeated scans
    // Simple: build on the fly with thread-local cache; for now scan directly via AR

    for (int i = 0; i < n; ++i) {
        if (cfSplit[i] == 1) {
            triplets.emplace_back(i, coarseIndex[i], 1.0);
            continue;
        }

        // First scan of row i
        double aii = 0.0, sumWeak = 0.0;
        std::vector<int> cStrong; cStrong.reserve(8);
        std::vector<std::pair<int,double>> rowI;
        rowI.reserve(16);
        for (SparseMatrixRM::InnerIterator it(AR, i); it; ++it) {
            int j = static_cast<int>(it.col());
            double v = it.value();
            rowI.emplace_back(j, v);
            if (j == i) aii = v;
            else if (cfSplit[j] == 1 && isStrong(i, j)) cStrong.push_back(j);
            else if (j != i && cfSplit[j] != 1) sumWeak += v;
        }

        // Build extended set: distance-2 via strong F neighbors
        std::vector<int> cExtended; cExtended.reserve(8);
        // Deduplicate via small sort+unique rather than unordered_set
        std::vector<int> seenMark; seenMark.reserve(16);
        for (int k : strongNeighbors[i]) {
            if (k < 0 || k >= n || cfSplit[k] != 0) continue;
            double aik = 0.0;
            for (auto& [col, val] : rowI) if (col == k) { aik = val; break; }
            if (std::abs(aik) < 1e-30) continue;
            for (int c : strongNeighbors[k]) {
                if (c < 0 || c >= n || cfSplit[c] != 1) continue;
                if (isStrong(i, c)) continue;
                // Check sumAkc for row k
                double sumAkc = 0.0;
                for (SparseMatrixRM::InnerIterator it(AR, k); it; ++it) {
                    int j = static_cast<int>(it.col());
                    if (cfSplit[j] == 1 && isStrong(i, j)) sumAkc += it.value();
                }
                if (std::abs(sumAkc) > 1e-30) cExtended.push_back(c);
            }
        }
        if (!cExtended.empty()) {
            std::sort(cExtended.begin(), cExtended.end());
            cExtended.erase(std::unique(cExtended.begin(), cExtended.end()), cExtended.end());
        }

        if (cStrong.empty() && cExtended.empty()) {
            // Fallback to weak C or col 0
            int fallback = -1;
            double bestAbs = 0.0;
            for (auto& [j, v] : rowI) if (cfSplit[j] == 1 && std::abs(v) > bestAbs) { bestAbs = std::abs(v); fallback = j; }
            if (fallback >= 0) triplets.emplace_back(i, coarseIndex[fallback], 1.0);
            else triplets.emplace_back(i, 0, 1.0);
        } else {
            double denom = aii;
            if (std::abs(sumWeak) > 1e-30) denom -= sumWeak;
            if (std::abs(denom) < 1e-30) {
                int fb = cStrong.empty() ? cExtended[0] : cStrong[0];
                triplets.emplace_back(i, coarseIndex[fb], 1.0);
            } else {
                for (int j : cStrong) {
                    double aij = 0.0;
                    for (auto& [col, val] : rowI) if (col == j) { aij = val; break; }
                    double val = -aij / denom;
                    if (std::abs(val) > 1e-15) triplets.emplace_back(i, coarseIndex[j], val);
                }
                // Second pass: compute distance-2 contributions using cached rowI and rowK
                for (int c : cExtended) {
                    double aij_dist2 = 0.0;
                    for (int k : strongNeighbors[i]) {
                        if (k < 0 || k >= n || cfSplit[k] != 0) continue;
                        double aik = 0.0;
                        for (auto& [col, val] : rowI) if (col == k) { aik = val; break; }
                        if (std::abs(aik) < 1e-30) continue;
                        double akc = 0.0, sumAkc = 0.0;
                        for (SparseMatrixRM::InnerIterator it(AR, k); it; ++it) {
                            int j = static_cast<int>(it.col());
                            double v = it.value();
                            if (j == c) akc = v;
                            if (cfSplit[j] == 1 && isStrong(i, j)) sumAkc += v;
                        }
                        if (std::abs(sumAkc) > 1e-30) aij_dist2 += aik * akc / sumAkc;
                    }
                    double val = -aij_dist2 / denom;
                    if (std::abs(val) > 1e-15) triplets.emplace_back(i, coarseIndex[c], val);
                }
            }
        }
    }

    P.setFromTriplets(triplets.begin(), triplets.end());
    P.makeCompressed();
}

void AMGHierarchy::truncateInterpolation(SparseMatrix& P) const {
    const double eps = config_.truncThreshold;
    const bool doDrop = (eps > 0.0 && eps < 1.0);

    const int n = static_cast<int>(P.rows());
    // Row-major view for efficient row-wise truncation; move-assign back avoids extra copy
    SparseMatrixRM PRM = P;
    for (int i = 0; i < n; ++i) {
        double maxVal = 0.0;
        for (SparseMatrixRM::InnerIterator it(PRM, i); it; ++it) maxVal = std::max(maxVal, std::abs(it.value()));
        if (maxVal < 1e-30) continue;
        double threshold = doDrop ? (eps * maxVal) : 0.0;
        double rowSum = 0.0;
        int count = 0;
        for (SparseMatrixRM::InnerIterator it(PRM, i); it; ++it) {
            double v = it.value();
            if (std::abs(v) >= threshold) { rowSum += v; ++count; }
        }
        // Always enforce partition-of-unity (row sum = 1) so interpolation
        // reproduces the constant null space of the Poisson operator. Hypre's
        // classical interpolation is constructed to satisfy P*1 = 1; without this
        // normalization the coarse grid degrades for constant fields.
        if (count > 0 && std::abs(rowSum) > 1e-30) {
            double scale = 1.0 / rowSum;
            for (SparseMatrixRM::InnerIterator it(PRM, i); it; ++it) {
                if (std::abs(it.value()) < threshold) it.valueRef() = 0.0;
                else it.valueRef() *= scale;
            }
        }
    }
    PRM.prune(0.0);
    PRM.makeCompressed();
    P = std::move(PRM);
}

void AMGHierarchy::updateFineValues(const SparseMatrix& A) {
    // Coarse levels depend on A through the Galerkin product, so a value-only
    // update of the fine level is unsound. Rebuild the full hierarchy so that
    // strength/interpolation and the Galerkin coarse operator stay consistent
    // with the current matrix values (e.g. momentum matrices in transient SIMPLE).
    build(A);
}

SparseMatrix AMGHierarchy::galerkinProduct(const SparseMatrix& A, const SparseMatrix& P) const {
    SparseMatrix AP = A * P;
    SparseMatrix Ac = P.transpose() * AP;
    if (config_.galerkinDropTol > 0.0) {
        Ac.prune(config_.galerkinDropTol);
    }
    Ac.makeCompressed();
    return Ac;
}

void AMGHierarchy::gaussSeidel(const SparseMatrixRM& ARM, const Eigen::VectorXd& b,
                                Eigen::VectorXd& x, int numSweeps, bool forward) const {
    const int n = static_cast<int>(ARM.rows());
    // Cache diagonal presence and value to avoid a per-sweep scan.
    std::vector<char> hasDiag(n, 0);
    std::vector<double> diagVal(n, 0.0);
    for (int i = 0; i < n; ++i) {
        for (SparseMatrixRM::InnerIterator it(ARM, i); it; ++it) {
            if (static_cast<int>(it.col()) == i) {
                hasDiag[i] = 1;
                diagVal[i] = it.value();
                break;
            }
        }
    }

    // Clamp omega into a range that keeps the standalone smoother convergent.
    double omega = config_.smootherOmega;
    if (!(omega > 0.0) || omega > 1.0) omega = 1.0;

    for (int sweep = 0; sweep < numSweeps; ++sweep) {
        for (int k = 0; k < n; ++k) {
            const int i = forward ? k : (n - 1 - k);
            double diag = diagVal[i];
            if (!hasDiag[i] || std::abs(diag) < 1e-30) {
                // Structural hole (e.g. a Dirichlet reference cell). Rescan,
                // then fall back to a damped Jacobi step on that row so the
                // sweep stays convergent.
                diag = 0.0;
                for (SparseMatrixRM::InnerIterator it(ARM, i); it; ++it) {
                    if (static_cast<int>(it.col()) == i) { diag = it.value(); break; }
                }
                if (std::abs(diag) < 1e-30) {
                    x(i) = 0.5 * x(i) + 0.5 * b(i);
                    continue;
                }
                diagVal[i] = diag;
                hasDiag[i] = 1;
            }
            double sigma = 0.0;
            for (SparseMatrixRM::InnerIterator it(ARM, i); it; ++it) {
                int j = static_cast<int>(it.col());
                if (j != i) sigma += it.value() * x(j);
            }
            const double gs = (b(i) - sigma) / diag;
            // omega == 1 -> plain Gauss-Seidel
            x(i) = (omega == 1.0) ? gs : x(i) + omega * (gs - x(i));
        }
    }
}

void AMGHierarchy::smoothPre_(const SparseMatrixRM& ARM, const Eigen::VectorXd& b,
                              Eigen::VectorXd& x, int numSweeps) const {
    if (numSweeps <= 0) return;
    switch (config_.smootherType) {
        case SmootherType::ForwardGaussSeidel:
        case SmootherType::ForwardSOR:
            // Forward only: the sweep order defines the iteration for a
            // nonsymmetric operator, so a forward pre-sweep is paired with a
            // forward post-sweep to keep a fixed linear preconditioner.
            gaussSeidel(ARM, b, x, numSweeps, true);
            return;
        case SmootherType::SymmetricGaussSeidel:
        default:
            gaussSeidel(ARM, b, x, numSweeps, true);
            gaussSeidel(ARM, b, x, numSweeps, false);
            return;
    }
}

void AMGHierarchy::smoothPost_(const SparseMatrixRM& ARM, const Eigen::VectorXd& b,
                               Eigen::VectorXd& x, int numSweeps) const {
    if (numSweeps <= 0) return;
    switch (config_.smootherType) {
        case SmootherType::ForwardGaussSeidel:
        case SmootherType::ForwardSOR:
            gaussSeidel(ARM, b, x, numSweeps, true);
            return;
        case SmootherType::SymmetricGaussSeidel:
        default:
            gaussSeidel(ARM, b, x, numSweeps, false);
            return;
    }
}


void AMGHierarchy::vCycle(int level, const Eigen::VectorXd& b, Eigen::VectorXd& x) const {
    const AMGLevel& lvl = levels_[level];
    const int n = static_cast<int>(lvl.ARM.rows());

    if (level == static_cast<int>(levels_.size()) - 1) {
        const SparseMatrixRM& Ac = lvl.ARM;
        const int nc = static_cast<int>(Ac.rows());

        if (nc <= 30) {
            Eigen::MatrixXd denseA = Eigen::MatrixXd::Zero(nc, nc);
            for (int i = 0; i < nc; ++i) {
                for (SparseMatrixRM::InnerIterator it(Ac, i); it; ++it) {
                    denseA(i, static_cast<int>(it.col())) = it.value();
                }
            }
            Eigen::VectorXd bc(nc);
            for (int i = 0; i < nc; ++i) bc(i) = b(i);
            // Use FullPivLU for robustness on small coarse (handles non-SPD)
            x = denseA.fullPivLu().solve(bc);
            if (!x.allFinite()) x = denseA.ldlt().solve(bc);
        } else {
            bool isSymmetric = false;
            if (Ac.rows() == Ac.cols()) {
                SparseMatrixRM At = Ac.transpose();
                At = At - Ac;
                double nrm = Ac.norm();
                isSymmetric = (nrm < 1e-30) || (At.norm() < 1e-12 * nrm);
            }
            if (isSymmetric) {
                Eigen::SimplicialLDLT<SparseMatrixRM> ldl;
                ldl.compute(Ac);
                if (ldl.info() == Eigen::Success && ldl.vectorD().minCoeff() > 0) {
                    Eigen::VectorXd xt = ldl.solve(b);
                    double bnorm = b.norm();
                    double rnorm = (Ac * xt - b).norm();
                    // Tight verification: require true SPD solve, not loose 1e6
                    if (xt.allFinite() && rnorm < 100.0 * config_.tolerance * (bnorm + 1.0)) { x = xt; return; }
                }
            }
            Eigen::SparseLU<SparseMatrixRM> lu;
            lu.compute(Ac);
            if (lu.info() == Eigen::Success) x = lu.solve(b);
            else x = b;
        }
        return;
    }

    smoothPre_(lvl.ARM, b, x, config_.preSweeps);

    Eigen::VectorXd residual = b - lvl.ARM * x;
    Eigen::VectorXd rCoarse = lvl.R * residual;

    Eigen::VectorXd eCoarse = Eigen::VectorXd::Zero(rCoarse.size());
    vCycle(level + 1, rCoarse, eCoarse);

    x += lvl.P * eCoarse;

    smoothPost_(lvl.ARM, b, x, config_.postSweeps);
}

void AMGHierarchy::fCycle(int level, const Eigen::VectorXd& b, Eigen::VectorXd& x) const {
    const AMGLevel& lvl = levels_[level];

    if (level == static_cast<int>(levels_.size()) - 1) {
        const SparseMatrixRM& Ac = lvl.ARM;
        const int nc = static_cast<int>(Ac.rows());

        if (nc <= 30) {
            Eigen::MatrixXd denseA = Eigen::MatrixXd::Zero(nc, nc);
            for (int i = 0; i < nc; ++i) {
                for (SparseMatrixRM::InnerIterator it(Ac, i); it; ++it) {
                    denseA(i, static_cast<int>(it.col())) = it.value();
                }
            }
            Eigen::VectorXd bc(nc);
            for (int i = 0; i < nc; ++i) bc(i) = b(i);
            x = denseA.fullPivLu().solve(bc);
            if (!x.allFinite()) x = denseA.ldlt().solve(bc);
        } else {
            bool isSymmetric = false;
            if (Ac.rows() == Ac.cols()) {
                SparseMatrixRM At = Ac.transpose();
                At = At - Ac;
                double nrm = Ac.norm();
                isSymmetric = (nrm < 1e-30) || (At.norm() < 1e-12 * nrm);
            }
            if (isSymmetric) {
                Eigen::SimplicialLDLT<SparseMatrixRM> ldl;
                ldl.compute(Ac);
                if (ldl.info() == Eigen::Success && ldl.vectorD().minCoeff() > 0) {
                    Eigen::VectorXd xt = ldl.solve(b);
                    double bnorm = b.norm();
                    double rnorm = (Ac * xt - b).norm();
                    if (xt.allFinite() && rnorm < 100.0 * config_.tolerance * (bnorm + 1.0)) { x = xt; return; }
                }
            }
            Eigen::SparseLU<SparseMatrixRM> lu;
            lu.compute(Ac);
            if (lu.info() == Eigen::Success) x = lu.solve(b);
            else x = b;
        }
        return;
    }

    smoothPre_(lvl.ARM, b, x, config_.preSweeps);

    Eigen::VectorXd residual = b - lvl.ARM * x;
    Eigen::VectorXd rCoarse = lvl.R * residual;

    Eigen::VectorXd eCoarse = Eigen::VectorXd::Zero(rCoarse.size());
    fCycle(level + 1, rCoarse, eCoarse);

    x += lvl.P * eCoarse;

    smoothPost_(lvl.ARM, b, x, config_.postSweeps);

    // Second stage is intentionally V-cycle (standard F-cycle = F + V), not recursive F.
    // Changing this to fCycle would make cost O(2^levels) instead of O(levels).
    residual = b - lvl.ARM * x;
    rCoarse = lvl.R * residual;
    eCoarse.setZero();
    vCycle(level + 1, rCoarse, eCoarse);

    x += lvl.P * eCoarse;

    smoothPost_(lvl.ARM, b, x, config_.postSweeps);
}

void AMGHierarchy::cycle(int level, const Eigen::VectorXd& b, Eigen::VectorXd& x) const {
    if (config_.cycleType == CycleType::F) {
        fCycle(level, b, x);
    } else {
        vCycle(level, b, x);
    }
}

void AMGHierarchy::applyCycle(const Eigen::VectorXd& b, Eigen::VectorXd& x) const {
    if (levels_.empty()) { x = b; return; }
    // x is the caller's current guess and is NOT discarded: the cycle is a
    // stationary iteration on top of it, so calling applyCycle repeatedly
    // applies multiple cycles (AMGPreconditioner::setCycles).
    cycle(0, b, x);
}

bool AMGHierarchy::solve(const Eigen::VectorXd& b, Eigen::VectorXd& x) const {
    if (levels_.empty()) return false;

    const int n = static_cast<int>(levels_[0].ARM.rows());
    if (x.size() != n) {
        x.resize(n);
        x.setZero();
    } else if (!x.allFinite()) {
        x.setZero();
    }

    double bnorm = b.norm();
    if (bnorm < 1e-30) {
        x.setZero();
        return true;
    }

    double prevResidual = std::numeric_limits<double>::max();

    for (int iter = 0; iter < config_.maxIterations; ++iter) {
        cycle(0, b, x);

        double residual = (b - levels_[0].ARM * x).norm();
        double relResidual = residual / bnorm;

        if (relResidual < config_.tolerance) {
            return true;
        }

        if (residual > prevResidual * config_.stagnationFactor && iter >= config_.stagnationMinIter) {
            return relResidual < config_.tolerance * config_.stagnationLooseFactor;
        }
        prevResidual = residual;
    }

    return (b - levels_[0].ARM * x).norm() / bnorm < config_.tolerance * config_.stagnationLooseFactor;
}

} // namespace fvm
