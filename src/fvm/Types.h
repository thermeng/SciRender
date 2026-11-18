#pragma once

#include <Eigen/Sparse>
#include <cmath>
#include <algorithm>
#include <cstddef>
#include <utility>

namespace fvm {

template<std::size_t Dim> class Mesh;


using SparseMatrix = Eigen::SparseMatrix<double>;
using Triplet = Eigen::Triplet<double>;

enum class ConvectionScheme { Upwind, Central, SecondOrderUpwind };
enum class TimeScheme { Euler, BDF2 };

// Coefficients of the implicit time term
//
//   c0 * phi^{n+1}  -  cOld * phi^n  -  cOldPrev * phi^{n-1}   (diagonal / LHS)
//   with (cOld*phi^n + cOldPrev*phi^{n-1}) moved to the right-hand side.
//
// For a uniform step this is the classic (3/2, -2, +1/2)/dt BDF2 pair.
// For a varying step the coefficients MUST change: reusing the uniform ones
// silently drops the scheme to roughly first order, and for a growing step
// it is outright unstable. So we derive them from dt/dtPrev and fall back to
// explicit Euler whenever no previous step is available.
struct ImplicitTimeCoeffs {
    double c0 = 1.0;       // multiplies phi^{n+1} on the diagonal
    double cOld = 1.0;     // multiplies phi^n      on the RHS
    double cOldPrev = 0.0; // multiplies phi^{n-1}   on the RHS
    bool secondOrder = false;
};

inline ImplicitTimeCoeffs implicitTimeCoeffs(double dt, double dtPrev) {
    ImplicitTimeCoeffs c;
    if (!(dt > 0.0) || !std::isfinite(dt)) return c;          // caller falls back to Euler
    if (!(dtPrev > 0.0) || !std::isfinite(dtPrev)) return c;   // first step -> Euler

    const double r = dt / dtPrev;
    if (std::abs(r - 1.0) <= 1e-12) {
        c.c0 = 1.5 / dt;
        c.cOld = 2.0 / dt;
        c.cOldPrev = -0.5 / dt;
    } else {
        // Variable-step BDF2 (r = dt_n / dt_{n-1}):
        //   (1/dt_n) [ (1+2r)/(1+r) u^{n+1} - (1+r) u^n + r^2/(1+r) u^{n-1} ] = du/dt
        const double onePlusR = 1.0 + r;
        c.c0 = (1.0 + 2.0 * r) / (onePlusR * dt);
        c.cOld = onePlusR / dt;
        c.cOldPrev = -(r * r) / (onePlusR * dt);
    }
    c.secondOrder = true;
    return c;
}

namespace detail {

// FNV-1a hash of the sparse-matrix sparsity pattern (row/col indices only,
// values excluded). Used to decide whether a preconditioner's pattern analysis
// can be reused across solve calls when only matrix values change.
inline std::size_t sparsityHash(const SparseMatrix& A) {
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

} // namespace detail

} // namespace fvm
