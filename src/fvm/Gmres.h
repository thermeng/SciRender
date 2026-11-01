#pragma once

#include <Eigen/Core>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace fvm {

struct GmresResult {
    bool converged = false;
    int iterations = 0;
    // Final relative residual |b - A x| / |b|.
    double residual = 0.0;
};

// Restarted, right-preconditioned GMRES(m).
//
// Eigen's IterativeLinearSolvers module ships CG, BiCGSTAB and GMRES only in
// the *unsupported* module, and the vendored copy here excludes it. 
// exposes GMRES as an AMG acceleration option, so a compact implementation
// lives here instead of pulling in another dependency.
//
// Right preconditioning solves A M^-1 y = b and recovers x = M^-1 y, which
// keeps the residual that GMRES minimises equal to the true residual.
//
// applyA(x)  -> A * x
// applyM(x)  -> M^-1 * x   (one AMG cycle, for our use)
template <typename ApplyA, typename ApplyM>
GmresResult gmresRestarted(const Eigen::VectorXd& b,
                           Eigen::VectorXd& x,
                           ApplyA&& applyA,
                           ApplyM&& applyM,
                           double relTol,
                           int maxIter,
                           int restart = 30,
                           const std::atomic<bool>* cancel = nullptr) {
    GmresResult result;

    const int n = static_cast<int>(b.size());
    const double bnorm = b.norm();
    const double scale = (bnorm > 0.0) ? bnorm : 1.0;
    const double tol = std::max(1e-30, relTol);

    Eigen::VectorXd r = b - applyA(x);
    if (!r.allFinite()) {
        x.setZero();
        return result;
    }
    double rnorm = r.norm();
    result.residual = rnorm / scale;
    if (result.residual <= tol) {
        result.converged = true;
        return result;
    }

    const int m = std::max(1, restart);
    std::vector<Eigen::VectorXd> V(static_cast<std::size_t>(m) + 1,
                                   Eigen::VectorXd::Zero(n));
    std::vector<Eigen::VectorXd> Z(static_cast<std::size_t>(m),
                                   Eigen::VectorXd::Zero(n));
    std::vector<std::vector<double>> H(static_cast<std::size_t>(m) + 1,
                                       std::vector<double>(static_cast<std::size_t>(m), 0.0));
    std::vector<double> cs(static_cast<std::size_t>(m), 0.0);
    std::vector<double> sn(static_cast<std::size_t>(m), 0.0);
    std::vector<double> g(static_cast<std::size_t>(m) + 1, 0.0);
    std::vector<double> y(static_cast<std::size_t>(m), 0.0);

    while (result.iterations < maxIter) {
        V[0] = r / rnorm;
        std::fill(g.begin(), g.end(), 0.0);
        g[0] = rnorm;
        for (auto& row : H) std::fill(row.begin(), row.end(), 0.0);

        // Number of Krylov directions stored in this restart cycle.
        int used = 0;
        for (int j = 0; j < m && result.iterations < maxIter; ++j) {
            if (cancel && cancel->load()) {
                x.setZero();
                return result;
            }

            Z[static_cast<std::size_t>(j)] = applyM(V[static_cast<std::size_t>(j)]);
            if (!Z[static_cast<std::size_t>(j)].allFinite())
                Z[static_cast<std::size_t>(j)].setZero();

            Eigen::VectorXd w = applyA(Z[static_cast<std::size_t>(j)]);
            if (!w.allFinite()) break;

            // Modified Gram-Schmidt, with a second pass for stability.
            for (int i = 0; i <= j; ++i) {
                H[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] =
                    w.dot(V[static_cast<std::size_t>(i)]);
                w -= H[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] *
                     V[static_cast<std::size_t>(i)];
            }
            for (int i = 0; i <= j; ++i) {
                const double corr = w.dot(V[static_cast<std::size_t>(i)]);
                H[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] += corr;
                w -= corr * V[static_cast<std::size_t>(i)];
            }
            const double wnorm = w.norm();
            H[static_cast<std::size_t>(j) + 1][static_cast<std::size_t>(j)] = wnorm;
            if (wnorm > 0.0)
                V[static_cast<std::size_t>(j) + 1] = w / wnorm;

            // Apply the rotations from previous steps to this column.
            for (int i = 0; i < j; ++i) {
                const double h_i = H[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
                const double h_i1 = H[static_cast<std::size_t>(i) + 1][static_cast<std::size_t>(j)];
                H[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] =
                    cs[static_cast<std::size_t>(i)] * h_i + sn[static_cast<std::size_t>(i)] * h_i1;
                H[static_cast<std::size_t>(i) + 1][static_cast<std::size_t>(j)] =
                    -sn[static_cast<std::size_t>(i)] * h_i + cs[static_cast<std::size_t>(i)] * h_i1;
            }

            // New rotation that zeroes H[j+1][j].
            const double a = H[static_cast<std::size_t>(j)][static_cast<std::size_t>(j)];
            const double c = H[static_cast<std::size_t>(j) + 1][static_cast<std::size_t>(j)];
            const double len = std::hypot(a, c);
            if (len > 0.0) {
                cs[static_cast<std::size_t>(j)] = a / len;
                sn[static_cast<std::size_t>(j)] = c / len;
            } else {
                cs[static_cast<std::size_t>(j)] = 1.0;
                sn[static_cast<std::size_t>(j)] = 0.0;
            }
            H[static_cast<std::size_t>(j)][static_cast<std::size_t>(j)] = len;
            H[static_cast<std::size_t>(j) + 1][static_cast<std::size_t>(j)] = 0.0;
            g[static_cast<std::size_t>(j) + 1] =
                -sn[static_cast<std::size_t>(j)] * g[static_cast<std::size_t>(j)];
            g[static_cast<std::size_t>(j)] =
                cs[static_cast<std::size_t>(j)] * g[static_cast<std::size_t>(j)];

            used = j + 1;
            ++result.iterations;
            result.residual = std::abs(g[static_cast<std::size_t>(j) + 1]) / scale;
            if (result.residual <= tol) break;
            if (wnorm == 0.0) break;   // lucky breakdown: invariant subspace
        }

        if (used == 0) break;

        // Back substitution on the upper triangular factor of H.
        for (int i = used - 1; i >= 0; --i) {
            double sum = g[static_cast<std::size_t>(i)];
            for (int k = i + 1; k < used; ++k) {
                sum -= H[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)] *
                       y[static_cast<std::size_t>(k)];
            }
            const double diag = H[static_cast<std::size_t>(i)][static_cast<std::size_t>(i)];
            y[static_cast<std::size_t>(i)] = (std::abs(diag) > 0.0) ? sum / diag : 0.0;
        }
        for (int i = 0; i < used; ++i) {
            if (y[static_cast<std::size_t>(i)] != 0.0)
                x += y[static_cast<std::size_t>(i)] * Z[static_cast<std::size_t>(i)];
        }

        // Recompute the true residual: the Givens estimate drifts from it,
        // and this is also the value the caller reports.
        r = b - applyA(x);
        if (!r.allFinite()) {
            x.setZero();
            return result;
        }
        rnorm = r.norm();
        result.residual = rnorm / scale;
        if (result.residual <= tol) {
            result.converged = true;
            return result;
        }
    }

    result.converged = result.residual <= tol;
    return result;
}

} // namespace fvm
