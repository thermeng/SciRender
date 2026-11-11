#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/FaceState.h"
#include "fvm/Gradient.h"
#include "fvm/SIMPLETypes.h"
#include <Eigen/Dense>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace fvm {

// u = u* - d * grad(p').
//
// The gradient here IS the operator whose divergence the pressure matrix is
// assembled from (the per-cell least-squares gradient, see Gradient.h): the
// pressure equation solves div(d grad p') = div(u*), so the correction below
// cancels the divergence it was built for EXACTLY - on every cell, not only
// in total. A different gradient would make the two operators disagree where
// p' has a local feature, which is where the mass balance then leaks.
template<std::size_t Dim>
inline void correctVelocity(
    const Mesh<Dim>& mesh,
    const VectorField<Dim>& uStar,
    const Field<Dim>& pPrime,
    const std::array<std::vector<double>, Dim>& dCoeff,
    const std::vector<std::vector<char>>& dirichletMask,
    const std::vector<std::vector<double>>& dirichletVals,
    std::array<Field<Dim>, Dim>& dpdx,
    VectorField<Dim>& velocity,
    const std::vector<FaceVelocityBC>* faceBC = nullptr,
    double maxUnlimitedSpeed = 20.0,   // growth limiting only applies above this speed
    double maxGrowthRate = 0.15,       // max per-iteration growth rate (e.g. 0.15 = 15%)
    double alphaP = 1.0,               // pressure under-relaxation applied to velocity correction
    Gradient<Dim>* reusableGradient = nullptr   // caller-owned LS workspace
) {

    // The gradient of p' is computed directly into dpdx, the buffer the update
    // below consumes. `reusableGradient` lets the solver reuse its least-squares
    // workspace (geometry matrix, scratch) across correctors; without one a
    // fresh Gradient is used, which allocates nothing until compute() is called.
    Gradient<Dim> localGradient(mesh, GradientScheme::LeastSquares);
    Gradient<Dim>& gradient = reusableGradient ? *reusableGradient : localGradient;
    for (auto& g : dpdx)
        if (g.size() != mesh.nCells()) g = Field<Dim>(mesh, 0.0);
    gradient.compute(pPrime, dpdx);

    for (std::size_t d = 0; d < Dim; ++d) {
        #pragma omp parallel for schedule(static)
        for (std::size_t i = 0; i < mesh.nCells(); ++i) {
            if (dirichletMask[d][i]) velocity[d][i] = dirichletVals[d][i];
            else velocity[d][i] = uStar[d][i] - dCoeff[d][i] * dpdx[d][i];
        }
    }

    // Velocity amplitude damping: cap per-iteration growth rate, but only when
    // the cell speed exceeds maxUnlimitedSpeed. Below this threshold no damping
    // is applied. Growth rate is always limited (reduction rate is unlimited).
    // Dirichlet cells keep their prescribed velocity exactly and are not damped.
    if (maxGrowthRate >= 0.0) {
        #pragma omp parallel for schedule(static)
        for (std::size_t i = 0; i < mesh.nCells(); ++i) {
            // Skip cells with any prescribed velocity component.
            bool isDirichlet = false;
            for (std::size_t d = 0; d < Dim; ++d)
                if (dirichletMask[d][i]) { isDirichlet = true; break; }
            if (isDirichlet) continue;

            double newSpeed2 = 0.0, oldSpeed2 = 0.0;
            for (std::size_t d = 0; d < Dim; ++d) {
                newSpeed2 += velocity[d][i] * velocity[d][i];
                oldSpeed2  += uStar[d][i] * uStar[d][i];
            }
            double newSpeed = std::sqrt(newSpeed2);
            double oldSpeed = std::sqrt(oldSpeed2);

            if (oldSpeed > maxUnlimitedSpeed && oldSpeed > 0.0) {
                double maxAllowedSpeed = oldSpeed * (1.0 + maxGrowthRate);
                if (newSpeed > maxAllowedSpeed) {
                    double scale = maxAllowedSpeed / newSpeed;
                    for (std::size_t d = 0; d < Dim; ++d)
                        velocity[d][i] *= scale;
                }
            }
            // No limit on reduction rate.
        }
    }

    if (faceBC) {
        for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
            if (mesh.neighbor(f) != std::size_t(-1)) continue;
            if (f >= faceBC->size()) continue;
            if ((*faceBC)[f].type == VelocityBCType::NoSlip) continue;
            auto t = (*faceBC)[f].type;
            if (isSlipVelocityBC(t)) {
                std::size_t P = mesh.owner(f);
                const auto& n = mesh.faceNormal(f);
                // Same no-penetration projection the momentum assembly and the
                // pressure correction use for a slip face.
                double un = 0.0;
                for (std::size_t d = 0; d < Dim; ++d) un += velocity[d][P] * n[d];
                for (std::size_t d = 0; d < Dim; ++d) velocity[d][P] -= un * n[d];
            }
        }
    }
}

} // namespace fvm
