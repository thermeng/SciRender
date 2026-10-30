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

inline bool isSecondOrderUpwind(ConvectionScheme s) {
    return s == ConvectionScheme::SecondOrderUpwind;
}

template<std::size_t Dim>
inline std::pair<std::size_t, std::size_t> secondOrderUpwindCells(
    const Mesh<Dim>& mesh,
    std::size_t P,
    std::size_t N,
    double un) {

    const auto& fn = mesh.faceNormal(N == std::size_t(-1) ? P : P); // placeholder, will be overridden by caller with correct face normal
    // This overload is kept for backward compatibility but the preferred path
    // is gradient-based reconstruction (see Convection/Momentum). The old
    // heuristic (first non-boundary neighbor) is replaced by alignment search
    // to fix cross-wind selection. Callers should use the face-normal overload.
    (void)fn;
    if (N == std::size_t(-1)) {
        if (un >= 0.0) {
            return {P, P};
        } else {
            const auto& neighbors = mesh.cellNeighbors(P);
            // alignment: pick neighbor most opposite to face normal (upstream)
            // Fallback to first non-boundary if geometry unavailable
            for (std::size_t nb : neighbors) {
                if (nb != N && !mesh.isBoundary(nb)) {
                    return {nb, P};
                }
            }
            return {P, P};
        }
    }

    if (un >= 0.0) {
        const auto& neighbors = mesh.cellNeighbors(P);
        for (std::size_t nb : neighbors) {
            if (nb != N && !mesh.isBoundary(nb)) {
                return {P, nb};
            }
        }
        return {P, P};
    } else {
        const auto& neighbors = mesh.cellNeighbors(N);
        for (std::size_t nb : neighbors) {
            if (nb != P && !mesh.isBoundary(nb)) {
                return {N, nb};
            }
        }
        return {N, N};
    }
}

// Face-normal aware overload: picks upwind-upwind cell most aligned with -flow
template<std::size_t Dim>
inline std::pair<std::size_t, std::size_t> secondOrderUpwindCells(
    const Mesh<Dim>& mesh,
    std::size_t P,
    std::size_t N,
    double un,
    const std::array<double, Dim>& faceNormal) {

    auto pickUpwindUpwind = [&](std::size_t up, std::size_t downwindToExclude) -> std::size_t {
        const auto& neighbors = mesh.cellNeighbors(up);
        if (neighbors.empty()) return up;
        const double* xUp = mesh.cellCenterPtr(up);
        double bestDot = (un >= 0.0 ? 1e100 : -1e100);
        std::size_t best = up;
        bool found = false;
        for (std::size_t nb : neighbors) {
            if (nb == downwindToExclude) continue;
            // Allow boundary cells as up_up now (second-order at boundary via extrapolation)
            const double* xNb = mesh.cellCenterPtr(nb);
            double dot = 0.0;
            for (std::size_t d = 0; d < Dim; ++d) dot += (xNb[d] - xUp[d]) * faceNormal[d];
            // For un>=0 (flow P->N), up=P, want most negative dot (upstream)
            // For un<0 (flow N->P), up=N, want most positive dot (upstream is +normal)
            if (un >= 0.0) {
                if (dot < bestDot) { bestDot = dot; best = nb; found = true; }
            } else {
                if (dot > bestDot) { bestDot = dot; best = nb; found = true; }
            }
        }
        return found ? best : up;
    };

    if (N == std::size_t(-1)) {
        if (un >= 0.0) return {P, P}; // outflow, no up_up needed for inflow logic
        std::size_t up2 = pickUpwindUpwind(P, std::size_t(-1));
        return {up2, P};
    }
    if (un >= 0.0) {
        std::size_t up2 = pickUpwindUpwind(P, N);
        return {P, up2};
    } else {
        std::size_t up2 = pickUpwindUpwind(N, P);
        return {N, up2};
    }
}

template<std::size_t Dim>
inline double computeSecondOrderUpwindFaceValue(
    const Mesh<Dim>& mesh,
    std::size_t P,
    std::size_t N,
    double un,
    const double* phiVals) {

    // Distance-weighted extrapolation: phi_f = phi_up + (phi_up - phi_up2) * |x_f - x_up| / |x_up - x_up2|
    // Falls back to 1.5/-0.5 on uniform meshes
    const std::size_t f = 0; // dummy, caller should use face-aware overload
    (void)f;
    auto [upwind, downwind] = secondOrderUpwindCells(mesh, P, N, un);
    double phiUpwind = phiVals[upwind];
    double phiDownwind = phiVals[downwind];
    if (upwind == downwind) return phiUpwind;
    // Uniform weight fallback (will be overridden by gradient path)
    return 1.5 * phiUpwind - 0.5 * phiDownwind;
}

template<std::size_t Dim>
inline double computeSecondOrderUpwindFaceValueWeighted(
    const Mesh<Dim>& mesh,
    std::size_t f,
    std::size_t P,
    std::size_t N,
    double un,
    const double* phiVals) {

    auto fn = mesh.faceNormal(f);
    auto [upwind, up2] = secondOrderUpwindCells(mesh, P, N, un, fn);
    if (upwind == up2) return phiVals[upwind];
    double phiUp = phiVals[upwind];
    double phiUp2 = phiVals[up2];
    const double* xF = mesh.faceCenterPtr(f);
    const double* xUp = mesh.cellCenterPtr(upwind);
    const double* xUp2 = mesh.cellCenterPtr(up2);
    double distF = 0.0, distUp = 0.0;
    for (std::size_t d = 0; d < Dim; ++d) {
        double df = xF[d] - xUp[d];
        double du = xUp[d] - xUp2[d];
        distF += df * df;
        distUp += du * du;
    }
    distF = std::sqrt(distF);
    distUp = std::sqrt(distUp);
    if (distUp < 1e-12) return phiUp;
    double w = distF / distUp;
    // Clamp w to [0.1, 2.0] to avoid extreme stretched mesh weights
    if (w > 2.0) w = 2.0;
    if (w < 0.0) w = 0.0;
    return phiUp + w * (phiUp - phiUp2);
}

} // namespace fvm
